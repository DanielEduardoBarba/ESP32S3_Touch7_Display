export type WifiState = 'disconnected' | 'connecting' | 'connected' | 'failed'

export interface StatusResponse {
  wifi: {
    state: WifiState
    ssid: string
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
}
