#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>

#include "Coroutine/sync/channel.h"
#include "Coroutine/mycoroutine.h"
#include "common/cmdline.h"
#include "common/epollctl.hpp"

using namespace std;
using namespace MyEcho;

void EchoDeal(const std::string req_message, std::string &resp_message) { resp_message = req_message; }

void Producer(MyCoroutine::Channel<EventData> &channel, EventData *event_data) {
  ClearEvent(event_data->epoll_fd, event_data->fd, false);
  channel.Send(event_data);
}

void Consumer(MyCoroutine::Schedule &schedule, MyCoroutine::Channel<EventData> &channel) {
  EventData *event_data = channel.Receive();
  // 注意，这里需要更新cid，之前的cid是Producer协程的id，需要更新成Consumer的协程id
  event_data->cid = schedule.CurrentCid();
  auto releaseConn = [&event_data]() {
    ClearEvent(event_data->epoll_fd, event_data->fd);
    delete event_data;  // 释放内存
  };
  AddReadEvent(event_data->epoll_fd, event_data->fd, event_data);  // 重新监听可读事件，不带one_shot标记
  while (true) {
    ssize_t ret = 0;
    Codec codec;
    string *req_message{nullptr};
    string resp_message;
    while (true) {  // 读操作
      ret = read(event_data->fd, codec.Data(), codec.Len());
      if (ret == 0) {
        perror("peer close connection");
        releaseConn();
        return;
      }
      if (ret < 0) {
        if (EINTR == errno) continue;  // 被中断，可以重启读操作
        if (EAGAIN == errno or EWOULDBLOCK == errno) {
          schedule.CoroutineYield();  // 让出cpu，切换到主协程，等待下一次数据可读
          continue;
        }
        perror("read failed");
        releaseConn();
        return;
      }
      codec.DeCode(ret);  // 解析请求数据
      req_message = codec.GetMessage();
      if (req_message) {  // 解析出一个完整的请求
        break;
      }
    }
    // 执行到这里说明已经读取到一个完整的请求
    EchoDeal(*req_message, resp_message);  // 业务handler的封装，这样协程的调用就对业务逻辑函数EchoDeal透明
    delete req_message;
    Packet pkt;
    codec.EnCode(resp_message, pkt);
    ModToWriteEvent(event_data->epoll_fd, event_data->fd, event_data);  // 监听可写事件。
    size_t sendLen = 0;
    while (sendLen != pkt.UseLen()) {  // 写操作
      ret = write(event_data->fd, pkt.Data() + sendLen, pkt.UseLen() - sendLen);
      if (ret < 0) {
        if (EINTR == errno) continue;  // 被中断，可以重启写操作
        if (EAGAIN == errno or EWOULDBLOCK == errno) {
          schedule.CoroutineYield();  // 让出cpu，切换到主协程，等待下一次数据可写
          continue;
        }
        perror("write failed");
        releaseConn();
        return;
      }
      sendLen += ret;
    }
    ModToReadEvent(event_data->epoll_fd, event_data->fd, event_data);  // 监听可读事件。
  }
}

void usage() {
  cout << "EpollReactorSingleProcessCoroutineHSHA1 -ip 0.0.0.0 -port 1688" << endl;
  cout << "options:" << endl;
  cout << "    -h,--help      print usage" << endl;
  cout << "    -ip,--ip       listen ip" << endl;
  cout << "    -port,--port   listen port" << endl;
  cout << endl;
}

int main(int argc, char *argv[]) {
  string ip;
  int64_t port;
  CmdLine::StrOptRequired(&ip, "ip");
  CmdLine::Int64OptRequired(&port, "port");
  CmdLine::SetUsage(usage);
  CmdLine::Parse(argc, argv);
  int sock_fd = CreateListenSocket(ip, port, false);
  if (sock_fd < 0) {
    return -1;
  }
  epoll_event events[2048];
  int epoll_fd = epoll_create(1);
  if (epoll_fd < 0) {
    perror("epoll_create failed");
    return -1;
  }
  EventData event_data(sock_fd, epoll_fd);
  SetNotBlock(sock_fd);
  AddReadEvent(epoll_fd, sock_fd, &event_data);
  MyCoroutine::Schedule schedule(5000);                     // 协程池初始化
  MyCoroutine::Channel<EventData> channel(schedule, 3000);  // channel初始化，分配3000的缓存
  for (int i = 0; i < 3000; i++) {
    int cid = schedule.CoroutineCreate(Consumer, ref(schedule), ref(channel));
    schedule.CoroutineResume(cid);
  }

  int msec = -1;
  while (true) {
    int num = epoll_wait(epoll_fd, events, 2048, msec);
    if (num < 0) {
      perror("epoll_wait failed");
      continue;
    } else if (num == 0) {  // 没有事件了，下次调用epoll_wait大概率被挂起
      sleep(0);  // 这里直接sleep(0)让出cpu，大概率被挂起，这里主动让出cpu，可以减少一次epoll_wait的调用
      msec = -1;  // 大概率被挂起，故这里超时时间设置为-1
      continue;
    }
    msec = 0;  // 下次大概率还有事件，故msec设置为0
    for (int i = 0; i < num; i++) {
      EventData *event_data = (EventData *)events[i].data.ptr;
      if (event_data->fd == sock_fd) {
        LoopAccept(sock_fd, 2048, [epoll_fd](int client_fd) {
          EventData *event_data = new EventData(client_fd, epoll_fd);
          SetNotBlock(client_fd);
          AddReadEvent(epoll_fd, client_fd, event_data, true);  // 监听可读事件，设置one_shot标记
        });
        continue;
      }
      if (event_data->cid == MyCoroutine::kInvalidCid) {  // 第一次事件，则创建协程
        event_data->cid = schedule.CoroutineCreate(Producer, ref(channel), event_data);  // 创建协程
        schedule.CoroutineResume(event_data->cid);
      } else {
        schedule.CoroutineResume(event_data->cid);  // 唤醒之前主动让出cpu的协程
      }
    }
    schedule.CoSemaphoreResume();
  }
  return 0;
}