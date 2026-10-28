#pragma once

// Small lock abstraction so the recorder core can run on the LN882H
// FreeRTOS build without relying on a hosted std::mutex implementation.
#if defined(RECORDER_MCU_LN882H)

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
}

namespace recorder {

class RecorderMutex {
 public:
  RecorderMutex() : handle_(xSemaphoreCreateMutex()) {}
  ~RecorderMutex() {
    if (handle_ != nullptr) vSemaphoreDelete(handle_);
  }
  RecorderMutex(const RecorderMutex&) = delete;
  RecorderMutex& operator=(const RecorderMutex&) = delete;

  void lock() {
    if (handle_ != nullptr) xSemaphoreTake(handle_, portMAX_DELAY);
  }
  void unlock() {
    if (handle_ != nullptr) xSemaphoreGive(handle_);
  }

 private:
  SemaphoreHandle_t handle_;
};

class RecorderLockGuard {
 public:
  explicit RecorderLockGuard(RecorderMutex& m) : m_(m) { m_.lock(); }
  ~RecorderLockGuard() { m_.unlock(); }
  RecorderLockGuard(const RecorderLockGuard&) = delete;
  RecorderLockGuard& operator=(const RecorderLockGuard&) = delete;

 private:
  RecorderMutex& m_;
};

}  // namespace recorder

#else

#include <mutex>

namespace recorder {
using RecorderMutex = std::mutex;
using RecorderLockGuard = std::lock_guard<std::mutex>;
}  // namespace recorder

#endif
