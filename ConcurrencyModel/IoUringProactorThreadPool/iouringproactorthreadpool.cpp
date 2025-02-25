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
#include <thread>

#include "common/cmdline.h"
#include "common/iouringctl.hpp"
#include "common/utils.hpp"

using namespace std;
using namespace MyEcho;

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
  // 如果得到一个完整的请求，则写应答数据
  if (request->conn.OneMessage()) {
    request->conn.EnCode();                  // 应答数据序列化
    IoURing::AddWriteEvent(&ring, request);  // 发起异步写
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

void handler(string ip, int64_t port) {
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
  cout << "IoUringProactorThreadPool -ip 0.0.0.0 -port 1688 -poolsize 8" << endl;
  cout << "options:" << endl;
  cout << "    -h,--help      print usage" << endl;
  cout << "    -ip,--ip       listen ip" << endl;
  cout << "    -port,--port   listen port" << endl;
  cout << "    -poolsize,--poolsize   pool size" << endl;
  cout << endl;
}

int main(int argc, char *argv[]) {
  string ip;
  int64_t port;
  int64_t pool_size;
  CmdLine::StrOptRequired(&ip, "ip");
  CmdLine::Int64OptRequired(&port, "port");
  CmdLine::Int64OptRequired(&pool_size, "poolsize");
  CmdLine::SetUsage(usage);
  CmdLine::Parse(argc, argv);
  pool_size = pool_size > GetNProcs() ? GetNProcs() : pool_size;
  for (int i = 0; i < pool_size; i++) {
    std::thread(handler, ip, port).detach();  // 这里需要调用detach，让创建的线程独立运行
  }
  while (true) sleep(1);  // 主线程陷入死循环
  return 0;
}