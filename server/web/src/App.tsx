import { useCallback, useEffect, useRef, useState } from 'react'
import { fetchCapture, fetchCaptures, fetchDevices, fetchSegments } from './api'
import { CaptureList } from './components/CaptureList'
import { CaptureView } from './components/CaptureView'
import { ControlPanel } from './components/ControlPanel'
import { DevicePanel } from './components/DevicePanel'
import type { CaptureDetail, CaptureSummary, DeviceSnapshot, SegmentEntry } from './types'

function parseHash(): { capture: string | null; segment: number | null } {
  const match = window.location.hash.match(/^#\/capture\/([^/]+)(?:\/(\d+))?/)
  if (!match) return { capture: null, segment: null }
  return {
    capture: decodeURIComponent(match[1]),
    segment: match[2] ? Number(match[2]) : null,
  }
}

export default function App() {
  const initial = useRef(parseHash()).current
  const [captures, setCaptures] = useState<CaptureSummary[]>([])
  const [devices, setDevices] = useState<DeviceSnapshot[]>([])
  const [captureId, setCaptureId] = useState<string | null>(initial.capture)
  const [segmentId, setSegmentId] = useState<number | null>(initial.segment)
  const [detail, setDetail] = useState<CaptureDetail | null>(null)
  const [segments, setSegments] = useState<SegmentEntry[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loadingCaptures, setLoadingCaptures] = useState(true)
  const [loadingDetail, setLoadingDetail] = useState(false)
  const [refreshKey, setRefreshKey] = useState(0)

  const loadCaptures = useCallback(async (showLoading: boolean) => {
    if (showLoading) setLoadingCaptures(true)
    try {
      const next = await fetchCaptures()
      setCaptures(next)
      setCaptureId((prev) => prev ?? (next.length > 0 ? next[0].id : null))
    } catch (e) {
      setError(String(e))
    } finally {
      if (showLoading) setLoadingCaptures(false)
    }
  }, [])

  useEffect(() => {
    void loadCaptures(true)
    const timer = window.setInterval(() => void loadCaptures(false), 5000)
    return () => window.clearInterval(timer)
  }, [loadCaptures])

  useEffect(() => {
    let alive = true
    const load = async () => {
      try {
        const next = await fetchDevices()
        if (alive) setDevices(next)
      } catch {
        /* device polling is best-effort */
      }
    }
    void load()
    const timer = window.setInterval(load, 2000)
    return () => {
      alive = false
      window.clearInterval(timer)
    }
  }, [])

  useEffect(() => {
    if (!captureId) {
      setDetail(null)
      setSegments([])
      setLoadingDetail(false)
      return
    }
    let alive = true
    let first = true
    const load = async () => {
      if (first) setLoadingDetail(true)
      try {
        const [nextDetail, nextSegments] = await Promise.all([
          fetchCapture(captureId),
          fetchSegments(captureId),
        ])
        if (!alive) return
        setDetail(nextDetail)
        setSegments(nextSegments)
        setSegmentId((prev) => {
          if (prev !== null && nextSegments.some((s) => s.id === prev)) return prev
          return nextSegments.length > 0 ? nextSegments[0].id : null
        })
        setError(null)
      } catch (e) {
        if (alive) setError(String(e))
      } finally {
        if (alive) {
          setLoadingDetail(false)
          first = false
        }
      }
    }
    void load()
    const timer = window.setInterval(load, 5000)
    return () => {
      alive = false
      window.clearInterval(timer)
    }
  }, [captureId, refreshKey])

  useEffect(() => {
    const hash = captureId
      ? `#/capture/${encodeURIComponent(captureId)}${segmentId !== null ? `/${segmentId}` : ''}`
      : ''
    if (window.location.hash !== hash) {
      window.history.replaceState(null, '', hash || `${window.location.pathname}${window.location.search}`)
    }
  }, [captureId, segmentId])

  const selectCapture = useCallback((id: string) => {
    setCaptureId(id)
    setSegmentId(null)
  }, [])

  const refresh = useCallback(() => {
    setRefreshKey((k) => k + 1)
    void loadCaptures(false)
  }, [loadCaptures])

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark" aria-hidden="true">
            ◉
          </span>
          <div>
            <h1>Audio Probe</h1>
            <span className="brand-sub">capture diagnostics</span>
          </div>
        </div>
        <div className="top-actions">
          <span className={`live-dot${devices.length > 0 ? ' on' : ''}`} aria-hidden="true" />
          <span className="muted small">
            {devices.length} live device{devices.length === 1 ? '' : 's'}
          </span>
          <button type="button" className="button" onClick={refresh}>
            Refresh
          </button>
        </div>
      </header>

      {error ? (
        <div className="banner error">
          <span>{error}</span>
          <button type="button" className="banner-close" onClick={() => setError(null)}>
            ✕
          </button>
        </div>
      ) : null}

      <div className="layout">
        <aside className="sidebar">
          <ControlPanel devices={devices} onChanged={refresh} />
          <DevicePanel devices={devices} />
          <CaptureList
            captures={captures}
            selected={captureId}
            loading={loadingCaptures}
            onSelect={selectCapture}
          />
        </aside>
        <main className="main">
          {detail ? (
            <CaptureView
              detail={detail}
              segments={segments}
              selectedSegment={segmentId}
              onSelectSegment={setSegmentId}
            />
          ) : loadingDetail ? (
            <div className="empty">Loading capture…</div>
          ) : (
            <div className="empty hero">
              <h1>Select a capture</h1>
              <p>
                Captures are created by the server when a device starts recording over MQTT. Pick
                one on the left to inspect its segments, play the audio, and view metrics.
              </p>
              <p className="muted small">
                MQTT owns start/stop by default; the WebSocket data plane connects on demand.
              </p>
            </div>
          )}
        </main>
      </div>
    </div>
  )
}
