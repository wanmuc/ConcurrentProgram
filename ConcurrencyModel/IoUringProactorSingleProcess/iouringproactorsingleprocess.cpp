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
  IoURing::Request *request = IoURing::NewRequest(sock_fd, IoURing::ACCEPT, 0);
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
      if (request->event == IoURing::ACCEPT) {
        if (cqe->res > 0) {
          // 新的客户端连接，发起异步读
          int client_fd = cqe->res;
          IoURing::Request read_request = IoURing::NewRequest(client_fd, IoURing::READ, 1024);
          IoURing::AddReadEvent(&ring, read_request);
        }
        // 继续等待新的客户端连接
        IoURing::AddAcceptEvent(&ring, request);
      } else if (request->event == IoURing::READ) {
        ret = cqe->res;
        if (ret <= 0) {  // 客户端关闭连接
          close(result.fd);
        } else if (ret > 0) {
          IoURing::Request write_request = IoURing::NewRequest(client_fd, IoURing::WRITE, ret);
          // 拷贝要写的数据
          memcpy(write_request->buffer, request->buffer, ret);
          IoURing::AddWriteEvent(&ring, write_request);
        }
        DeleteRequest(request);
      } else if (request->event == IoURing::WRITE) {
        ret = cqe->res;
        if (ret < 0) {
          close(result.fd);
        } else {
          if (ret <) {
          }
        }
        DeleteRequest(request);
      }
    }
    io_uring_cq_advance(&ring, count);
  }

  io_uring_queue_exit(&ring);
  return 0;
}