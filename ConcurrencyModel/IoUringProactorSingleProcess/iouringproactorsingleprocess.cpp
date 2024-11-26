#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <functional>

#include "common/cmdline.h"
#include "common/iouringctl.hpp"
#include "common/utils.hpp"

using namespace std;
using namespace MyEcho;

void usage() {
  cout << "IoUringProactorSingleProcess -ip 0.0.0.0 -port 1688 -io_uring_flag 1" << endl;
  cout << "options:" << endl;
  cout << "    -h,--help      print usage" << endl;
  cout << "    -ip,--ip       listen ip" << endl;
  cout << "    -port,--port   listen port" << endl;
  cout << "    -io_uring_flag,--io_uring_flag   1 is defualt, 2 is SQPOLL" << endl;
  cout << endl;
}

void ReleaseConn(IoURing::Request *request) {
  cout << "close client connection fd = " << request->conn.Fd() << endl;
  close(request->conn.Fd());
  delete request;
}

void OnAcceptEvent(struct io_uring &ring, IoURing::Request *request,
                   struct io_uring_cqe *cqe) {
  if (cqe->res > 0) {
    // 新的客户端连接，发起异步读
    int client_fd = cqe->res;
    IoURing::Request *read_request =
        IoURing::NewRequest(client_fd, IoURing::READ);
    IoURing::AddReadEvent(&ring, read_request);
  }
  // 继续等待新的客户端连接
  IoURing::AddAcceptEvent(&ring, request);
}

void OnReadEvent(struct io_uring &ring, IoURing::Request *request,
                 struct io_uring_cqe *cqe) {
  int32_t ret = cqe->res;
  if (ret <= 0) { // 客户端关闭连接或者读失败
    ReleaseConn(request);
    return;
  }
  // 执行到这里就是读取成功
  request->conn.ReadCallBack(ret);
  // 如果得到一个完整的请求，则写应答数据
  if (request->conn.OneMessage()) {
    request->conn.EnCode();                 // 应答数据序列化
    IoURing::AddWriteEvent(&ring, request); // 发起异步写
  } else {
    IoURing::AddReadEvent(&ring, request); // 否则继续发起异步读
  }
}

void OnWriteEvent(struct io_uring &ring, IoURing::Request *request,
                  struct io_uring_cqe *cqe) {
  int32_t ret = cqe->res;
  if (ret < 0) {  // 写失败
    ReleaseConn(request);
    return;
  }
  // 执行到这里就是写成功
  request->conn.WriteCallBack(ret);
  if (request->conn.FinishWrite()) {
    request->conn.Reset();                 // 连接重置
    IoURing::AddReadEvent(&ring, request); // 重新发起异步读，处理新的请求
  } else {
    IoURing::AddWriteEvent(&ring, request); // 否则继续发起异步写
  }
}

int main(int argc, char *argv[]) {
  string ip;
  int64_t port;
  int64_t io_uring_flag;
  CmdLine::StrOptRequired(&ip, "ip");
  CmdLine::Int64OptRequired(&port, "port");
  CmdLine::Int64OptRequired(&io_uring_flag, "io_uring_flag");
  CmdLine::SetUsage(usage);
  CmdLine::Parse(argc, argv);
  int sock_fd = CreateListenSocket(ip, port, false);
  if (sock_fd < 0) {
    return -1;
  }

  unsigned flag = 0;
  if (io_uring_flag == 2) {
    flag = IORING_SETUP_SQPOLL;
  }
  struct io_uring ring;
  uint32_t entries = 1024;
  int ret = io_uring_queue_init(entries, &ring, flag);
  if (ret < 0) {
    errno = -ret;
    perror("io_uring_queue_init failed");
    return ret;
  }
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
  delete conn_request;
  return 0;
}