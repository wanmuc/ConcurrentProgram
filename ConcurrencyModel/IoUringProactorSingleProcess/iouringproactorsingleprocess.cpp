#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "EventDriven/timer.hpp"
#include <iostream>

using namespace std;

#define TIMEOUT_MS 100

EventDriven::Timer timer;
struct io_uring ring;

void TimeOutCall() {
  cout << "call Timeout" << endl;
  timer.Register(1000, TimeOutCall);
}

int main() {
  struct io_uring ring;
  struct io_uring_cqe *cqe;

  uint32_t entries = 1024;

  // Initialize io_uring
  if (io_uring_queue_init(entries, &ring, 0) < 0) {
    perror("io_uring_queue_init");
    exit(EXIT_FAILURE);
  }

  timer.Register(1000, TimeOutCall);

  while (true) {
    EventDriven::TimerData timer_data;
    bool has_timer = timer.GetLastTimer(timer_data);
    if (has_timer) {
      struct io_uring_sqe *timeout_sqe = io_uring_get_sqe(&ring);
      struct __kernel_timespec ts;
      ts.tv_sec = timer.TimeOutMs(timer_data) / 1000;
      ts.tv_nsec = (timer.TimeOutMs(timer_data) % 1000) * 1000000;
      io_uring_prep_timeout(timeout_sqe, &ts, 0, 0); // 100 ms timeout
      timeout_sqe->user_data = 1; // Set user data to identify the timeout
      // Submit both requests
      io_uring_submit(&ring);
    }

    if (io_uring_wait_cqe(&ring, &cqe) < 0) {
      perror("io_uring_wait_cqe");
      break;
    }

    if (cqe->user_data == 1) { // Timeout operation
      timer.Run(timer_data);
    }
    // Mark the completion
    io_uring_cqe_seen(&ring, cqe);

  }

  io_uring_queue_exit(&ring);
  return 0;
}