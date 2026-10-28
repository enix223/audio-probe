import type { ReactNode } from 'react'
import { captureFileUrl } from '../api'
import { DASH, fmtDate, fmtDb, fmtDbfs, fmtInt, fmtMs, fmtPct, qualityFlags } from '../format'
import type { CaptureDetail, SegmentEntry } from '../types'
import { SegmentPanel } from './SegmentPanel'

function Chip({ children }: { children: ReactNode }) {
  return <span className="chip">{children}</span>
}

function Meta({ label, value }: { label: string; value: ReactNode }) {
  return (
    <div className="meta-item">
      <span className="meta-label">{label}</span>
      <span className="meta-value">{value}</span>
    </div>
  )
}

export function CaptureView({
  detail,
  segments,
  selectedSegment,
  onSelectSegment,
}: {
  detail: CaptureDetail
  segments: SegmentEntry[]
  selectedSegment: number | null
  onSelectSegment: (id: number) => void
}) {
  const m = detail.manifest
  const audio = m.audio
  const reportUrl = detail.report ?? captureFileUrl(detail.id, 'report.html')
  const summaryUrl = detail.summary ?? captureFileUrl(detail.id, 'summary.csv')
  const selected = segments.find((s) => s.id === selectedSegment) ?? segments[0]

  return (
    <div className="capture-view">
      <div className="capture-head">
        <div>
          <div className="eyebrow">capture</div>
          <h1 className="capture-title">{detail.id}</h1>
          <div className="chips">
            <Chip>{m.format ?? audio?.format ?? DASH}</Chip>
            {audio?.sample_rate ? <Chip>{audio.sample_rate} Hz</Chip> : null}
            {audio?.channels ? <Chip>{audio.channels} ch</Chip> : null}
            {audio?.frame_ms ? <Chip>{audio.frame_ms} ms</Chip> : null}
            {m.path ? <Chip>{m.path}</Chip> : null}
            {m.state ? <Chip>{m.state}</Chip> : null}
          </div>
        </div>
        <div className="head-actions">
          <a className="button" href={reportUrl} target="_blank" rel="noreferrer">
            Report
          </a>
          <a className="button" href={summaryUrl} download>
            Summary CSV
          </a>
          <a className="button" href={captureFileUrl(detail.id, 'manifest.json')} target="_blank" rel="noreferrer">
            Manifest
          </a>
        </div>
      </div>

      <div className="meta-grid">
        <Meta label="Device" value={m.device_id ?? DASH} />
        <Meta label="Model" value={m.device?.model ?? DASH} />
        <Meta
          label="Started"
          value={fmtDate(m.started_at)}
        />
        <Meta label="Ended" value={fmtDate(m.ended_at)} />
        <Meta label="End reason" value={m.end_reason ?? DASH} />
        <Meta
          label="Gain"
          value={m.gain_db === null || m.gain_db === undefined ? DASH : `${m.gain_db.toFixed(1)} dB`}
        />
        <Meta label="Frames" value={fmtInt(m.counters?.frames_received)} />
        <Meta label="Bytes" value={fmtInt(m.counters?.bytes_received)} />
        <Meta label="Loss" value={`${fmtInt(m.counters?.frames_lost)} frames`} />
      </div>

      {m.notes && m.notes.length > 0 ? (
        <ul className="notes">
          {m.notes.map((n) => (
            <li key={n}>{n}</li>
          ))}
        </ul>
      ) : null}

      <div className="segments-head">
        <h2>Segments</h2>
        <span className="count">{segments.length}</span>
      </div>

      {segments.length === 0 ? (
        <div className="empty">
          <p>No segments yet.</p>
          <p className="muted small">
            The server writes a segment when the device sends <code>segment_start</code>/
            <code>segment_end</code> and audio frames.
          </p>
        </div>
      ) : (
        <div className="segment-layout">
          <ul className="segment-list">
            {segments.map((s) => {
              const sm = s.metrics
              const flags = qualityFlags(sm).filter((f) => f.tone !== 'good')
              const active = selected?.id === s.id
              return (
                <li key={s.id}>
                  <button
                    type="button"
                    className={`segment-item${active ? ' active' : ''}`}
                    onClick={() => onSelectSegment(s.id)}
                  >
                    <span className="segment-id">#{s.id}</span>
                    <span className="segment-main">
                      <span className="segment-reason">
                        {s.meta?.segment_start?.reason ?? s.meta?.segment_end?.reason ?? 'segment'}
                        {s.meta?.segment_start?.label ? (
                          <span className="label-chip">{s.meta.segment_start.label}</span>
                        ) : null}
                      </span>
                      <span className="segment-sub">
                        {fmtMs(sm?.duration_ms)} · {fmtDbfs(sm?.rms_dbfs)} · SNR{' '}
                        {fmtDb(sm?.snr_db)}
                      </span>
                    </span>
                    <span className="segment-side">
                      {flags.length > 0 ? (
                        <span className="flag bad">{flags[0].label}</span>
                      ) : (
                        <span className="flag good">ok</span>
                      )}
                      <span className="small muted">loss {fmtPct(sm?.loss_pct)}</span>
                    </span>
                  </button>
                </li>
              )
            })}
          </ul>

          {selected ? (
            <SegmentPanel captureId={detail.id} segment={selected} />
          ) : null}
        </div>
      )}
    </div>
  )
}
