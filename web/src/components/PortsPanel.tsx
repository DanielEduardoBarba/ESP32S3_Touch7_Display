import { useEffect, useState } from 'react'
import { api, type PortsResponse } from '../lib/api'

/**
 * Mirrors the device's "Ports" scene: pick which peer-to-peer transport
 * (RS485/CAN/I2C/UART) carries machine-sync + firmware-update traffic.
 * Transports that exist in the codebase but are disabled in ports_config.h
 * are shown greyed out, exactly like on the touchscreen.
 */
export function PortsPanel() {
  const [ports, setPorts] = useState<PortsResponse | null>(null)
  const [error, setError] = useState<string | null>(null)

  const refresh = () => {
    api
      .ports()
      .then(setPorts)
      .catch((e) => setError(String(e)))
  }

  useEffect(refresh, [])

  const select = async (name: string) => {
    setError(null)
    const res = await api.setPort(name).catch((e) => ({ ok: false, error: String(e) }))
    if (!res.ok) setError(res.error ?? 'Switch failed')
    refresh()
  }

  if (!ports) {
    return <p className="text-sm text-gray-500">{error ?? 'Loading ports…'}</p>
  }

  return (
    <div className="space-y-4">
      <div>
        <h2 className="text-lg font-semibold text-white">Peer link transport</h2>
        <p className="text-sm text-gray-400">
          Which physical link carries machine sync and firmware updates between boards.
        </p>
      </div>

      <div className="space-y-2">
        {ports.transports.map((t) => {
          const active = ports.active === t.name
          return (
            <button
              key={t.name}
              disabled={!t.enabled}
              onClick={() => select(t.name)}
              className={`flex w-full items-center justify-between rounded-lg border px-4 py-3 text-left text-sm transition-colors ${
                active
                  ? 'border-emerald-500 bg-emerald-500/10 text-white'
                  : t.enabled
                    ? 'border-white/10 bg-white/5 text-gray-300 hover:bg-white/10'
                    : 'cursor-not-allowed border-white/5 bg-white/[0.02] text-gray-600'
              }`}
            >
              <span>{t.label}</span>
              <span className="text-xs">
                {active ? 'active' : t.enabled ? 'available' : 'disabled in ports_config.h'}
              </span>
            </button>
          )
        })}
      </div>

      {error && <p className="text-sm text-red-400">{error}</p>}
    </div>
  )
}
