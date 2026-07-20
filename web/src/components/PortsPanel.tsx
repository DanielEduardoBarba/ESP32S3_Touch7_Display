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

  // Poll so changes from any other source (the touchscreen, the peer's baud
  // broadcast, another browser) show up here without a manual reload.
  useEffect(() => {
    refresh()
    const id = setInterval(refresh, 3000)
    return () => clearInterval(id)
  }, [])

  const select = async (name: string) => {
    setError(null)
    const res = await api.setPort(name).catch((e) => ({ ok: false, error: String(e) }))
    if (!res.ok) setError(res.error ?? 'Switch failed')
    refresh()
  }

  const selectBaud = async (baud: number) => {
    setError(null)
    // The device broadcasts the change to its peer at the old rate first so
    // both ends of the bus switch together.
    const res = await api.setBaud(baud).catch((e) => ({ ok: false, error: String(e) }))
    if (!res.ok) setError(res.error ?? 'Baud switch failed')
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

      {ports.locked && (
        <p className="rounded-md bg-amber-500/10 px-3 py-2 text-sm text-amber-400">
          Locked: firmware update transfer in progress — transport and baud cannot change.
        </p>
      )}

      <div className="space-y-2">
        {ports.transports.map((t) => {
          const active = ports.active === t.name
          return (
            <button
              key={t.name}
              disabled={!t.enabled || ports.locked}
              onClick={() => select(t.name)}
              className={`flex w-full items-center justify-between rounded-lg border px-4 py-3 text-left text-sm transition-colors ${
                active
                  ? 'border-emerald-500 bg-emerald-500/10 text-white'
                  : t.enabled && !ports.locked
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

      <div>
        <h3 className="text-sm font-semibold text-white">Baud rate</h3>
        <p className="text-xs text-gray-500">
          Switching broadcasts the new rate to the peer first so both boards stay in sync.
        </p>
        <div className="mt-2 flex flex-wrap gap-2">
          {ports.baud_rates.map((rate) => (
            <button
              key={rate}
              disabled={ports.locked}
              onClick={() => selectBaud(rate)}
              className={`rounded-md px-3 py-1.5 text-sm transition-colors ${
                ports.baud === rate
                  ? 'bg-emerald-600 text-white'
                  : ports.locked
                    ? 'cursor-not-allowed bg-white/[0.02] text-gray-600'
                    : 'bg-white/5 text-gray-300 hover:bg-white/10'
              }`}
            >
              {rate.toLocaleString()}
            </button>
          ))}
        </div>
      </div>

      <p className="text-sm text-emerald-400">
        Active: {ports.active} @ {ports.baud.toLocaleString()} baud
      </p>

      {error && <p className="text-sm text-red-400">{error}</p>}
    </div>
  )
}
