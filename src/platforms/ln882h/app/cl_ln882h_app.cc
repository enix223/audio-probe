#include "cl_ln882h_app.h"

#ifdef CONFIG_IOT_PLATFORM_LN882H

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "osal/osal.h"
}

#include <ctime>
#include <memory>

#include "app/cl_init.h"
#include "sched/cl_pollable.h"
#include "utils/cl_logger.h"

#include "cl_gx8006_audio_source.h"
#include "cl_gx8006_protocol.h"
#include "cl_hal_ws_ln882h.h"
#include "cl_ln882h_capabilities.h"
#include "cl_ln882h_gx_uart.h"
#include "cl_mqtt_control.h"
#include "cl_recorder_app.h"

namespace {

class LnClock : public recorder::Clock {
 public:
  int64_t MonotonicMs() const override {
    return static_cast<int64_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
  }
  int64_t MonotonicUs() const override { return MonotonicMs() * 1000; }
  int64_t WallMs() const override {
    const std::time_t now = std::time(nullptr);
    return now > 0 ? static_cast<int64_t>(now) * 1000 : MonotonicMs();
  }
};

class AppPollable : public cl::Pollable {
 public:
  explicit AppPollable(recorder::RecorderApp* app) : app_(app) {}
  void Poll() override { app_->Poll(); }

 private:
  recorder::RecorderApp* app_;
};

void RecorderTask(void* arg) {
  (void)arg;
  LOG_I("main", "LN882H recorder starting");

  AppInit();

  // The upstream connect path calls ConfigStore().value() for the CA and
  // device-level-auth fields. Seed both explicitly; the CA comes from the
  // generated platform_config.h.
  if (g_cfgStore != nullptr) {
    g_cfgStore->SetOnenetCaCert(CONFIG_IOT_PLATFORM_ONENET_CA_CERT);
    g_cfgStore->SetOnenetDeviceLevelAuth(false);
  }

  recorder::Ln882hGxUart uart;
  if (!uart.Open(CONFIG_RECORDER_GX_UART_PORT, CONFIG_RECORDER_GX_BAUD,
                 CONFIG_RECORDER_GX_FC_PIN)) {
    LOG_E("main", "GX8006 UART open failed");
    return;
  }

  recorder::Gx8006Protocol proto;
  proto.Init(
      [&uart](const uint8_t* data, std::size_t len) { return uart.Write(data, len); },
      [&uart]() { return uart.FlowReady(); },
      [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms == 0 ? 1 : ms)); });
  uart.SetRxCallback([&proto](const uint8_t* data, std::size_t len) {
    proto.Input(data, len);
    proto.Process();
  });

  recorder::Gx8006AudioSource source(&proto);
  auto ws_factory = []() -> std::unique_ptr<recorder::RecorderWs> {
    return std::make_unique<recorder::WsClientLn882h>();
  };
  recorder::RecorderClient client(
      ws_factory, recorder::TxQueue::Config{CONFIG_RECORDER_TX_QUEUE_RECORDS,
                                            CONFIG_RECORDER_TX_QUEUE_BYTES});
  LnClock clock;
  recorder::RecorderMqttControl mqtt;

  recorder::AppDependencies deps;
  deps.source = &source;
  deps.client = &client;
  deps.mqtt = &mqtt;
  deps.clock = &clock;
  deps.sink = nullptr;  // test tone is not advertised on this platform
  deps.device.id = CONFIG_IOT_DEVICE_ID;
  if (deps.device.id.empty()) deps.device.id = "recorder-ln882h";
  deps.device.model = "ln882h-gx8006";
  deps.device.firmware = "0.1.0";
  deps.device.chip = "ln882h";
  deps.device.audio_frontend = "gx8006";
  deps.capabilities = recorder::BuildLn882hCapabilities();

  deps.control.owner = "mqtt";
  deps.control.allow_ws_capture_control = false;
  deps.control.ws_mode = "on_demand";
  deps.control.mqtt_lost_stop_timeout_ms = 30000;
  deps.control.max_capture_ms = 1800000;
  deps.control.ws_idle_timeout_ms = 10000;

  deps.audio.format = recorder::AudioFormat::kOpus;
  deps.audio.sample_rate = 16000;
  deps.audio.channels = 1;
  deps.audio.frame_ms = 20;
  deps.audio.bits_per_sample = 16;
  deps.audio.interleaved = true;
  deps.audio.opus_encoder = "gx8006";

  deps.capture.mode = recorder::CaptureMode::kContinuous;
  deps.capture.pre_roll_ms = 0;
  deps.capture.max_segment_ms = 30000;
  deps.capture.segment_ms = 10000;
  deps.capture.vad.engine = recorder::VadEngine::kServer;
  deps.capture.vad.onset_ms = 120;
  deps.capture.vad.silence_ms = 700;
  deps.capture.vad.min_speech_ms = 150;
  deps.capture.vad.threshold_db = 6.0;
  deps.capture.gain_supported = true;
  deps.capture.gain_db = 26;

  deps.telemetry.frame_stats = false;  // chip Opus has no local PCM stats
  deps.telemetry.level_interval_ms = 500;
  deps.telemetry.stats_interval_ms = 5000;
  deps.limits.max_frame_bytes = 2048;
  deps.limits.max_queue_ms = 3000;

  deps.ws_url = CONFIG_RECORDER_WS_URL;
  deps.ws_token = CONFIG_RECORDER_WS_TOKEN;
  deps.ws_ca_pem = CONFIG_RECORDER_WS_CA_PEM;
  deps.ws_allow_insecure = (CONFIG_RECORDER_WS_INSECURE != 0);
  deps.default_label = "ln882h";

  recorder::RecorderApp app(deps);
  if (!app.Init()) {
    LOG_E("main", "recorder init failed");
    return;
  }
  AppPollable pollable(&app);
  g_scheduler->Add(&pollable, 5);
  LOG_I("main", "recorder running");
  g_scheduler->Run();
}

}  // namespace

extern "C" int recorder_start(void) {
  static OS_Thread_t task;
  const OS_Status rc = OS_ThreadCreate(&task, "recorder", RecorderTask, nullptr,
                                       OS_PRIORITY_NORMAL, CONFIG_IOT_STACK_APP_MAIN);
  if (rc != OS_OK) {
    LOG_E("main", "failed to create recorder task: %d", static_cast<int>(rc));
    return -1;
  }
  return 0;
}

#endif  // CONFIG_IOT_PLATFORM_LN882H
