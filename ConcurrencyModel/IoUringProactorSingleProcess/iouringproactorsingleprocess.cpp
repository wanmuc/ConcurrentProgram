#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>

#include "EventDriven/timer.hpp"

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
    struct io_uring_cqe *cqe;
    struct __kernel_timespec temp;
    struct __kernel_timespec *ts = nullptr;
    if (has_timer) {
      temp.tv_sec = timer.TimeOutMs(timer_data) / 1000;
      temp.tv_nsec = (timer.TimeOutMs(timer_data) % 1000) * 1000000;
      ts = &temp;
    }
    int ret = io_uring_wait_cqes(&ring, &cqe, 1024, ts, nullptr);
    if (ret < 0) {
      errno = -ret;
      perror("io_uring_wait_cqes faild");
      continue;
    }

    unsigned head;
    unsigned count = 0;
    io_uring_for_each_cqe(&ring, head, cqe) { count++; }
    io_uring_cq_advance(&ring, count);
    if (has_timer) timer.Run(timer_data);
  }

  io_uring_queue_exit(&ring);
  return 0;
}