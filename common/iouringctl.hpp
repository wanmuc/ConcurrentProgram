#pragma once

#include <liburing.h>

#include "iouringconn.hpp"

namespace IoURing {
enum Event { ACCEPT = 1, READ = 2, WRITE = 3 };

typedef struct Request {
  int event{-1};       // 事件类型
  Conn conn;           // 客户端连接封装
  int cid{-1};         // 在协程模式下，是关联的从协程id
  int32_t cqe_res{-1}; // io_uring完成队列对象返回值
} Request;

inline Request *NewRequest(int fd, Event event) {
  Request *request = new Request;
  request->conn.SetFd(fd);
  request->event = event;
  return request;
}

inline void DeleteRequest(Request *request) {
  if (not request) return;
  delete request;
}

inline void AddAcceptEvent(struct io_uring *ring, Request *request) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_accept(sqe, request->conn.Fd(), nullptr, 0, 0);
  io_uring_sqe_set_data(sqe, request);
  io_uring_submit(ring);
}

inline void AddReadEvent(struct io_uring *ring, Request *request) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  request->event = READ;
  io_uring_prep_recv(sqe, request->conn.Fd(), request->conn.ReadData(), request->conn.ReadLen(), 0);
  io_uring_sqe_set_data(sqe, request);
  io_uring_submit(ring);
}

inline void AddWriteEvent(struct io_uring *ring, Request *request) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  request->event = WRITE;
  io_uring_prep_send(sqe, request->conn.Fd(), request->conn.WriteData(), request->conn.WriteLen(), 0);
  io_uring_sqe_set_data(sqe, request);
  io_uring_submit(ring);
}
}  // namespace IoURing
