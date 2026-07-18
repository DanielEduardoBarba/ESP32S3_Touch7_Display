import { useEffect, useState } from 'react'
import { api, type StatusResponse, type WifiAp } from '../lib/api'

export function WifiPanel() {
  const [networks, setNetworks] = useState<WifiAp[]>([])
  const [scanning, setScanning] = useState(false)
  const [selected, setSelected] = useState<WifiAp | null>(null)
  const [ssid, setSsid] = useState('')
  const [password, setPassword] = useState('')
  const [message, setMessage] = useState('')
  const [status, setStatus] = useState<StatusResponse['wifi'] | null>(null)

  const connectedSsid = status?.state === 'connected' ? status.ssid : ''

  const refreshStatus = async () => {
    try {
      const res = await api.status()
      setStatus(res.wifi)
    } catch {
      // Status polling failures are non-critical (e.g. mid-reconnect); ignore.
    }
  }

  const scan = async () => {
    setScanning(true)
    try {
      const results = await api.wifiScan()
      setNetworks(results)
    } catch (e) {
      setMessage(e instanceof Error ? e.message : 'Scan failed')
    } finally {
      setScanning(false)
    }
  }

  useEffect(() => {
    scan()
    refreshStatus()
    const id = setInterval(() => {
      scan()
      refreshStatus()
    }, 5000)
    return () => clearInterval(id)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const selectNetwork = (ap: WifiAp) => {
    setSelected(ap)
    setSsid(ap.ssid)
    setPassword('')
    setMessage('')
  }

  const connect = async () => {
    if (!ssid) return
    setMessage('Connecting…')
    try {
      await api.wifiConnect(ssid, password)
      setMessage(`Requested connection to "${ssid}". Check the status card above.`)
      refreshStatus()
    } catch (e) {
      setMessage(e instanceof Error ? e.message : 'Connect failed')
    }
  }

  const forget = async () => {
    setMessage('Forgetting network…')
    try {
      await api.wifiForget()
      setMessage('Forgotten. Disconnected.')
      refreshStatus()
    } catch (e) {
      setMessage(e instanceof Error ? e.message : 'Forget failed')
    }
  }

  return (
    <div className="rounded-xl border border-white/10 bg-[#1c2128] p-5">
      <div className="mb-3 flex items-center justify-between">
        <h2 className="text-sm font-medium text-gray-400">Wi-Fi networks</h2>
        <button
          onClick={scan}
          disabled={scanning}
          className="rounded-md bg-white/10 px-3 py-1 text-xs text-white hover:bg-white/20 disabled:opacity-50"
        >
          {scanning ? 'Scanning…' : 'Rescan'}
        </button>
      </div>

      <ul className="mb-4 max-h-56 divide-y divide-white/5 overflow-y-auto">
        {networks.length === 0 && <li className="py-2 text-sm text-gray-500">No networks found yet.</li>}
        {networks.map((ap) => {
          const isConnected = connectedSsid !== '' && connectedSsid === ap.ssid
          return (
            <li key={ap.ssid}>
              <button
                onClick={() => selectNetwork(ap)}
                className={`flex w-full items-center justify-between rounded-md border px-2 py-2 text-left text-sm hover:bg-white/5 ${
                  isConnected
                    ? 'border-emerald-500 bg-emerald-500/10'
                    : selected?.ssid === ap.ssid
                      ? 'border-transparent bg-white/10'
                      : 'border-transparent'
                }`}
              >
                <span className="flex items-center gap-2 text-white">
                  {ap.ssid}
                  {isConnected && (
                    <span className="rounded-full bg-emerald-600/20 px-1.5 py-0.5 text-[10px] font-medium text-emerald-400">
                      Connected
                    </span>
                  )}
                </span>
                <span className="text-xs text-gray-400">
                  {ap.secure ? '🔒 ' : ''}
                  {ap.rssi} dBm
                </span>
              </button>
            </li>
          )
        })}
      </ul>

      <div className="space-y-2 border-t border-white/10 pt-4">
        <label className="block text-xs text-gray-400">
          SSID
          <input
            value={ssid}
            onChange={(e) => setSsid(e.target.value)}
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1.5 text-sm text-white focus:border-emerald-500 focus:outline-none"
            placeholder="Network name"
          />
        </label>
        <label className="block text-xs text-gray-400">
          Password
          <input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-2 py-1.5 text-sm text-white focus:border-emerald-500 focus:outline-none"
            placeholder="Password"
          />
        </label>
        <div className="flex gap-2">
          <button
            onClick={connect}
            disabled={!ssid}
            className="flex-1 rounded-md bg-emerald-600 px-3 py-2 text-sm font-medium text-white hover:bg-emerald-500 disabled:opacity-50"
          >
            Connect
          </button>
          {connectedSsid !== '' && connectedSsid === ssid && (
            <button
              onClick={forget}
              className="flex-1 rounded-md bg-red-600/80 px-3 py-2 text-sm font-medium text-white hover:bg-red-600"
            >
              Forget
            </button>
          )}
        </div>
        {message && <p className="text-xs text-gray-400">{message}</p>}
      </div>
    </div>
  )
}

