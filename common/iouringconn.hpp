#pragma once

#include <liburing.h>

#include "utils.hpp"

namespace IoURing {
class Conn {
 public:
  void SetFd(int fd) { fd_ = fd; }
  int Fd() { return fd_; }
  uint8_t* ReadData() { return codec_.Data(); }
  size_t ReadLen() { return codec_.Len(); }
  void ReadCallBack(size_t len) { codec_.DeCode(len); }
  bool OneMessage() {
    bool get_one{false};
    std::string* temp = codec_.GetMessage();
    if (temp) {
      get_one = true;
      message_ = *temp;
      delete temp;
    }
    return get_one;
  }

  uint8_t* WriteData() { return pkt_.Data() + send_len_; }
  size_t WriteLen() { return pkt_.UseLen() - send_len_; }
  void WriteCallBack(size_t send_len) { send_len_ += send_len; }

  bool FinishWrite() { return send_len_ == pkt_.UseLen(); }

  void Reset() {
    codec_.Reset();
    send_len_ = 0;
    pkt_.Reset();
  }

 private:
  int fd_;
  size_t send_len_{0};   // 要发送的应答数据的长度
  std::string message_;  // 对于EchoServer来说，即是获取的请求消息，也是要发送的应答消息
  Packet pkt_;           // 发送应答消息的二进制数据包
  Codec codec_;          // EchoServer协议的编解码
};
}  // namespace IoURing
