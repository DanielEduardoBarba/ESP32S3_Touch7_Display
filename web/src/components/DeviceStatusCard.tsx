import type { StatusResponse } from '../lib/api'

interface Props {
  status: StatusResponse | null
}

function formatUptime(ms: number) {
  const totalSeconds = Math.floor(ms / 1000)
  const h = Math.floor(totalSeconds / 3600)
  const m = Math.floor((totalSeconds % 3600) / 60)
  const s = totalSeconds % 60
  return `${h}h ${m}m ${s}s`
}

export function DeviceStatusCard({ status }: Props) {
  return (
    <div className="rounded-xl border border-white/10 bg-[#1c2128] p-5">
      <h2 className="mb-3 text-sm font-medium text-gray-400">Device status</h2>
      {status ? (
        <dl className="grid grid-cols-2 gap-y-2 text-sm">
          <dt className="text-gray-400">Wi-Fi</dt>
          <dd className="text-right text-white">{status.wifi.state}</dd>

          <dt className="text-gray-400">SSID</dt>
          <dd className="text-right text-white">{status.wifi.ssid || '—'}</dd>

          <dt className="text-gray-400">IP address</dt>
          <dd className="text-right text-white">{status.wifi.ip || '—'}</dd>

          <dt className="text-gray-400">Signal</dt>
          <dd className="text-right text-white">{status.wifi.rssi} dBm</dd>

          <dt className="text-gray-400">Web UI source</dt>
          <dd className="text-right text-white">{status.web_root_source}</dd>

          <dt className="text-gray-400">Free heap</dt>
          <dd className="text-right text-white">{Math.round(status.free_heap / 1024)} KB</dd>

          <dt className="text-gray-400">Uptime</dt>
          <dd className="text-right text-white">{formatUptime(status.uptime_ms)}</dd>
        </dl>
      ) : (
        <p className="text-sm text-gray-500">Loading…</p>
      )}
    </div>
  )
}
