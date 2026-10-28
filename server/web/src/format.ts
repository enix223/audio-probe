import type { SegmentMetrics } from './types'

export const DASH = '—'

export function fmtNum(v: number | null | undefined, digits = 1): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return DASH
  return v.toFixed(digits)
}

export function fmtDb(v: number | null | undefined, digits = 1): string {
  if (v === null || v === undefined || !Number.isFinite(v) || v < -190) return DASH
  return `${v.toFixed(digits)} dB`
}

export function fmtDbfs(v: number | null | undefined, digits = 1): string {
  if (v === null || v === undefined || !Number.isFinite(v) || v < -190) return DASH
  return `${v.toFixed(digits)} dBFS`
}

export function fmtPct(v: number | null | undefined, digits = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return DASH
  return `${v.toFixed(digits)}%`
}

export function fmtMs(v: number | null | undefined): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return DASH
  if (v >= 60000) return `${(v / 60000).toFixed(1)} min`
  if (v >= 1000) return `${(v / 1000).toFixed(2)} s`
  return `${v.toFixed(0)} ms`
}

export function fmtInt(v: number | null | undefined): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return DASH
  return Math.round(v).toLocaleString()
}

export function fmtDate(iso?: string): string {
  if (!iso) return DASH
  const d = new Date(iso)
  if (Number.isNaN(d.getTime())) return iso
  return d.toLocaleString()
}

export function fmtRelative(iso?: string): string {
  if (!iso) return DASH
  const d = new Date(iso)
  if (Number.isNaN(d.getTime())) return iso
  const diff = Date.now() - d.getTime()
  const abs = Math.abs(diff)
  const suffix = diff >= 0 ? 'ago' : 'from now'
  if (abs < 60e3) return `${Math.round(abs / 1000)}s ${suffix}`
  if (abs < 3600e3) return `${Math.round(abs / 60e3)}m ${suffix}`
  if (abs < 86400e3) return `${Math.round(abs / 3600e3)}h ${suffix}`
  return `${Math.round(abs / 86400e3)}d ${suffix}`
}

export interface QualityFlag {
  label: string
  tone: 'good' | 'warn' | 'bad'
}

// Thresholds are the tunable starting points from docs/audio-quality-metrics.md
// §12. The UI shows them as hints, not chip-grade conclusions.
export function qualityFlags(m?: SegmentMetrics): QualityFlag[] {
  if (!m) return []
  const flags: QualityFlag[] = []
  const loss = m.loss_pct ?? 0
  if (loss > 0.5) flags.push({ label: `packet loss ${loss.toFixed(1)}%`, tone: 'bad' })
  else if (loss > 0) flags.push({ label: `packet loss ${loss.toFixed(2)}%`, tone: 'warn' })

  const clip = m.clip_pct ?? 0
  if (clip > 0.1) flags.push({ label: `clipping ${clip.toFixed(1)}%`, tone: 'bad' })
  else if (clip > 0) flags.push({ label: `clipping ${clip.toFixed(2)}%`, tone: 'warn' })

  if (m.dropouts && m.dropouts > 0) flags.push({ label: `${m.dropouts} dropout(s)`, tone: 'warn' })

  const snr = m.snr_db
  if (snr !== undefined && Number.isFinite(snr) && snr < 20) {
    flags.push({ label: `low SNR ${snr.toFixed(1)} dB`, tone: 'warn' })
  }

  const rms = m.rms_dbfs
  if (rms !== undefined && Number.isFinite(rms) && rms < -50 && rms > -190) {
    flags.push({ label: `very low level ${rms.toFixed(1)} dBFS`, tone: 'warn' })
  }

  const dc = Math.abs(m.dc_offset_pct_fs ?? 0)
  if (dc > 1) flags.push({ label: `DC offset ${dc.toFixed(1)}% FS`, tone: 'warn' })

  if (flags.length === 0) flags.push({ label: 'no warnings', tone: 'good' })
  return flags
}
