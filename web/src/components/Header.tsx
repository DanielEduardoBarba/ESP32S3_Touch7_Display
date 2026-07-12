import type { StatusResponse } from '../lib/api'

interface Props {
  status: StatusResponse | null
}

function wifiBadgeColor(state?: string) {
  switch (state) {
    case 'connected':
      return 'bg-emerald-500/20 text-emerald-400 ring-emerald-500/30'
    case 'connecting':
      return 'bg-amber-500/20 text-amber-400 ring-amber-500/30'
    case 'failed':
      return 'bg-red-500/20 text-red-400 ring-red-500/30'
    default:
      return 'bg-gray-500/20 text-gray-400 ring-gray-500/30'
  }
}

export function Header({ status }: Props) {
  const wifiState = status?.wifi.state
  return (
    <header className="flex items-center justify-between border-b border-white/10 bg-[#1c2128] px-6 py-4">
      <div className="flex items-center gap-3">
        <div className="flex h-9 w-9 items-center justify-center rounded-full bg-white/10 text-lg">
          🏠
        </div>
        <div>
          <h1 className="text-lg font-semibold leading-tight text-white">touch-esp32</h1>
          <p className="text-xs text-gray-400">Waveshare ESP32-S3 Touch LCD 7</p>
        </div>
      </div>

      <span
        className={`rounded-full px-3 py-1 text-xs font-medium ring-1 ${wifiBadgeColor(wifiState)}`}
      >
        {wifiState === 'connected' && status
          ? `Wi-Fi: ${status.wifi.ssid} (${status.wifi.ip})`
          : wifiState === 'connecting'
            ? 'Wi-Fi: connecting…'
            : wifiState === 'failed'
              ? 'Wi-Fi: connection failed'
              : 'Wi-Fi: disconnected'}
      </span>
    </header>
  )
}
