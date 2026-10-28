import { useEffect, useState } from 'react'
import { CommandError, sendCommand } from '../api'
import type { DeviceSnapshot } from '../types'

type Message = { tone: 'ok' | 'warn' | 'bad'; text: string }

export function ControlPanel({
  devices,
  onChanged,
}: {
  devices: DeviceSnapshot[]
  onChanged: () => void
}) {
  const [deviceId, setDeviceId] = useState('')
  const [mode, setMode] = useState<'vad' | 'continuous'>('vad')
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState<Message | null>(null)

  useEffect(() => {
    if (devices.length === 0) {
      if (deviceId) setDeviceId('')
      return
    }
    if (!deviceId || !devices.some((d) => d.device_id === deviceId)) {
      setDeviceId(devices[0].device_id ?? '')
    }
  }, [devices, deviceId])

  const device = devices.find((d) => d.device_id === deviceId)
  const recording = device?.state === 'capturing'

  async function run(command: Record<string, unknown>, label: string) {
    if (!deviceId) return
    setBusy(true)
    setMessage(null)
    try {
      const result = await sendCommand(deviceId, command)
      if (result.type === 'nack') {
        setMessage({
          tone: 'bad',
          text: `${label} rejected: ${result.code ?? 'error'}${result.message ? ` — ${result.message}` : ''}`,
        })
      } else if (result.type === 'failed') {
        setMessage({
          tone: 'bad',
          text: `${label} failed: ${result.code ?? 'error'}${result.detail ? ` — ${String(result.detail)}` : ''}`,
        })
      } else {
        setMessage({ tone: 'ok', text: `${label} accepted` })
      }
      onChanged()
    } catch (e) {
      let text = e instanceof Error ? e.message : String(e)
      if (e instanceof CommandError) {
        if (e.code === 'control_owned_by_mqtt') {
          text =
            'MQTT owns start/stop. Run the device with --ws-control --ws-always for lab control, or start it over MQTT.'
        } else if (e.status === 404) {
          text =
            'The device has no WebSocket session while idle (ws_mode=on_demand). Run it with --ws-always, or start over MQTT.'
        }
      }
      setMessage({ tone: 'bad', text })
    } finally {
      setBusy(false)
    }
  }

  const start = () =>
    void run(
      {
        type: 'start_capture',
        mode,
        label: `web-${new Date().toISOString().replace(/[:.]/g, '-')}`,
      },
      'Start',
    )

  const stop = () => void run({ type: 'stop_capture' }, 'Stop')

  return (
    <section className="panel control-panel">
      <div className="panel-head">
        <h2>Recording</h2>
        <span className={`pill ${recording ? 'good' : 'muted'}`}>
          {recording ? 'capturing' : device?.state ?? 'no device'}
        </span>
      </div>

      {devices.length === 0 ? (
        <p className="muted small">
          No live device. Start the device with MQTT connected; for frontend control use{' '}
          <code>--ws-control --ws-always</code>.
        </p>
      ) : (
        <>
          {devices.length > 1 ? (
            <label className="field">
              <span>Device</span>
              <select value={deviceId} onChange={(e) => setDeviceId(e.target.value)}>
                {devices.map((d) => (
                  <option key={d.device_id} value={d.device_id}>
                    {d.device_id}
                  </option>
                ))}
              </select>
            </label>
          ) : null}

          <div className="control-row">
            <select
              value={recording ? (device?.capture?.mode ?? mode) : mode}
              onChange={(e) => setMode(e.target.value as 'vad' | 'continuous')}
              disabled={busy || recording}
              aria-label="Capture mode"
            >
              <option value="vad">VAD</option>
              <option value="continuous">Continuous</option>
            </select>
            <button type="button" className="button primary" onClick={start} disabled={busy || recording}>
              {busy ? 'Working…' : 'Start'}
            </button>
            <button type="button" className="button" onClick={stop} disabled={busy || !recording}>
              Stop
            </button>
          </div>

          {recording ? (
            <p className="small muted control-capture">
              capture {device?.capture_id || 'pending'} · {device?.audio?.format ?? 'audio'} ·{' '}
              {device?.audio?.sample_rate ?? '?'} Hz
            </p>
          ) : null}

          {message ? <div className={`inline-message ${message.tone}`}>{message.text}</div> : null}
        </>
      )}
    </section>
  )
}
