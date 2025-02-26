#pragma once

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>

// 共享内存队列
class ShareMemoryQueue {
public:
  ShareMemoryQueue(key_t key, int count) : count_(count) {
    constexpr size_t kCurrentCountSize = 4; // 用4个字节来保存当前队列中数据的个数
    constexpr size_t kItemLenSize = 4; // 用4个字节来保存当前元素的大小
    constexpr size_t kItemMaxLen = 4 * 1024; // 队列中每个元素的最大长度为4k
    size_t total_size = kCurrentCountSize + (kItemLenSize + kItemMaxLen) * count;

    shm_id_ = shmget(key, total_size, 0666 | IPC_CREAT);
    assert(shm_id_ != -1);
    uint8_t* shm_begin = (uint8_t*)shmat(shm_id_, nullptr, 0);
    assert(shm_begin != nullptr);
    uint32_t * current_count_ = (uint32_t * )shm_begin;
  }

private:
  int32_t count_;                   // 队列大小
  int32_t *current_count_{nullptr}; // 当前队列大小
  uint8_t *shm_begin_{0};           // 共享内存起始地址
  int32_t shm_id_{0};               // 共享内存id
};