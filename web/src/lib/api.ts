export type WifiState = 'disconnected' | 'connecting' | 'connected' | 'failed'

export interface StatusResponse {
  wifi: {
    state: WifiState
    ssid: string
    saved_ssid: string
    ip: string
    rssi: number
  }
  web_root_source: 'none' | 'spiffs' | 'sdcard'
  uptime_ms: number
  free_heap: number
}

export interface WifiAp {
  ssid: string
  rssi: number
  secure: boolean
}

export interface PortsResponse {
  active: string
  baud: number
  baud_rates: number[]
  locked: boolean
  transports: { name: string; label: string; enabled: boolean }[]
}

export type PeerCompare = 'unknown' | 'peer_newer' | 'same' | 'peer_older'

export interface UpdateInfoResponse {
  running_slot: string
  app_version: string
  version: string
  idf_version: string
  compile_time: string
  image_size: number
  ota_state: string
  transfer_state: 'idle' | 'sending' | 'receiving' | 'send_done' | 'receive_done' | 'failed'
  transfer_total: number
  transfer_done: number
  transfer_bps: number
  transfer_elapsed_ms: number
  transfer_message: string
  peer_known: boolean
  peer_no_response: boolean
  peer_version: string
  peer_compare: PeerCompare
}

async function json<T>(res: Response): Promise<T> {
  if (!res.ok) {
    const text = await res.text().catch(() => res.statusText)
    throw new Error(text || `HTTP ${res.status}`)
  }
  return res.json() as Promise<T>
}

export const api = {
  status: () => fetch('/api/status').then((r) => json<StatusResponse>(r)),

  wifiScan: () => fetch('/api/wifi/scan').then((r) => json<WifiAp[]>(r)),

  wifiConnect: (ssid: string, password: string) =>
    fetch('/api/wifi/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid, password }),
    }).then((r) => json<{ connecting: boolean }>(r)),

  wifiForget: () =>
    fetch('/api/wifi/forget', { method: 'POST' }).then((r) => json<{ forgotten: boolean }>(r)),

  ports: () => fetch('/api/ports').then((r) => json<PortsResponse>(r)),

  setPort: (transport: string) =>
    fetch('/api/ports', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ transport }),
    }).then((r) => json<{ ok: boolean; error?: string }>(r)),

  setBaud: (baud: number) =>
    fetch('/api/ports', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ baud }),
    }).then((r) => json<{ ok: boolean; error?: string }>(r)),

  updateInfo: () => fetch('/api/update/info').then((r) => json<UpdateInfoResponse>(r)),

  updateSync: () =>
    fetch('/api/update/sync', { method: 'POST' }).then((r) => json<{ ok: boolean }>(r)),

  updateSend: () =>
    fetch('/api/update/send', { method: 'POST' }).then((r) => json<{ ok: boolean; error?: string }>(r)),

  updatePull: () =>
    fetch('/api/update/pull', { method: 'POST' }).then((r) => json<{ ok: boolean; error?: string }>(r)),

  updateReboot: () =>
    fetch('/api/update/reboot', { method: 'POST' }).then((r) => json<{ ok: boolean }>(r)),

  logs: () => fetch('/api/logs').then((r) => json<{ lines: string[] }>(r)),
}

