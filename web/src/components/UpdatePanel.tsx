import { useEffect, useState } from 'react'
import { api, type UpdateInfoResponse } from '../lib/api'

/**
 * Mirrors the device's "Update" scene: shows which OTA slot this device is
 * running (version, size, state), lets you pipe the running firmware to the
 * peer board over the active transport, and -- when THIS device has
 * received a verified update -- surfaces the red reboot button and disables
 * further sends, exactly like the touchscreen.
 */
export function UpdatePanel() {
  const [info, setInfo] = useState<UpdateInfoResponse | null>(null)
  const [error, setError] = useState<string | null>(null)

  // Poll while a transfer is running so the progress bar moves.
  useEffect(() => {
    let cancelled = false
    const poll = async () => {
      try {
        const i = await api.updateInfo()
        if (!cancelled) setInfo(i)
      } catch {
        /* device busy/rebooting -- retry next tick */
      }
    }
    poll()
    const id = setInterval(poll, 1500)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  if (!info) {
    return <p className="text-sm text-gray-500">{error ?? 'Loading update info…'}</p>
  }

  const busy = info.transfer_state === 'sending' || info.transfer_state === 'receiving'
  const received = info.transfer_state === 'receive_done'
  const failed = info.transfer_state === 'failed'
  const progress =
    info.transfer_total > 0 ? Math.round((100 * info.transfer_done) / info.transfer_total) : 0

  const send = async () => {
    setError(null)
    const res = await api.updateSend().catch((e) => ({ ok: false, error: String(e) }))
    if (!res.ok) setError(res.error ?? 'Could not start update')
  }

  const reboot = async () => {
    await api.updateReboot().catch(() => {})
  }

  return (
    <div className="space-y-6">
      <div>
        <h2 className="text-lg font-semibold text-white">Firmware</h2>
        <dl className="mt-3 grid grid-cols-2 gap-x-6 gap-y-2 text-sm">
          {(
            [
              ['Running slot', info.running_slot],
              ['App version', info.version],
              ['Image size', `${info.image_size.toLocaleString()} bytes`],
              ['OTA state', info.ota_state],
              ['ESP-IDF', info.idf_version],
              ['Compiled', info.compile_time],
            ] as const
          ).map(([k, v]) => (
            <div key={k} className="contents">
              <dt className="text-gray-500">{k}</dt>
              <dd className="text-gray-200">{v}</dd>
            </div>
          ))}
        </dl>
      </div>

      <div className="space-y-3">
        <button
          onClick={send}
          disabled={busy || received}
          className="rounded-lg bg-emerald-600 px-5 py-2.5 text-sm font-medium text-white transition-colors hover:bg-emerald-500 disabled:cursor-not-allowed disabled:bg-white/10 disabled:text-gray-500"
        >
          {busy ? 'Transfer in progress…' : 'Update peer device'}
        </button>

        {received && (
          <button
            onClick={reboot}
            className="block rounded-lg bg-red-600 px-5 py-2.5 text-sm font-medium text-white transition-colors hover:bg-red-500"
          >
            Reboot into update
          </button>
        )}

        {(busy || info.transfer_total > 0) && (
          <div className="h-2 w-full overflow-hidden rounded bg-white/10">
            <div
              className="h-full rounded bg-emerald-500 transition-all"
              style={{ width: `${progress}%` }}
            />
          </div>
        )}

        <p
          className={`text-sm ${
            failed ? 'text-red-400' : received ? 'text-emerald-400' : 'text-gray-400'
          }`}
        >
          {info.transfer_message || 'Idle'}
        </p>
        {error && <p className="text-sm text-red-400">{error}</p>}
      </div>
    </div>
  )
}
