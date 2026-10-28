#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include "cl_recorder_mutex.h"
#include <string>

namespace recorder {

// Control-plane interface. The app never talks to embedded-iot-client
// directly; tests provide a fake implementation.
class MqttControl {
 public:
  virtual ~MqttControl() = default;

  virtual bool Init() = 0;
  virtual bool IsCloudConnected() const = 0;
  virtual void PublishStatus(const std::string& json) = 0;

  // Pops one recorder_cmd JSON envelope delivered by the cloud.
  virtual bool TakeCommand(std::string* json) = 0;
  // True once after the MQTT link comes up (the app publishes its status).
  virtual bool TakeConnectedEvent() = 0;
};

// embedded-iot-client composition-root adapter (AppInit/IoTCore/thing model).
// recorderCmd is registered as an internal datapoint and never reaches a
// serial MCU path; recorderStatus is reported through kEventDatapointReport.
class RecorderMqttControl : public MqttControl {
 public:
  ~RecorderMqttControl() override;

  bool Init() override;
  bool IsCloudConnected() const override;
  void PublishStatus(const std::string& json) override;
  bool TakeCommand(std::string* json) override;
  bool TakeConnectedEvent() override;

 private:
  RecorderMutex mu_;
  std::deque<std::string> commands_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> connected_event_{false};
  bool initialized_ = false;
  uint32_t dp_handle_ = 0;
  uint32_t cloud_handle_ = 0;
};

}  // namespace recorder
