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
  transports: { name: string; label: string; enabled: boolean }[]
}

export interface UpdateInfoResponse {
  running_slot: string
  version: string
  idf_version: string
  compile_time: string
  image_size: number
  ota_state: string
  transfer_state: 'idle' | 'sending' | 'receiving' | 'send_done' | 'receive_done' | 'failed'
  transfer_total: number
  transfer_done: number
  transfer_message: string
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

  updateInfo: () => fetch('/api/update/info').then((r) => json<UpdateInfoResponse>(r)),

  updateSend: () =>
    fetch('/api/update/send', { method: 'POST' }).then((r) => json<{ ok: boolean; error?: string }>(r)),

  updateReboot: () =>
    fetch('/api/update/reboot', { method: 'POST' }).then((r) => json<{ ok: boolean }>(r)),
}

