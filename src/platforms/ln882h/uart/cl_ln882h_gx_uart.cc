#include "cl_ln882h_gx_uart.h"

#ifdef CONFIG_IOT_PLATFORM_LN882H

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "osal/osal.h"
}

#include "cl_gx8006_fc_pin.h"
#include "utils/cl_logger.h"

namespace recorder {

namespace {
constexpr const char* kTag = "gx_uart";
constexpr uint32_t kRxTaskStack = 4096;
}  // namespace

Ln882hGxUart::~Ln882hGxUart() { Close(); }

bool Ln882hGxUart::Open(int port, uint32_t baud, uint16_t fc_pin) {
  fc_pin_ = fc_pin;
  serial_ = std::make_unique<cl::Ln882hUserSerial>(port);
  serial_->Begin(baud);
  gx8006_fc_pin_init(fc_pin_);
  LOG_I(kTag, "UART%d opened at %lu baud, FC pin %u", port, static_cast<unsigned long>(baud),
        fc_pin_);
  StartRxTask();
  return true;
}

void Ln882hGxUart::Close() {
  StopRxTask();
}

int Ln882hGxUart::Write(const uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) return 0;
  RecorderLockGuard lock(tx_mu_);
  if (!serial_) return 0;
  return static_cast<int>(serial_->Write(data, len));
}

bool Ln882hGxUart::FlowReady() const { return gx8006_fc_pin_read(fc_pin_); }

void Ln882hGxUart::StartRxTask() {
  if (task_ != nullptr) return;
  running_.store(true);
  task_exited_.store(false);
  auto* task = new OS_Thread_t;
  if (OS_ThreadCreate(task, "gx_rx", RxTaskEntry, this, OS_PRIORITY_NORMAL, kRxTaskStack) !=
      OS_OK) {
    delete task;
    running_.store(false);
    LOG_E(kTag, "failed to create GX RX task");
    return;
  }
  task_ = task;
}

void Ln882hGxUart::StopRxTask() {
  if (task_ == nullptr) return;
  running_.store(false);
  for (int i = 0; i < 30 && !task_exited_.load(); ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  delete static_cast<OS_Thread_t*>(task_);
  task_ = nullptr;
}

void Ln882hGxUart::RxTaskEntry(void* arg) { static_cast<Ln882hGxUart*>(arg)->RxTask(); }

void Ln882hGxUart::RxTask() {
  uint8_t buf[256];
  while (running_.load()) {
    const std::size_t n = serial_ ? serial_->ReadBytes(buf, sizeof(buf)) : 0;
    if (n > 0) {
      rx_bytes_.fetch_add(static_cast<uint32_t>(n));
      if (rx_cb_) rx_cb_(buf, n);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  task_exited_.store(true);
  OS_ThreadDelete(nullptr);
}

}  // namespace recorder

#endif  // CONFIG_IOT_PLATFORM_LN882H
