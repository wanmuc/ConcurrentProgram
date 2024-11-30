#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <vector>

#include "common/cmdline.h"
#include "common/conn.hpp"
#include "common/epollctl.hpp"

using namespace std;
using namespace MyEcho;

void mainReactor(string ip, int64_t port, bool is_main_read, vector<int> &send_fd_unix_sockets) {
  int index = 0;
  int sock_fd = CreateListenSocket(ip, port, true);
  assert(sock_fd > 0);
  epoll_event events[2048];
  int epoll_fd = epoll_create(1);
  assert(epoll_fd > 0);
  Conn conn(sock_fd, epoll_fd, true);
  SetNotBlock(sock_fd);
  AddReadEvent(&conn);
  auto getClientUnixSocketFd = [&index, &send_fd_unix_sockets]() {
    index++;
    index %= send_fd_unix_sockets.size();
    return send_fd_unix_sockets[index];
  };
  while (true) {
    int num = epoll_wait(epoll_fd, events, 2048, -1);
    if (num < 0) {
      perror("epoll_wait failed");
      continue;
    }
    for (int i = 0; i < num; i++) {
      Conn *conn = (Conn *)events[i].data.ptr;
      if (conn->Fd() == sock_fd) {  // 有客户端的连接到来了
        LoopAccept(sock_fd, 2048, [is_main_read, epoll_fd, getClientUnixSocketFd](int client_fd) {
          SetNotBlock(client_fd);
          if (is_main_read) {
            Conn *conn = new Conn(client_fd, epoll_fd, true);
            AddReadEvent(conn);  // 在mainReactor线程中监听可读事件
          } else {
            SendFd(getClientUnixSocketFd(), client_fd);
            close(client_fd);
          }
        });
        continue;
      }
      // 客户端有数据可读，则把连接迁移到subReactor线程中管理
      ClearEvent(conn, false);
      SendFd(getClientUnixSocketFd(), conn->Fd());
      close(conn->Fd());
      delete conn;
    }
  }
}

void subReactor(int read_fd_unix_socket, int64_t sub_reactor_count) {
  epoll_event events[2048];
  int epoll_fd = epoll_create(1);
  if (epoll_fd < 0) {
    perror("epoll_create failed");
    return;
  }
  Conn conn(read_fd_unix_socket, epoll_fd, true);
  conn.SetUnixSocket();
  AddReadEvent(&conn);
  while (true) {
    int num = epoll_wait(epoll_fd, events, 2048, -1);
    if (num < 0) {
      perror("epoll_wait failed");
      continue;
    }
    for (int i = 0; i < num; i++) {
      Conn *conn = (Conn *)events[i].data.ptr;
      auto releaseConn = [&conn]() {
        ClearEvent(conn);
        delete conn;
      };
      if (conn->IsUnixSocket()) {
        int client_fd = 0;
        // 接收从mainReactor传递过来的客户端连接fd
        if (0 == RecvFd(conn->Fd(), client_fd)) {
          Conn *conn = new Conn(client_fd, epoll_fd, true);
          AddReadEvent(conn);
        }
        continue;
      }
      // 执行到这里就是真正的客户端的读写事件
      if (events[i].events & EPOLLIN) {  // 可读
        if (not conn->Read()) {          // 执行非阻塞读
          releaseConn();
          continue;
        }
        if (conn->OneMessage()) {  // 判断是否要触发写事件
          conn->EnCode();
          ModToWriteEvent(conn);  // 修改成只监控可写事件
        }
      }
      if (events[i].events & EPOLLOUT) {  // 可写
        if (not conn->Write()) {          // 执行非阻塞写
          releaseConn();
          continue;
        }
        if (conn->FinishWrite()) {  // 完成了请求的应答写，则可以释放连接
          conn->Reset();
          ModToReadEvent(conn);  // 修改成只监控可读事件
        }
      }
    }
  }
}

void createSubReactor(vector<int> &send_fd_unix_sockets, int64_t sub_reactor_count) {
  for (int i = 0; i < sub_reactor_count; i++) {
    int socket_pairs[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socket_pairs);
    assert(0 == ret);
    // socket_pairs的第一个 fd 作为写端，保持着send_fd_unix_sockets数组中
    send_fd_unix_sockets.push_back(socket_pairs[0]);
    pid_t pid = fork();
    assert(pid != -1);
    if (pid == 0) {  // 子进程
      // 子进程会继承父进程写端的 fd，这里需要全部关闭，避免 fd 泄露
      for_each(send_fd_unix_sockets.begin(), send_fd_unix_sockets.end(), [](int fd) { close(fd); });
      cout << "subReactor pid = " << getpid() << endl;
      // socket_pairs[1]作为读端，用于获取传递的客户端连接
      subReactor(socket_pairs[1], sub_reactor_count);
      exit(0);
    } else {
      close(socket_pairs[1]);  // 父进程关闭socket_pairs的另一个 fd
    }
  }
}

void createMainReactor(string ip, int64_t port, bool is_main_read, int64_t main_reactor_count,
                       vector<int> &send_fd_unix_sockets) {
  for (int i = 0; i < main_reactor_count; i++) {
    pid_t pid = fork();
    assert(pid != -1);
    if (pid == 0) {  // 子进程
      cout << "mainReactor pid = " << getpid() << endl;
      mainReactor(ip, port, is_main_read, send_fd_unix_sockets);
      exit(0);
    }
  }
}

void usage() {
  cout << "EpollReactorProcessPoolMS -ip 0.0.0.0 -port 1688 -main 3 -sub 8 -mainread" << endl;
  cout << "options:" << endl;
  cout << "    -h,--help      print usage" << endl;
  cout << "    -ip,--ip       listen ip" << endl;
  cout << "    -port,--port   listen port" << endl;
  cout << "    -main,--main   mainReactor count" << endl;
  cout << "    -sub,--sub     subReactor count" << endl;
  cout << "    -mainread,--mainread mainReactor read" << endl;
  cout << endl;
}

int main(int argc, char *argv[]) {
  string ip;
  int64_t port;
  int64_t main_reactor_count;
  int64_t sub_reactor_count;
  bool is_main_read;
  CmdLine::StrOptRequired(&ip, "ip");
  CmdLine::Int64OptRequired(&port, "port");
  CmdLine::Int64OptRequired(&main_reactor_count, "main");
  CmdLine::Int64OptRequired(&sub_reactor_count, "sub");
  CmdLine::BoolOpt(&is_main_read, "mainread");
  CmdLine::SetUsage(usage);
  CmdLine::Parse(argc, argv);
  main_reactor_count = main_reactor_count > GetNProcs() ? GetNProcs() : main_reactor_count;
  sub_reactor_count = sub_reactor_count > GetNProcs() ? GetNProcs() : sub_reactor_count;
  vector<int> send_fd_unix_sockets;  // MainReactor侧用于发送客户端连接的 unix socket数组
  createSubReactor(send_fd_unix_sockets, sub_reactor_count);                            // 创建SubReactor进程
  createMainReactor(ip, port, is_main_read, main_reactor_count, send_fd_unix_sockets);  // 创建MainRector进程
  // 父进程写端的 fd数组已经没用了，这里需要全部关闭，避免 fd 泄露
  for_each(send_fd_unix_sockets.begin(), send_fd_unix_sockets.end(), [](int fd) { close(fd); });
  while (true) sleep(1);  // 主进程陷入死循环
  return 0;
}