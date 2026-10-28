import type {
  CaptureDetail,
  CaptureSummary,
  DeviceSnapshot,
  SegmentEntry,
} from './types'

const API_BASE = (import.meta.env.VITE_API_BASE as string | undefined) ?? ''

async function getJSON<T>(path: string): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    headers: { Accept: 'application/json' },
  })
  if (!res.ok) {
    let detail = res.statusText
    try {
      const body = (await res.json()) as { error?: string; message?: string }
      detail = body.message || body.error || detail
    } catch {
      /* non-JSON error body */
    }
    throw new Error(`${res.status} ${detail}`)
  }
  return (await res.json()) as T
}

export async function fetchCaptures(): Promise<CaptureSummary[]> {
  const body = await getJSON<{ captures: CaptureSummary[] | null }>('/api/v1/captures')
  return body.captures ?? []
}

export async function fetchCapture(id: string): Promise<CaptureDetail> {
  return getJSON<CaptureDetail>(`/api/v1/captures/${encodeURIComponent(id)}`)
}

export async function fetchSegments(id: string): Promise<SegmentEntry[]> {
  const body = await getJSON<{ segments: SegmentEntry[] | null }>(
    `/api/v1/captures/${encodeURIComponent(id)}/segments`,
  )
  return body.segments ?? []
}

export async function fetchDevices(): Promise<DeviceSnapshot[]> {
  const body = await getJSON<{ devices: DeviceSnapshot[] | null }>('/api/v1/devices')
  return body.devices ?? []
}

export interface CommandResult {
  type?: string
  id?: number
  code?: string
  message?: string
  retryable?: boolean
  result?: unknown
  [key: string]: unknown
}

export class CommandError extends Error {
  code?: string
  status: number

  constructor(message: string, status: number, code?: string) {
    super(message)
    this.name = 'CommandError'
    this.status = status
    this.code = code
  }
}

export async function sendCommand(
  deviceId: string,
  command: Record<string, unknown>,
): Promise<CommandResult> {
  const res = await fetch(`${API_BASE}/api/v1/devices/${encodeURIComponent(deviceId)}/commands`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(command),
  })
  let body: CommandResult = {}
  try {
    body = (await res.json()) as CommandResult
  } catch {
    /* empty/non-JSON error body */
  }
  if (!res.ok) {
    throw new CommandError(
      body.message || (body.error as string | undefined) || res.statusText,
      res.status,
      body.error as string | undefined,
    )
  }
  return body
}

export function segmentFileUrl(captureId: string, segmentId: number, file: string): string {
  return `${API_BASE}/api/v1/captures/${encodeURIComponent(captureId)}/segments/${segmentId}/${file}`
}

export function captureFileUrl(captureId: string, file: string): string {
  return `${API_BASE}/api/v1/captures/${encodeURIComponent(captureId)}/${file}`
}
