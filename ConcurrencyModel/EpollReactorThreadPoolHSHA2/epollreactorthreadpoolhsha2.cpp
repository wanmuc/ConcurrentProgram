#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

#include "../../common/cmdline.h"
#include "../../common/conn.hpp"
#include "../../common/epollctl.hpp"

using namespace std;
using namespace MyEcho;

std::mutex Mutex;
std::queue<Conn *> Queue;

sem_t Idle; // 计数信号量，表示缓冲区空闲的数据
sem_t Fill;  // 计数信号量，表示缓冲区填充的数据

void pushToQueue(Conn *conn) {
  // 等待空缓冲区
  sem_wait(&Idle);
  {
    std::lock_guard<std::mutex> locker(Mutex);
    Queue.push(conn);
  }
  // 增加满缓冲区的数量
  sem_post(&Fill);
}

Conn *getQueueData() {
  Conn *conn = nullptr;
  // 等待满缓冲区
  sem_wait(&Fill);
  {
    std::lock_guard<std::mutex> locker(Mutex);
    conn = Queue.front();
    Queue.pop();
  }
  // 增加空缓冲区的数量
  sem_post(&Idle);
  return conn;
}

void workerHandler(bool is_direct) {
  while (true) {
    Conn *conn = getQueueData();
    conn->EnCode();
    if (is_direct) {  // 直接把数据发送给客户端，而不是通过I/O线程来发送
      bool success = true;
      while (not conn->FinishWrite()) {
        if (not conn->Write()) {
          success = false;
          break;
        }
      }
      if (not success) {
        ClearEvent(conn);
        delete conn;
      } else {
        conn->Reset();
        ReStartReadEvent(conn);  // 修改成只监控可读事件，携带oneshot选项
      }
    } else {
      ModToWriteEvent(conn);  // 监听写事件，数据通过I/O线程来发送
    }
  }
}

void ioHandler(string ip, int64_t port) {
  int sock_fd = CreateListenSocket(ip, port, true);
  if (sock_fd < 0) {
    return;
  }
  epoll_event events[2048];
  int epoll_fd = epoll_create(1);
  if (epoll_fd < 0) {
    perror("epoll_create failed");
    return;
  }
  Conn conn(sock_fd, epoll_fd, true);
  SetNotBlock(sock_fd);
  AddReadEvent(&conn);
  int msec = -1;
  while (true) {
    int num = epoll_wait(epoll_fd, events, 2048, msec);
    if (num < 0) {
      perror("epoll_wait failed");
      continue;
    }
    for (int i = 0; i < num; i++) {
      Conn *conn = (Conn *)events[i].data.ptr;
      if (conn->Fd() == sock_fd) {
        LoopAccept(sock_fd, 2048, [epoll_fd](int client_fd) {
          Conn *conn = new Conn(client_fd, epoll_fd, true);
          SetNotBlock(client_fd);
          AddReadEvent(conn, false, true);  // 监听可读事件，开启oneshot
        });
        continue;
      }
      auto releaseConn = [&conn]() {
        ClearEvent(conn);
        delete conn;
      };
      if (events[i].events & EPOLLIN) {  // 可读
        if (not conn->Read()) {  // 执行非阻塞read
          releaseConn();
          continue;
        }
        if (conn->OneMessage()) {
          pushToQueue(conn);  // 入共享输入队列，有锁
        } else {
          ReStartReadEvent(conn);  // 还没收到完整的请求，则重新启动可读事件的监听，携带oneshot选项
        }
      }
      if (events[i].events & EPOLLOUT) {  // 可写
        if (not conn->Write()) {  // 执行非阻塞write
          releaseConn();
          continue;
        }
        if (conn->FinishWrite()) {  // 完成了请求的应答写，则可以释放连接close
          conn->Reset();
          ReStartReadEvent(conn);  // 修改成只监控可读事件，携带oneshot选项
        }
      }
    }
  }
}

void usage() {
  cout << "EpollReactorThreadPoolHSHA2 -ip 0.0.0.0 -port 1688 -io 3 -worker 8 -queue_buffer_size 50000 -direct" << endl;
  cout << "options:" << endl;
  cout << "    -h,--help      print usage" << endl;
  cout << "    -ip,--ip       listen ip" << endl;
  cout << "    -port,--port   listen port" << endl;
  cout << "    -io,--io       io thread count" << endl;
  cout << "    -worker,--worker   worker thread count" << endl;
  cout << "    -queue_buffer_size,--queue_buffer_size  queue buffer size" << endl;
  cout << "    -direct,--direct   direct send response data by worker thread" << endl;
  cout << endl;
}

int main(int argc, char *argv[]) {
  string ip;
  int64_t port;
  int64_t io_count;
  int64_t worker_count;
  int64_t queue_buffer_size;
  bool is_direct;
  CmdLine::StrOptRequired(&ip, "ip");
  CmdLine::Int64OptRequired(&port, "port");
  CmdLine::Int64OptRequired(&io_count, "io");
  CmdLine::Int64OptRequired(&worker_count, "worker");
  CmdLine::Int64OptRequired(&queue_buffer_size, "queue_buffer_size");
  CmdLine::BoolOpt(&is_direct, "direct");
  CmdLine::SetUsage(usage);
  CmdLine::Parse(argc, argv);
  cout << "is_direct=" << is_direct << endl;
  io_count = io_count > GetNProcs() ? GetNProcs() : io_count;
  worker_count = worker_count > GetNProcs() ? GetNProcs() : worker_count;
  // 初始化信号量
  sem_init(&Idle, 0, queue_buffer_size); // 初始空缓冲区大小
  sem_init(&Fill, 0, 0);      // 初始满缓冲区大小
  for (int i = 0; i < worker_count; i++) {  // 创建worker线程
    std::thread(workerHandler, is_direct).detach();  // 这里需要调用detach，让创建的线程独立运行
  }
  for (int i = 0; i < io_count; i++) {  // 创建io线程
    std::thread(ioHandler, ip, port).detach();  // 这里需要调用detach，让创建的线程独立运行
  }
  while (true) sleep(1);  // 主线程陷入死循环
  return 0;
}