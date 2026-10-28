// Shapes mirror the Go JSON in server/internal/storage, metrics, and httpapi.

export interface CaptureSummary {
  id: string
  path: string
  started_at?: string
  device_id?: string
  format?: string
  segments: number
}

export interface DeviceInfo {
  id?: string
  model?: string
  firmware?: string
  chip?: string
  audio_frontend?: string
  boot_id?: string
}

export interface AudioConfig {
  format: string
  sample_rate: number
  channels: number
  frame_ms: number
  bits_per_sample?: number
  endianness?: string
  interleaved?: boolean
  opus?: { encoder?: string; bitrate?: number }
}

export interface CaptureConfig {
  mode?: string
  pre_roll_ms?: number
  max_segment_ms?: number
  segment_ms?: number
  vad?: { engine?: string; threshold_db?: number }
}

export interface Counters {
  frames_received?: number
  bytes_received?: number
  segments?: number
  frames_lost?: number
  device_drops?: number
  decode_errors?: number
  backpressure?: number
}

export interface Manifest {
  capture_id: string
  session_id?: string
  device_id?: string
  device?: DeviceInfo
  format?: string
  sample_rate?: number
  channels?: number
  frame_ms?: number
  path?: string
  gain_db?: number | null
  started_at?: string
  ended_at?: string
  end_reason?: string
  state?: string
  audio?: AudioConfig
  capture?: CaptureConfig
  counters?: Counters
  notes?: string[]
  segments?: unknown[]
}

export interface CaptureDetail {
  id: string
  manifest: Manifest
  segments: string[]
  report?: string
  summary?: string
}

export interface ToneResult {
  tone_id?: string
  waveform?: string
  expected_hz?: number
  measured_hz?: number
  frequency_error_ppm?: number
  amplitude_dbfs?: number
  thd_db?: number
  thd_n_db?: number
  sfdr_db?: number
  analysis_band_hz?: [number, number]
  fft_size?: number
  window?: string
  harmonics?: number
  no_signal?: boolean
}

export interface Utterance {
  index?: number
  start_ms?: number
  end_ms?: number
  speech_ms?: number
  pre_roll_ms?: number
  rms_dbfs?: number
  peak_dbfs?: number
  noise_dbfs?: number
  snr_db?: number
  start_sample?: number
  end_sample?: number
}

export interface SegmentMetrics {
  segment_id: number
  capture_id?: string
  format?: string
  sample_rate?: number
  channels?: number
  gain_db?: number | null
  path?: string
  duration_ms?: number
  speech_ms?: number
  pre_roll_ms?: number
  frames_expected?: number
  frames_received?: number
  frames_lost?: number
  loss_pct?: number
  jitter_ms?: number
  rms_dbfs?: number
  peak_dbfs?: number
  crest_db?: number
  dc_offset_lsb?: number
  dc_offset_pct_fs?: number
  clip_pct?: number
  zcr?: number
  speech_rms_dbfs?: number
  noise_floor_dbfs?: number
  snr_db?: number
  noise_method?: string
  spectral_centroid_hz?: number
  spectral_flatness?: number
  rolloff_85_hz?: number
  hum_50_db?: number
  hum_60_db?: number
  high_freq_ratio?: number
  dropouts?: number
  device_drops?: number
  tone?: ToneResult | null
  utterances?: Utterance[]
  notes?: string[]
}

export interface ToneInfo {
  tone_id?: string
  waveform?: string
  frequency_hz?: number
  start_hz?: number
  end_hz?: number
  sweep_mode?: string
  amplitude_dbfs?: number
  speaker_volume?: number
  capture_path?: string
  loopback?: boolean
  fade_ms?: number
}

export interface SegmentStartMeta {
  reason?: string
  t_ms?: number
  onset_t_ms?: number
  pre_roll_ms?: number
  label?: string
  tone?: ToneInfo | null
  audio?: AudioConfig
}

export interface SegmentEndMeta {
  reason?: string
  t_ms?: number
  frames?: number
  frames_dropped?: number
  pre_roll_frames?: number
  duration_ms?: number
  speech_ms?: number
  rms_dbfs?: number
  peak_dbfs?: number
  noise_dbfs?: number
}

export interface SegmentMeta {
  segment_start?: SegmentStartMeta
  segment_end?: SegmentEndMeta
  audio?: AudioConfig
  tone?: ToneInfo | null
  notes?: string[]
  utterance_files?: string[]
}

export interface SegmentEntry {
  id: number
  has_audio: boolean
  has_packets: boolean
  has_waveform: boolean
  has_spectrum: boolean
  metrics?: SegmentMetrics
  meta?: SegmentMeta
}

export interface Telemetry {
  t_ms?: number
  vad_state?: string
  vad_score?: number
  rms_dbfs?: number | null
  peak_dbfs?: number | null
  noise_dbfs?: number | null
  tx_queue_ms?: number
  tx_drops?: number
  tx_errors?: number
  capture_overruns?: number
  reconnects?: number
  rssi_dbm?: number
  heap_free?: number
  uptime_ms?: number
}

export interface DeviceSnapshot {
  session_id?: string
  capture_id?: string
  device_id?: string
  state?: string
  control?: { owner?: string; ws_mode?: string; allow_ws_capture_control?: boolean }
  audio?: AudioConfig
  capture?: CaptureConfig
  counters?: Counters
  notes?: string[]
  last_telemetry?: Telemetry
}
