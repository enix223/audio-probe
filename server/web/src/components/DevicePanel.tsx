import { DASH, fmtDbfs } from '../format'
import type { DeviceSnapshot } from '../types'

function stateTone(state?: string): string {
  if (state === 'capturing') return 'good'
  if (state === 'error') return 'bad'
  if (state === 'connecting_ws' || state === 'handshaking' || state === 'draining') return 'warn'
  return 'muted'
}

export function DevicePanel({ devices }: { devices: DeviceSnapshot[] }) {
  return (
    <section className="panel">
      <div className="panel-head">
        <h2>Live devices</h2>
        <span className="count">{devices.length}</span>
      </div>
      {devices.length === 0 ? (
        <p className="muted small">
          No device connected. MQTT is the control plane; a <code>start</code> command opens the
          WebSocket data plane.
        </p>
      ) : (
        <ul className="device-list">
          {devices.map((d) => {
            const t = d.last_telemetry
            return (
              <li key={d.device_id || d.session_id}>
                <div className="device-row">
                  <span className={`pill ${stateTone(d.state)}`}>{d.state ?? 'unknown'}</span>
                  <strong>{d.device_id || 'unknown device'}</strong>
                </div>
                <div className="device-meta">
                  {d.audio?.format ?? DASH}
                  {d.audio?.sample_rate ? ` @ ${d.audio.sample_rate} Hz` : ''}
                  {d.audio?.channels ? ` · ${d.audio.channels} ch` : ''}
                </div>
                <div className="device-meta">
                  capture {d.capture_id || DASH} · {d.control?.owner ?? DASH} control
                </div>
                <div className="device-levels">
                  <span>RMS {fmtDbfs(t?.rms_dbfs ?? null)}</span>
                  <span>noise {fmtDbfs(t?.noise_dbfs ?? null)}</span>
                </div>
              </li>
            )
          })}
        </ul>
      )}
    </section>
  )
}
