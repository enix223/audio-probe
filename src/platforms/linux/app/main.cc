#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "app/cl_init.h"
#include "sched/cl_pollable.h"
#include "utils/cl_logger.h"

#include "cl_file_audio_source.h"
#include "cl_hal_ws_linux.h"
#include "cl_linux_audio_sink.h"
#include "cl_linux_audio_source.h"
#include "cl_mqtt_control.h"
#include "cl_recorder_app.h"

namespace {

class SystemClock : public recorder::Clock {
 public:
  int64_t MonotonicMs() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  int64_t MonotonicUs() const override {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  int64_t WallMs() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
};

class AppPollable : public cl::Pollable {
 public:
  explicit AppPollable(recorder::RecorderApp* app) : app_(app) {}
  void Poll() override { app_->Poll(); }

 private:
  recorder::RecorderApp* app_;
};

struct Options {
  std::string server = "ws://127.0.0.1:8080/v1/recorder";
  std::string token;
  std::string ca_file;
  std::string mqtt_ca_file;
  std::string device_id = "recorder-linux";
  std::string source = "mic";
  std::string format = "pcm_s16le";
  std::string mode = "vad";
  std::string label = "recorder";
  int rate = 16000;
  int channels = 1;
  int frame_ms = 20;
  int pre_roll_ms = 300;
  int duration_ms = 0;
  bool realtime = true;
  bool loop = true;
  bool ws_control = false;
  bool ws_always = false;
};

void PrintUsage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "  --server URL          recorder WebSocket URL (default %s)\n"
      "  --token TOKEN         Authorization: Bearer token\n"
      "  --ca FILE             CA bundle for wss://\n"
      "  --mqtt-ca FILE        CA bundle for MQTT TLS (lab broker)\n"
      "  --device-id ID        device identity (default recorder-linux)\n"
      "  --source mic|file:P   capture source (default mic)\n"
      "  --no-realtime         replay a file as fast as possible\n"
      "  --no-loop             stop at the end of a file\n"
      "  --format pcm_s16le|opus\n"
      "  --rate HZ             default 16000\n"
      "  --channels N          default 1\n"
      "  --frame-ms N          default 20\n"
      "  --mode vad|continuous|single\n"
      "  --pre-roll-ms N       default 300\n"
      "  --duration-ms N       single-mode duration\n"
      "  --label TEXT          capture label\n"
      "  --ws-control          allow WS start/stop (debug/lab)\n"
      "  --ws-always           keep the WS connected while idle\n",
      argv0, "ws://127.0.0.1:8080/v1/recorder");
}

bool ParseArgs(int argc, char** argv, Options* o, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::string* out) -> bool {
      if (i + 1 >= argc) {
        *error = "missing value for " + a;
        return false;
      }
      *out = argv[++i];
      return true;
    };
    auto number = [&](int* out) -> bool {
      std::string v;
      if (!value(&v)) return false;
      try {
        *out = std::stoi(v);
      } catch (...) {
        *error = "invalid number for " + a;
        return false;
      }
      return true;
    };

    if (a == "--help" || a == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (a == "--server") {
      if (!value(&o->server)) return false;
    } else if (a == "--token") {
      if (!value(&o->token)) return false;
    } else if (a == "--ca") {
      if (!value(&o->ca_file)) return false;
    } else if (a == "--mqtt-ca") {
      if (!value(&o->mqtt_ca_file)) return false;
    } else if (a == "--device-id") {
      if (!value(&o->device_id)) return false;
    } else if (a == "--source") {
      if (!value(&o->source)) return false;
    } else if (a == "--format") {
      if (!value(&o->format)) return false;
    } else if (a == "--mode") {
      if (!value(&o->mode)) return false;
    } else if (a == "--label") {
      if (!value(&o->label)) return false;
    } else if (a == "--rate") {
      if (!number(&o->rate)) return false;
    } else if (a == "--channels") {
      if (!number(&o->channels)) return false;
    } else if (a == "--frame-ms") {
      if (!number(&o->frame_ms)) return false;
    } else if (a == "--pre-roll-ms") {
      if (!number(&o->pre_roll_ms)) return false;
    } else if (a == "--duration-ms") {
      if (!number(&o->duration_ms)) return false;
    } else if (a == "--no-realtime") {
      o->realtime = false;
    } else if (a == "--no-loop") {
      o->loop = false;
    } else if (a == "--ws-control") {
      o->ws_control = true;
    } else if (a == "--ws-always") {
      o->ws_always = true;
    } else {
      *error = "unknown option: " + a;
      return false;
    }
  }

  if (o->format != "pcm_s16le" && o->format != "opus") {
    *error = "--format must be pcm_s16le or opus";
    return false;
  }
  if (o->mode != "vad" && o->mode != "continuous" && o->mode != "single") {
    *error = "--mode must be vad, continuous, or single";
    return false;
  }
  if (o->mode == "single" && o->duration_ms <= 0) o->duration_ms = 10000;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  std::string error;
  if (!ParseArgs(argc, argv, &opts, &error)) {
    std::fprintf(stderr, "recorder: %s\n", error.c_str());
    PrintUsage(argv[0]);
    return 2;
  }

  // Probe the speaker before advertising test_tone: the protocol requires us
  // to advertise only verified capabilities.
  bool speaker_ok = false;
  {
    recorder::LinuxAudioSink probe;
    if (probe.Init(16000, 1) == recorder::AvpErr::kOk) {
      speaker_ok = true;
      probe.Deinit();
    }
  }

  std::unique_ptr<recorder::AudioSource> source;
  if (opts.source.rfind("file:", 0) == 0) {
    source = std::make_unique<recorder::FileAudioSource>(opts.source.substr(5), opts.realtime,
                                                         opts.loop);
  } else if (opts.source == "mic") {
    source = std::make_unique<recorder::LinuxAudioSource>();
  } else {
    std::fprintf(stderr, "recorder: unknown source %s\n", opts.source.c_str());
    return 2;
  }

  auto ws_factory = []() -> std::unique_ptr<recorder::RecorderWs> {
    return std::make_unique<recorder::WsClientLinux>();
  };
  recorder::RecorderClient client(ws_factory);
  SystemClock clock;
  recorder::LinuxAudioSink sink;

  // -----------------------------------------------------------------------
  // Composition root (embedded-iot-client): network, MQTT, thing model,
  // recorderCmd/recorderStatus datapoints.
  // -----------------------------------------------------------------------
  AppInit();

  // The upstream OneNET connect path uses ConfigStore().value() for the CA
  // and device-level-auth fields. Empty compile-time values fall back to
  // storage, which has no entry on a first run, so seed both explicitly.
  // An empty CA means "plaintext MQTT", which is the documented develop-only
  // lab mode.
  if (g_cfgStore != nullptr) {
    std::string ca;
    if (!opts.mqtt_ca_file.empty()) {
      std::ifstream f(opts.mqtt_ca_file);
      ca.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    g_cfgStore->SetOnenetCaCert(ca);
    g_cfgStore->SetOnenetDeviceLevelAuth(false);
  }

  recorder::RecorderMqttControl mqtt;
  recorder::AppDependencies deps;
  deps.source = source.get();
  deps.client = &client;
  deps.mqtt = &mqtt;
  deps.clock = &clock;
  deps.sink = &sink;
  deps.device.id = opts.device_id;
  deps.device.model = opts.source == "mic" ? "linux-pcm" : "linux-file";
  deps.device.firmware = "0.1.0";
  deps.device.chip = "linux";
  deps.device.audio_frontend = opts.source == "mic" ? "miniaudio" : "file";
  deps.ws_url = opts.server;
  deps.ws_token = opts.token;
  deps.ws_ca_file = opts.ca_file;
  deps.debug_allow_ws_control = opts.ws_control;
  deps.default_label = opts.label;

  recorder::Capabilities& caps = deps.capabilities;
  caps.pcm_s16le = true;
  caps.pcm.sample_rates = {16000, 48000};
  caps.pcm.channels = {1, 2};
  caps.pcm.frame_ms = {10, 20, 40, 60};
  caps.pcm.payload_max = static_cast<int>(protocol::kMaxPcmPayload);
  caps.pcm.channel_map = {"mic0"};
  caps.opus = true;
  caps.opus_cap.sample_rates = {16000, 48000};
  caps.opus_cap.channels = {1, 2};
  caps.opus_cap.frame_ms = {10, 20, 40, 60};
  caps.opus_cap.bitrate = {6000, 64000};
  caps.opus_cap.payload_max = static_cast<int>(protocol::kMaxOpusPayload);
  caps.opus_cap.encoders = {"host"};
  caps.vad = {"server", "energy"};
  caps.gain_supported = false;
  caps.test_tone = speaker_ok;
  caps.tone_waveforms = {"sine", "square", "sweep"};
  caps.tone_min_hz = 20.0;
  caps.tone_max_hz = 20000.0;
  caps.tone_max_amplitude_dbfs = 0.0;
  caps.speaker = speaker_ok;
  caps.speaker_min = 0;
  caps.speaker_max = 100;
  caps.loopback = false;
  caps.device_pre_roll_max_ms = 5000;
  caps.server_pre_roll = true;
  caps.max_segment_ms = 30000;
  caps.tx_queue_ms = 3000;
  caps.max_frame_bytes = 8192;

  deps.control.owner = "mqtt";
  deps.control.allow_ws_capture_control = opts.ws_control;
  deps.control.ws_mode = opts.ws_always ? "always" : "on_demand";
  deps.control.mqtt_lost_stop_timeout_ms = 30000;
  deps.control.max_capture_ms = 1800000;
  deps.control.ws_idle_timeout_ms = 10000;

  deps.audio.format = opts.format == "opus" ? recorder::AudioFormat::kOpus
                                            : recorder::AudioFormat::kPcmS16le;
  deps.audio.sample_rate = opts.rate;
  deps.audio.channels = opts.channels;
  deps.audio.frame_ms = opts.frame_ms;
  deps.audio.bits_per_sample = 16;
  deps.audio.interleaved = true;
  if (deps.audio.format == recorder::AudioFormat::kOpus) deps.audio.opus_encoder = "host";

  deps.capture.pre_roll_ms = opts.pre_roll_ms;
  deps.capture.max_segment_ms =
      opts.mode == "vad" ? 15000 : 30000;
  deps.capture.segment_ms = 10000;
  deps.capture.duration_ms = opts.duration_ms;
  deps.capture.vad.engine = opts.mode == "continuous" ? recorder::VadEngine::kServer
                                                      : recorder::VadEngine::kAuto;
  deps.capture.vad.onset_ms = 120;
  deps.capture.vad.silence_ms = 700;
  deps.capture.vad.min_speech_ms = 150;
  deps.capture.vad.guard_ms = 0;
  deps.capture.vad.threshold_db = 6.0;
  if (!recorder::ParseCaptureMode(opts.mode, &deps.capture.mode)) {
    std::fprintf(stderr, "recorder: bad mode\n");
    return 2;
  }

  deps.telemetry.frame_stats = true;
  deps.telemetry.level_interval_ms = 500;
  deps.telemetry.stats_interval_ms = 5000;
  deps.limits.max_frame_bytes = 8192;
  deps.limits.max_queue_ms = 3000;

  recorder::RecorderApp app(deps);
  if (!app.Init()) {
    LOG_E("main", "recorder init failed");
    return 1;
  }

  AppPollable pollable(&app);
  g_scheduler->Add(&pollable, 5);
  LOG_I("main", "recorder running (source=%s, %s)", opts.source.c_str(), opts.server.c_str());
  g_scheduler->Run();

  LOG_I("main", "shutting down recorder");
  app.Shutdown();

  g_sysService->Stop();
  g_cmdHandler->Stop();
  g_iotCore->Stop();
  g_ntp->Stop();
  g_network->Stop();
  g_eventBus->Stop();

  if (g_eventLoopThread) g_eventLoopThread->Join();
  if (g_healthThread) g_healthThread->Join();
  if (g_mqttThread) g_mqttThread->Join();
  if (g_cmdThread) g_cmdThread->Join();
  if (g_reconnectThread) g_reconnectThread->Join();

  LOG_I("main", "shutdown complete");
  return 0;
}
