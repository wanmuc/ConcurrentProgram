#pragma once

#include <semaphore.h>

#include <string>

class SemMutex {
 public:
  SemMutex(std::string sem_name) : sem_name_(sem_name) {}
  ~SemMutex() {}

  void Lock() {}
  void UnLock() {}

 private:
  sem_t *sem_;
  std::string sem_name_;
};