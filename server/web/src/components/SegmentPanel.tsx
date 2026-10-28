import type { ReactNode } from 'react'
import { DASH, fmtDb, fmtDbfs, fmtInt, fmtMs, fmtNum, fmtPct, qualityFlags } from '../format'
import { segmentFileUrl } from '../api'
import type { SegmentEntry } from '../types'

function Metric({ label, value, hint }: { label: string; value: ReactNode; hint?: string }) {
  return (
    <div className="metric">
      <span className="metric-label">{label}</span>
      <span className="metric-value">{value}</span>
      {hint ? <span className="metric-hint">{hint}</span> : null}
    </div>
  )
}

export function SegmentPanel({
  captureId,
  segment,
}: {
  captureId: string
  segment: SegmentEntry
}) {
  const m = segment.metrics
  const meta = segment.meta
  const start = meta?.segment_start
  const end = meta?.segment_end
  const tone = m?.tone ?? undefined
  const flags = qualityFlags(m)
  const audioUrl = segmentFileUrl(captureId, segment.id, 'audio.wav')
  const noiseAvailable = (m?.noise_floor_dbfs ?? -200) > -190
  const format = m?.format ?? meta?.audio?.format ?? captureId
  const toneExpected = tone?.expected_hz ?? meta?.tone?.frequency_hz ?? undefined

  return (
    <section className="segment-panel">
      <div className="segment-title">
        <div>
          <div className="eyebrow">segment {segment.id}</div>
          <h2>
            {start?.reason ?? end?.reason ?? 'audio'}
            {start?.label ? <span className="label-chip">{start.label}</span> : null}
          </h2>
          <div className="muted small">
            {fmtMs(m?.duration_ms)} · {fmtInt(m?.frames_received)} frames ·{' '}
            {format === 'opus' ? 'Opus (lossy)' : format}
          </div>
        </div>
        <div className="quality-flags">
          {flags.map((f) => (
            <span key={f.label} className={`flag ${f.tone}`}>
              {f.label}
            </span>
          ))}
        </div>
      </div>

      <div className="audio-box">
        {segment.has_audio ? (
          <audio controls preload="metadata" src={audioUrl}>
            Your browser does not support audio playback.
          </audio>
        ) : segment.has_packets ? (
          <div className="notice">
            Raw Opus packets are preserved, but this server build has no Opus decoder. Rebuild with{' '}
            <code>-tags "opus nolibopusfile"</code> and libopus, or analyze the packets externally.
          </div>
        ) : (
          <div className="notice">No audio file was written for this segment.</div>
        )}
        <div className="audio-links">
          {segment.has_audio ? (
            <a href={audioUrl} target="_blank" rel="noreferrer">
              Open WAV
            </a>
          ) : null}
          {segment.has_packets ? (
            <a href={segmentFileUrl(captureId, segment.id, 'packets.bin')} download>
              packets.bin
            </a>
          ) : null}
          <a href={segmentFileUrl(captureId, segment.id, 'metrics.json')} target="_blank" rel="noreferrer">
            metrics.json
          </a>
          <a href={segmentFileUrl(captureId, segment.id, 'frames.csv')} target="_blank" rel="noreferrer">
            frames.csv
          </a>
          {segment.meta ? (
            <a href={segmentFileUrl(captureId, segment.id, 'meta.json')} target="_blank" rel="noreferrer">
              meta.json
            </a>
          ) : null}
        </div>
      </div>

      {(segment.has_waveform || segment.has_spectrum) && (
        <div className="plots">
          {segment.has_waveform ? (
            <figure>
              <a href={segmentFileUrl(captureId, segment.id, 'plots/waveform.png')} target="_blank" rel="noreferrer">
                <img
                  src={segmentFileUrl(captureId, segment.id, 'plots/waveform.png')}
                  alt={`Segment ${segment.id} waveform`}
                  loading="lazy"
                />
              </a>
              <figcaption>Waveform</figcaption>
            </figure>
          ) : null}
          {segment.has_spectrum ? (
            <figure>
              <a href={segmentFileUrl(captureId, segment.id, 'plots/spectrum.png')} target="_blank" rel="noreferrer">
                <img
                  src={segmentFileUrl(captureId, segment.id, 'plots/spectrum.png')}
                  alt={`Segment ${segment.id} average spectrum`}
                  loading="lazy"
                />
              </a>
              <figcaption>Average spectrum</figcaption>
            </figure>
          ) : null}
        </div>
      )}

      {m ? (
        <>
          <h3>Levels</h3>
          <div className="metrics-grid">
            <Metric label="RMS" value={fmtDbfs(m.rms_dbfs)} />
            <Metric label="Peak" value={fmtDbfs(m.peak_dbfs)} />
            <Metric label="Crest" value={fmtDb(m.crest_db)} />
            <Metric label="Speech RMS" value={fmtDbfs(m.speech_rms_dbfs)} />
            <Metric
              label="Noise floor"
              value={fmtDbfs(m.noise_floor_dbfs)}
              hint={m.noise_method}
            />
            <Metric
              label="SNR"
              value={noiseAvailable ? fmtDb(m.snr_db) : DASH}
              hint={noiseAvailable ? undefined : 'noise floor unavailable'}
            />
            <Metric label="DC offset" value={`${fmtNum(m.dc_offset_lsb, 1)} LSB`} hint={fmtPct(m.dc_offset_pct_fs)} />
            <Metric label="Clipping" value={fmtPct(m.clip_pct)} />
            <Metric label="ZCR" value={fmtNum(m.zcr, 4)} />
          </div>

          <h3>Transport</h3>
          <div className="metrics-grid">
            <Metric label="Duration" value={fmtMs(m.duration_ms)} />
            <Metric label="Speech" value={fmtMs(m.speech_ms)} />
            <Metric label="Pre-roll" value={fmtMs(m.pre_roll_ms)} />
            <Metric label="Frames received" value={fmtInt(m.frames_received)} />
            <Metric label="Frames expected" value={fmtInt(m.frames_expected)} />
            <Metric label="Frames lost" value={fmtInt(m.frames_lost)} />
            <Metric label="Loss" value={fmtPct(m.loss_pct)} />
            <Metric label="Jitter" value={fmtMs(m.jitter_ms)} />
            <Metric label="Device drops" value={fmtInt(m.device_drops)} />
            <Metric label="Dropouts" value={fmtInt(m.dropouts)} />
            <Metric label="Path" value={m.path || DASH} />
            <Metric
              label="Gain"
              value={m.gain_db === null || m.gain_db === undefined ? DASH : `${fmtNum(m.gain_db, 1)} dB`}
            />
          </div>

          <h3>Spectrum</h3>
          <div className="metrics-grid">
            <Metric label="Centroid" value={m.spectral_centroid_hz ? `${fmtNum(m.spectral_centroid_hz, 0)} Hz` : DASH} />
            <Metric label="Flatness" value={fmtNum(m.spectral_flatness, 4)} />
            <Metric label="Rolloff 85" value={m.rolloff_85_hz ? `${fmtNum(m.rolloff_85_hz, 0)} Hz` : DASH} />
            <Metric label="Hum 50" value={fmtDb(m.hum_50_db)} />
            <Metric label="Hum 60" value={fmtDb(m.hum_60_db)} />
            <Metric label="HF ratio" value={fmtNum(m.high_freq_ratio, 4)} />
          </div>

          {tone ? (
            <>
              <h3>Tone analysis</h3>
              <div className="metrics-grid">
                <Metric label="Tone ID" value={tone.tone_id || DASH} />
                <Metric label="Waveform" value={tone.waveform || DASH} />
                <Metric label="Expected" value={toneExpected ? `${fmtNum(toneExpected, 1)} Hz` : DASH} />
                <Metric label="Measured" value={tone.measured_hz ? `${fmtNum(tone.measured_hz, 2)} Hz` : DASH} />
                <Metric label="Frequency error" value={tone.frequency_error_ppm !== undefined ? `${fmtNum(tone.frequency_error_ppm, 0)} ppm` : DASH} />
                <Metric label="Amplitude" value={fmtDbfs(tone.amplitude_dbfs)} />
                <Metric label="THD" value={fmtDb(tone.thd_db)} />
                <Metric label="THD+N" value={fmtDb(tone.thd_n_db)} />
                <Metric label="SFDR" value={fmtDb(tone.sfdr_db)} />
                <Metric label="FFT" value={tone.fft_size ? `${tone.fft_size} (${tone.window || 'hann'})` : DASH} />
              </div>
              {format === 'opus' ? (
                <p className="note">
                  Opus is lossy. THD/THD+N and frequency response include codec artifacts and are
                  not chip-grade.
                </p>
              ) : null}
            </>
          ) : null}

          {m.utterances && m.utterances.length > 0 ? (
            <>
              <h3>Server-VAD utterances</h3>
              <div className="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th>#</th>
                      <th>Start</th>
                      <th>End</th>
                      <th>Speech</th>
                      <th>RMS</th>
                      <th>Noise</th>
                      <th>SNR</th>
                    </tr>
                  </thead>
                  <tbody>
                    {m.utterances.map((u, i) => (
                      <tr key={`${u.index ?? i}-${u.start_ms ?? i}`}>
                        <td>{u.index ?? i}</td>
                        <td>{fmtMs(u.start_ms)}</td>
                        <td>{fmtMs(u.end_ms)}</td>
                        <td>{fmtMs(u.speech_ms)}</td>
                        <td>{fmtDbfs(u.rms_dbfs)}</td>
                        <td>{fmtDbfs(u.noise_dbfs)}</td>
                        <td>{fmtDb(u.snr_db)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </>
          ) : null}

          {m.notes && m.notes.length > 0 ? (
            <ul className="notes">
              {m.notes.map((n) => (
                <li key={n}>{n}</li>
              ))}
            </ul>
          ) : null}
        </>
      ) : (
        <div className="notice">Metrics are not available yet for this segment.</div>
      )}

      <h3>Boundaries</h3>
      <div className="metrics-grid">
        <Metric label="Start reason" value={start?.reason || DASH} />
        <Metric label="Onset" value={fmtMs(start?.onset_t_ms)} />
        <Metric label="Start t" value={fmtMs(start?.t_ms)} />
        <Metric label="End reason" value={end?.reason || DASH} />
        <Metric label="End t" value={fmtMs(end?.t_ms)} />
        <Metric label="Pre-roll frames" value={fmtInt(end?.pre_roll_frames)} />
        <Metric label="Frames dropped" value={fmtInt(end?.frames_dropped)} />
        <Metric label="Tone ID" value={meta?.tone?.tone_id || DASH} />
      </div>
      {meta?.tone ? (
        <p className="small muted">
          Tone metadata: {meta.tone.waveform ?? 'sine'} @{' '}
          {fmtNum(meta.tone.frequency_hz ?? meta.tone.start_hz, 1)} Hz ·{' '}
          {fmtDbfs(meta.tone.amplitude_dbfs)} · volume {meta.tone.speaker_volume ?? DASH} · path{' '}
          {meta.tone.capture_path ?? DASH}
        </p>
      ) : null}
    </section>
  )
}
