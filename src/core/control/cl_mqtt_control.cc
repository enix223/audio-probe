#include "cl_mqtt_control.h"

#include <nonstd/any.hpp>

#include "app/cl_init.h"
#include "eventbus/cl_event.h"
#include "iot/cl_iot_core.h"
#include "iot/cl_named_datapoint.h"

namespace recorder {

namespace {
constexpr const char* kRecorderCmd = "recorderCmd";
constexpr const char* kRecorderStatus = "recorderStatus";
}  // namespace

RecorderMqttControl::~RecorderMqttControl() {
  if (initialized_ && g_eventBus != nullptr) {
    g_eventBus->Unsubscribe(cl::kEventInternalDatapoint, dp_handle_);
    g_eventBus->Unsubscribe(cl::kEventCloudStatusChange, cloud_handle_);
  }
}

bool RecorderMqttControl::Init() {
  if (initialized_) return true;
  if (g_iotCore == nullptr || g_eventBus == nullptr) return false;

  // Firmware-owned datapoint: IoTCore routes it to kEventInternalDatapoint and
  // never to the MCU serial path.
  g_iotCore->RegisterInternalDatapoint(kRecorderCmd);

  dp_handle_ = g_eventBus->Subscribe(cl::kEventInternalDatapoint, [this](const nonstd::any& payload) {
    const auto* named = nonstd::any_cast<cl::NamedDatapoint>(&payload);
    if (named == nullptr || named->identifier != kRecorderCmd) return;
    const auto* text = nonstd::get_if<std::string>(&named->value);
    if (text == nullptr) return;
    RecorderLockGuard lock(mu_);
    if (commands_.size() > 32) commands_.pop_front();
    commands_.push_back(*text);
  });

  cloud_handle_ = g_eventBus->Subscribe(cl::kEventCloudStatusChange, [this](const nonstd::any& payload) {
    const auto status = nonstd::any_cast<cl::CloudStatus>(payload);
    if (status == cl::CloudStatus::kConnectedCloud) {
      connected_.store(true);
      connected_event_.store(true);
    } else {
      connected_.store(false);
    }
  });

  initialized_ = true;
  if (g_iotCore->IsCloudConnected()) {
    connected_.store(true);
    connected_event_.store(true);
  }
  return true;
}

bool RecorderMqttControl::IsCloudConnected() const {
  if (g_iotCore != nullptr) return g_iotCore->IsCloudConnected();
  return connected_.load();
}

void RecorderMqttControl::PublishStatus(const std::string& json) {
  if (g_eventBus == nullptr) return;
  cl::NamedDatapoint named;
  named.identifier = kRecorderStatus;
  named.value = cl::DataValue(json);
  g_eventBus->Publish(cl::kEventDatapointReport, nonstd::any(std::move(named)));
}

bool RecorderMqttControl::TakeCommand(std::string* json) {
  RecorderLockGuard lock(mu_);
  if (commands_.empty()) return false;
  *json = std::move(commands_.front());
  commands_.pop_front();
  return true;
}

bool RecorderMqttControl::TakeConnectedEvent() { return connected_event_.exchange(false); }

}  // namespace recorder
