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

#include "common/cmdline.h"
#include "common/iouringctl.hpp"
#include "common/utils.hpp"

using namespace std;
using namespace MyEcho;

void usage() {
  cout << "IoUringProactorSingleProcess -ip 0.0.0.0 -port 1688" << endl;
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

  struct io_uring ring;
  uint32_t entries = 1024;
  int ret = io_uring_queue_init(entries, &ring, 0);
  if (ret < 0) {
    errno = -ret;
    perror("io_uring_queue_init failed");
    return ret;
  }
  IoURing::Request *request = IoURing::NewRequest(sock_fd, IoURing::ACCEPT);
  IoURing::AddAcceptEvent(&ring, request);
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
      request = (IoURing::Request *)io_uring_cqe_get_data(cqe);
      auto releaseConn = [](IoURing::Request *request) {
        close(request->conn.Fd());
        delete request;
      };
      if (request->event == IoURing::ACCEPT) {
        if (cqe->res > 0) {
          // 新的客户端连接，发起异步读
          int client_fd = cqe->res;
          IoURing::Request *read_request = IoURing::NewRequest(client_fd, IoURing::READ);
          IoURing::AddReadEvent(&ring, read_request);
        }
        // 继续等待新的客户端连接
        IoURing::AddAcceptEvent(&ring, request);
      } else if (request->event == IoURing::READ) {
        ret = cqe->res;
        if (ret <= 0) {  // 客户端关闭连接
          releaseConn(request);
          continue;
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
      } else if (request->event == IoURing::WRITE) {
        ret = cqe->res;
        if (ret < 0) {
          releaseConn(request);
          continue;
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
    }
    io_uring_cq_advance(&ring, count);
  }

  io_uring_queue_exit(&ring);
  return 0;
}