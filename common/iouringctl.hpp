#pragma once

#include <liburing.h>

namespace IoURing {
enum Event { ACCEPT = 1, READ = 2, WRITE = 3 };

typedef struct Request {
  int fd{-1};
  int event{-1};
  uint8_t *buffer{nullptr};
  size_t buffer_len{0};
} Request;

inline Request *NewRequest(int fd, Event event, size_t buffer_len) {
  Request *request = new Request;
  request->fd = fd;
  request->event = event;
  if (event != ACCEPT) {
    assert(buffer_len > 0);
    request->buffer = new uint8_t[buffer_len];
    request->buffer_len = buffer_len;
  }
  return request;
}

inline void DeleteRequest(Request *request) {
  if (not request) return;
  if (request->buffer) delete[] request->buffer;
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
