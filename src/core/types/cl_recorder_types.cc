#include "cl_recorder_types.h"

namespace recorder {

const char* CaptureModeName(CaptureMode m) {
  switch (m) {
    case CaptureMode::kVad:
      return "vad";
    case CaptureMode::kContinuous:
      return "continuous";
    case CaptureMode::kSingle:
      return "single";
  }
  return "vad";
}

const char* VadEngineName(VadEngine e) {
  switch (e) {
    case VadEngine::kAuto:
      return "auto";
    case VadEngine::kEnergy:
      return "energy";
    case VadEngine::kDevice:
      return "device";
    case VadEngine::kServer:
      return "server";
  }
  return "auto";
}

const char* RecorderStateName(RecorderState s) {
  switch (s) {
    case RecorderState::kIdle:
      return "idle";
    case RecorderState::kConnectingWs:
      return "connecting_ws";
    case RecorderState::kHandshaking:
      return "handshaking";
    case RecorderState::kCapturing:
      return "capturing";
    case RecorderState::kDraining:
      return "draining";
    case RecorderState::kError:
      return "error";
  }
  return "idle";
}

bool ParseCaptureMode(const std::string& s, CaptureMode* out) {
  if (s == "vad") {
    *out = CaptureMode::kVad;
    return true;
  }
  if (s == "continuous") {
    *out = CaptureMode::kContinuous;
    return true;
  }
  if (s == "single") {
    *out = CaptureMode::kSingle;
    return true;
  }
  return false;
}

bool ParseVadEngine(const std::string& s, VadEngine* out) {
  if (s == "auto") {
    *out = VadEngine::kAuto;
    return true;
  }
  if (s == "energy") {
    *out = VadEngine::kEnergy;
    return true;
  }
  if (s == "device") {
    *out = VadEngine::kDevice;
    return true;
  }
  if (s == "server") {
    *out = VadEngine::kServer;
    return true;
  }
  return false;
}

bool ParseAudioFormatName(const std::string& s, AudioFormat* out) {
  if (s == "pcm_s16le") {
    *out = AudioFormat::kPcmS16le;
    return true;
  }
  if (s == "opus") {
    *out = AudioFormat::kOpus;
    return true;
  }
  return false;
}

}  // namespace recorder
