import { fmtRelative } from '../format'
import type { CaptureSummary } from '../types'

interface Props {
  captures: CaptureSummary[]
  selected?: string | null
  loading: boolean
  onSelect: (id: string) => void
}

export function CaptureList({ captures, selected, loading, onSelect }: Props) {
  return (
    <section className="panel captures">
      <div className="panel-head">
        <h2>Captures</h2>
        <span className="count">{captures.length}</span>
      </div>
      {loading && captures.length === 0 ? (
        <p className="muted small">Loading captures…</p>
      ) : captures.length === 0 ? (
        <p className="muted small">
          No captures yet. Publish an MQTT <code>start</code> command; the server creates a capture
          when the WebSocket session begins.
        </p>
      ) : (
        <ul className="capture-list">
          {captures.map((c) => {
            const active = c.id === selected
            return (
              <li key={c.id}>
                <button
                  type="button"
                  className={`capture-item${active ? ' active' : ''}`}
                  onClick={() => onSelect(c.id)}
                >
                  <span className="capture-top">
                    <span className={`badge ${c.format === 'opus' ? 'opus' : 'pcm'}`}>
                      {c.format ?? 'capture'}
                    </span>
                    <span className="capture-time">{fmtRelative(c.started_at)}</span>
                  </span>
                  <span className="capture-id">{c.id}</span>
                  <span className="capture-sub">
                    {c.device_id || 'unknown device'} · {c.segments ?? 0} segment
                    {(c.segments ?? 0) === 1 ? '' : 's'}
                  </span>
                </button>
              </li>
            )
          })}
        </ul>
      )}
    </section>
  )
}
