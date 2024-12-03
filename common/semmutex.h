#pragma once

#include <semaphore.h>

#include <string>

class SemMutex {
 public:
  SemMutex(std::string sem_name) : sem_name_(sem_name) {
    sem_ = sem_open(sem_name_, O_CREAT, 0644, 1);
    assert(sem_ != SEM_FAILED);
  }
  ~SemMutex() { sem_close(sem_); }

  void Lock() {
    sem_wait(sem_);  // P 操作
  }
  void UnLock() {
    sem_post(sem_);  //  V 操作
  }
  void Delete() { sem_unlink(sem_name_); }  // 删除信号量

 private:
  sem_t *sem_;
  std::string sem_name_;
};