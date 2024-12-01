#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

#include "common/cmdline.h"
#include "common/iouringctl.hpp"
#include "common/utils.hpp"

using namespace std;
using namespace MyEcho;

std::mutex Mutex;
std::condition_variable Cond;
std::queue<IoURing::Request *> Queue;

void pushToQueue(IoURing::Request *request) {
  {
    std::unique_lock<std::mutex> locker(Mutex);
    Queue.push(request);
  }
  Cond.notify_one();
}

IoURing::Request *getQueueData() {
  std::unique_lock<std::mutex> locker(Mutex);
  Cond.wait(locker, []() -> bool { return Queue.size() > 0; });
  IoURing::Request *request = Queue.front();
  Queue.pop();
  return request;
}

void ReleaseConn(IoURing::Request *request) {
  errno = -request->cqe_res;  // io_uring返回的cqe中真正错误码的值，需要再取反。
  perror("close client");
  close(request->conn.Fd());
  IoURing::DeleteRequest(request);
}

void OnAcceptEvent(struct io_uring &ring, IoURing::Request *request, struct io_uring_cqe *cqe) {
  if (cqe->res > 0) {
    // 新的客户端连接，发起异步读
    int client_fd = cqe->res;
    IoURing::Request *client_request = IoURing::NewRequest(client_fd, IoURing::READ);
    IoURing::AddReadEvent(&ring, client_request);
  }
  // 继续等待新的客户端连接
  IoURing::AddAcceptEvent(&ring, request);
}

void OnReadEvent(struct io_uring &ring, IoURing::Request *request, struct io_uring_cqe *cqe) {
  int32_t ret = cqe->res;
  if (ret <= 0) {  // 客户端关闭连接或者读失败
    ReleaseConn(request);
    return;
  }
  // 执行到这里就是读取成功
  request->conn.ReadCallBack(ret);
  // 如果得到一个完整的请求，则通过队列传递给 worker 线程
  if (request->conn.OneMessage()) {
    pushToQueue(request);
  } else {
    IoURing::AddReadEvent(&ring, request);  // 否则继续发起异步读
  }
}

void OnWriteEvent(struct io_uring &ring, IoURing::Request *request, struct io_uring_cqe *cqe) {
  int32_t ret = cqe->res;
  if (ret < 0) {  // 写失败
    ReleaseConn(request);
    return;
  }
  // 执行到这里就是写成功
  request->conn.WriteCallBack(ret);
  if (request->conn.FinishWrite()) {
    request->conn.Reset();                  // 连接重置
    IoURing::AddReadEvent(&ring, request);  // 重新发起异步读，处理新的请求
  } else {
    IoURing::AddWriteEvent(&ring, request);  // 否则继续发起异步写
  }
}

void workerHandler() {
  while (true) {
    IoURing::Request *request = getQueueData();
    request->conn.EnCode();                          // 应答数据序列化
    IoURing::AddWriteEvent(request->ring, request);  // 发起异步写，io_uring 是线程安全的。
  }
}

void ioHandler(string ip, int64_t port) {
  int sock_fd = CreateListenSocket(ip, port, true);
  assert(sock_fd >= 0);
  struct io_uring ring;
  uint32_t entries = 1024;
  int ret = io_uring_queue_init(entries, &ring, IORING_SETUP_SQPOLL);
  assert(0 == ret);
  IoURing::Request *conn_request = IoURing::NewRequest(sock_fd, IoURing::ACCEPT);
  IoURing::AddAcceptEvent(&ring, conn_request);
  while (true) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      errno = -ret;
      perror("io_uring_wait_cqe faild");
      continue;
    }
    struct io_uring_cqe *cqes[1024];
    int count = io_uring_peek_batch_cqe(&ring, cqes, 1024);
    for (int i = 0; i < count; i++) {
      cqe = cqes[i];
      IoURing::Request *request = (IoURing::Request *)io_uring_cqe_get_data(cqe);
      if (request->event == IoURing::ACCEPT) {
        OnAcceptEvent(ring, request, cqe);
      } else if (request->event == IoURing::READ) {
        OnReadEvent(ring, request, cqe);
      } else if (request->event == IoURing::WRITE) {
        OnWriteEvent(ring, request, cqe);
      }
    }
    io_uring_cq_advance(&ring, count);
  }
  io_uring_queue_exit(&ring);
  IoURing::DeleteRequest(conn_request);
}

void usage() {
  cout << "IoUringProactorThreadPoolHSHA1 -ip 0.0.0.0 -port 1688 -io 3 -worker 8" << endl;
  cout << "options:" << endl;
  cout << "    -h,--help      print usage" << endl;
  cout << "    -ip,--ip       listen ip" << endl;
  cout << "    -port,--port   listen port" << endl;
  cout << "    -io,--io       io thread count" << endl;
  cout << "    -worker,--worker   worker thread count" << endl;
  cout << endl;
}

int main(int argc, char *argv[]) {
  string ip;
  int64_t port;
  int64_t io_count;
  int64_t worker_count;
  CmdLine::StrOptRequired(&ip, "ip");
  CmdLine::Int64OptRequired(&port, "port");
  CmdLine::Int64OptRequired(&io_count, "io");
  CmdLine::Int64OptRequired(&worker_count, "worker");
  CmdLine::SetUsage(usage);
  CmdLine::Parse(argc, argv);

  io_count = io_count > GetNProcs() ? GetNProcs() : io_count;
  worker_count = worker_count > GetNProcs() ? GetNProcs() : worker_count;

  for (int i = 0; i < worker_count; i++) {  // 创建worker线程
    std::thread(workerHandler).detach();    // 这里需要调用detach，让创建的线程独立运行
  }
  for (int i = 0; i < io_count; i++) {          // 创建io线程
    std::thread(ioHandler, ip, port).detach();  // 这里需要调用detach，让创建的线程独立运行
  }
  while (true) sleep(1);  // 主线程陷入死循环

  return 0;
}