import { useEffect, useState } from 'react'
import { api, type WifiAp } from '../lib/api'

export function WifiPanel() {
  const [networks, setNetworks] = useState<WifiAp[]>([])
  const [scanning, setScanning] = useState(false)
  const [selected, setSelected] = useState<WifiAp | null>(null)
  const [ssid, setSsid] = useState('')
  const [password, setPassword] = useState('')
  const [message, setMessage] = useState('')

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
    const id = setInterval(scan, 5000)
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
    } catch (e) {
      setMessage(e instanceof Error ? e.message : 'Connect failed')
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
        {networks.map((ap) => (
          <li key={ap.ssid}>
            <button
              onClick={() => selectNetwork(ap)}
              className={`flex w-full items-center justify-between rounded-md px-2 py-2 text-left text-sm hover:bg-white/5 ${
                selected?.ssid === ap.ssid ? 'bg-white/10' : ''
              }`}
            >
              <span className="text-white">{ap.ssid}</span>
              <span className="text-xs text-gray-400">
                {ap.secure ? '🔒 ' : ''}
                {ap.rssi} dBm
              </span>
            </button>
          </li>
        ))}
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
        <button
          onClick={connect}
          disabled={!ssid}
          className="w-full rounded-md bg-emerald-600 px-3 py-2 text-sm font-medium text-white hover:bg-emerald-500 disabled:opacity-50"
        >
          Connect
        </button>
        {message && <p className="text-xs text-gray-400">{message}</p>}
      </div>
    </div>
  )
}
