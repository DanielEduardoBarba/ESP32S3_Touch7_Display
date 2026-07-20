import { useEffect, useState } from 'react'
import { api, type UpdateInfoResponse } from '../lib/api'

function formatDuration(seconds: number): string {
  const s = Math.max(0, Math.round(seconds))
  return s >= 60 ? `${Math.floor(s / 60)}m ${String(s % 60).padStart(2, '0')}s` : `${s}s`
}

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
  const [syncing, setSyncing] = useState(false)

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
  const percent =
    info.transfer_total > 0 ? (100 * info.transfer_done) / info.transfer_total : 0
  const showDetail = info.transfer_total > 0 && info.transfer_state !== 'idle' && !failed
  const controlsDisabled = busy || received

  const sync = async () => {
    setError(null)
    setSyncing(true)
    await api.updateSync().catch((e) => setError(String(e)))
    // The device answers (or times out) within its sync window; the polled
    // info reflects the outcome, so just release the button after it.
    setTimeout(() => setSyncing(false), 3500)
  }

  const push = async () => {
    setError(null)
    if (info.peer_no_response) {
      if (
        !window.confirm(
          "The peer never replied to the version sync, so its version is unknown " +
            '(it may run an older firmware without this feature). Push v' +
            info.app_version +
            ' to it?'
        )
      )
        return
    } else if (info.peer_compare !== 'peer_older') {
      const note =
        info.peer_compare === 'same'
          ? 'The peer already runs the same version.'
          : `This would DOWNGRADE the peer from v${info.peer_version} to v${info.app_version}.`
      if (!window.confirm(`${note} Really proceed?`)) return
    }
    const res = await api.updateSend().catch((e) => ({ ok: false, error: String(e) }))
    if (!res.ok) setError(res.error ?? 'Could not start update')
  }

  const pull = async () => {
    setError(null)
    if (info.peer_compare !== 'peer_newer') {
      const note =
        info.peer_compare === 'same'
          ? 'You already run the same version.'
          : `This would DOWNGRADE this device from v${info.app_version} to v${info.peer_version}.`
      if (!window.confirm(`${note} Really proceed?`)) return
    }
    const res = await api.updatePull().catch((e) => ({ ok: false, error: String(e) }))
    if (!res.ok) setError(res.error ?? 'Could not start pull')
  }

  const reboot = async () => {
    await api.updateReboot().catch(() => {})
  }

  const compareText: Record<string, string> = {
    peer_newer: `Peer: v${info.peer_version} — you are OUT OF DATE`,
    peer_older: `Peer: v${info.peer_version} — you are AHEAD`,
    same: `Peer: v${info.peer_version} — same version`,
    unknown: info.peer_no_response
      ? 'Peer did not respond to the version sync — it is offline or runs an older firmware ' +
        'without this feature. You can still push your update; "Pull" is disabled because ' +
        "the peer's version cannot be verified."
      : 'Peer version unknown — press "Sync peer device"',
  }

  const showTransferButtons = (info.peer_known || info.peer_no_response) && !received

  return (
    <div className="space-y-6">
      <div>
        <h2 className="text-lg font-semibold text-white">Firmware</h2>
        <dl className="mt-3 grid grid-cols-2 gap-x-6 gap-y-2 text-sm">
          {(
            [
              ['Running slot', info.running_slot],
              ['App version', `v${info.app_version}`],
              ['Build', info.version],
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
          onClick={sync}
          disabled={controlsDisabled || syncing}
          className="rounded-lg bg-emerald-600 px-5 py-2.5 text-sm font-medium text-white transition-colors hover:bg-emerald-500 disabled:cursor-not-allowed disabled:bg-white/10 disabled:text-gray-500"
        >
          {syncing ? 'Syncing…' : 'Sync peer device'}
        </button>

        <p className="text-sm text-gray-400">
          {received
            ? 'Update received and verified. Reboot when ready — sending is disabled until this device runs the new image.'
            : compareText[info.peer_compare]}
        </p>

        {showTransferButtons && (
          <div className="flex flex-wrap gap-3">
            <button
              onClick={push}
              disabled={controlsDisabled}
              className={`rounded-lg px-5 py-2.5 text-sm font-medium transition-colors disabled:cursor-not-allowed disabled:bg-white/10 disabled:text-gray-500 ${
                info.peer_compare === 'peer_older' || info.peer_no_response
                  ? 'bg-emerald-600 text-white hover:bg-emerald-500'
                  : 'bg-white/10 text-gray-300 hover:bg-white/20'
              }`}
            >
              Push my update (v{info.app_version})
              {info.peer_known &&
                info.peer_compare !== 'peer_older' &&
                (info.peer_compare === 'same' ? ' — same' : ' — their downgrade')}
            </button>
            <button
              onClick={pull}
              disabled={controlsDisabled || info.peer_no_response}
              className={`rounded-lg px-5 py-2.5 text-sm font-medium transition-colors disabled:cursor-not-allowed disabled:bg-white/10 disabled:text-gray-500 ${
                info.peer_compare === 'peer_newer'
                  ? 'bg-emerald-600 text-white hover:bg-emerald-500'
                  : 'bg-white/10 text-gray-300 hover:bg-white/20'
              }`}
            >
              {info.peer_no_response
                ? 'Pull their update — unavailable (no version reply)'
                : `Pull their update (v${info.peer_version})` +
                  (info.peer_compare !== 'peer_newer'
                    ? info.peer_compare === 'same'
                      ? ' — same'
                      : ' — your downgrade'
                    : '')}
            </button>
          </div>
        )}

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
              style={{ width: `${percent}%` }}
            />
          </div>
        )}

        {showDetail && (
          <p className="font-mono text-sm text-gray-300">
            {percent.toFixed(1)}% &nbsp;·&nbsp; {info.transfer_done.toLocaleString()} /{' '}
            {info.transfer_total.toLocaleString()} bytes &nbsp;·&nbsp;{' '}
            {(info.transfer_bps / 1024).toFixed(1)} KB/s
            <br />
            Elapsed: {formatDuration(info.transfer_elapsed_ms / 1000)} &nbsp;·&nbsp; Remaining:{' '}
            {busy && info.transfer_bps > 0 && info.transfer_total > info.transfer_done
              ? `~${formatDuration((info.transfer_total - info.transfer_done) / info.transfer_bps)}`
              : busy
                ? '…'
                : 'done'}
          </p>
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
