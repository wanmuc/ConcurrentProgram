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
#include "Coroutine/mycoroutine.h"

using namespace std;
using namespace MyEcho;

void usage() {
  cout << "IoUringProactorSingleProcessCoroutine -ip 0.0.0.0 -port 1688" << endl;
  cout << "options:" << endl;
  cout << "    -h,--help      print usage" << endl;
  cout << "    -ip,--ip       listen ip" << endl;
  cout << "    -port,--port   listen port" << endl;
  cout << endl;
}

void ReleaseConn(IoURing::Request *request) {
  cout << "close client connection fd = " << request->conn.Fd() << endl;
  close(request->conn.Fd());
  IoURing::DeleteRequest(request);
}

void HandlerClient(MyCoroutine::Schedule& schedule, struct io_uring& ring, IoURing::Request *request) {
  while (true) {
    while (not request->conn.OneMessage()) {
      IoURing::AddReadEvent(&ring, request); // 发起异步读
      schedule.CoroutineYield();  // 让出cpu，切换到主协程，等待读结果回调，唤醒
      if (request->cqe_res <= 0) { // 客户端关闭连接或者读失败
        ReleaseConn(request);
        return;
      }
      // 执行到这里就是读取成功
      request->conn.ReadCallBack(request->cqe_res);
    }
    // 执行到这里说明已经读取到一个完整的请求
    request->conn.EnCode(); // 应答数据序列化
    while (not request->conn.FinishWrite()) {
      IoURing::AddWriteEvent(&ring, request); // 发起异步写
      schedule.CoroutineYield();  // 让出cpu，切换到主协程，等待写结果回调，唤醒
      if (request->cqe_res < 0) { // 写失败
        ReleaseConn(request);
        return;
      }
      // 执行到这里就是写成功
      request->conn.WriteCallBack(request->cqe_res);
    }
    request->conn.Reset(); // 连接重置
  }
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
  int ret = io_uring_queue_init(entries, &ring, IORING_SETUP_SQPOLL);
  if (ret < 0) {
    errno = -ret;
    perror("io_uring_queue_init failed");
    return ret;
  }
  IoURing::Request *conn_request = IoURing::NewRequest(sock_fd, IoURing::ACCEPT);
  IoURing::AddAcceptEvent(&ring, conn_request);
  MyCoroutine::Schedule schedule(5000);  // 协程池初始化
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
        if (cqe->res > 0) {
          int client_fd = cqe->res;
          IoURing::Request *client_request =
              IoURing::NewRequest(client_fd, IoURing::READ);
          // 新的客户端连接，则创建协程
          client_request->cid = schedule.CoroutineCreate(
              HandlerClient, ref(schedule), ref(ring), client_request);
          schedule.CoroutineResume(client_request->cid);
        }
        // 继续等待新的客户端连接
        IoURing::AddAcceptEvent(&ring, request);
        continue;
      }
      assert(request->cid != MyCoroutine::kInvalidCid);
      request->cqe_res = cqe->res;
      schedule.CoroutineResume(request->cid);  // 唤醒之前主动让出cpu的协程
    }
    io_uring_cq_advance(&ring, count);
  }
  io_uring_queue_exit(&ring);
  IoURing::DeleteRequest(conn_request);
  return 0;
}
