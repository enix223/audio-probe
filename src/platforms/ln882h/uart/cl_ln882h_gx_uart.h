#pragma once

#ifdef CONFIG_IOT_PLATFORM_LN882H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "cl_recorder_mutex.h"
#include "serial/ln882h_user_serial.h"

namespace recorder {

// UART1 transport for the GX8006. One RX FreeRTOS task feeds complete bytes to
// the protocol parser; TX is serialized so a control frame and a speaker frame
// can never interleave. DMA/ring sizing comes from the LN882H SDK project
// (CONFIG_IOT_UART_DMA_ENABLE / CONFIG_IOT_UART_DMA_RX_BUF_SIZE >= 2048).
class Ln882hGxUart {
 public:
  using RxFn = std::function<void(const uint8_t* data, std::size_t len)>;

  Ln882hGxUart() = default;
  ~Ln882hGxUart();

  bool Open(int port, uint32_t baud, uint16_t fc_pin);
  void Close();

  void SetRxCallback(RxFn cb) { rx_cb_ = std::move(cb); }
  void StartRxTask();
  void StopRxTask();

  int Write(const uint8_t* data, std::size_t len);
  bool FlowReady() const;

  uint32_t rx_bytes() const { return rx_bytes_.load(); }

 private:
  static void RxTaskEntry(void* arg);
  void RxTask();

  std::unique_ptr<cl::Ln882hUserSerial> serial_;
  uint16_t fc_pin_ = 5;
  RxFn rx_cb_;
  RecorderMutex tx_mu_;
  std::atomic<bool> running_{false};
  std::atomic<bool> task_exited_{false};
  std::atomic<uint32_t> rx_bytes_{0};
  void* task_ = nullptr;
};

}  // namespace recorder

#endif  // CONFIG_IOT_PLATFORM_LN882H
