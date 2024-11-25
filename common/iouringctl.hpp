#pragma once

#include <liburing.h>
#include "iouringconn.hpp"

namespace IoURing {
enum Event { ACCEPT = 1, READ = 2, WRITE = 3 };

typedef struct Request {
  int event{-1};
  Conn conn;
} Request;

inline Request *NewRequest(int fd, Event event) {
  Request *request = new Request;
  request->fd = fd;
  request->event = event;
  return request;
}

inline void DeleteRequest(Request *request) {
  if (not request) return;
  delete request;
}

inline void AddAcceptEvent(struct io_uring *ring, Request *request) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_accept(sqe, request->fd, nullptr, 0, 0);
  io_uring_sqe_set_data(sqe, request);
  io_uring_submit(ring);
}

inline void AddReadEvent(struct io_uring *ring, Request *request) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_recv(sqe, request->fd, request->buffer, request->buffer_len, 0);
  io_uring_sqe_set_data(sqe, request);
  io_uring_submit(ring);
}

inline void AddWriteEvent(struct io_uring *ring, Request *request) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_send(sqe, request->fd, request->buffer, request->buffer_len, 0);
  io_uring_sqe_set_data(sqe, request);
  io_uring_submit(ring);
}
}  // namespace IoURing
