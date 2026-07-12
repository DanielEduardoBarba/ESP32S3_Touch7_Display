import { useEffect, useState } from 'react'
import { Header } from './components/Header'
import { DeviceStatusCard } from './components/DeviceStatusCard'
import { WifiPanel } from './components/WifiPanel'
import { api, type StatusResponse } from './lib/api'

function App() {
  const [status, setStatus] = useState<StatusResponse | null>(null)

  useEffect(() => {
    let cancelled = false
    const poll = async () => {
      try {
        const s = await api.status()
        if (!cancelled) setStatus(s)
      } catch {
        // Device may be mid-reboot (e.g. right after a Wi-Fi change); ignore
        // and just try again on the next tick.
      }
    }
    poll()
    const id = setInterval(poll, 3000)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  return (
    <div className="flex min-h-full flex-col">
      <Header status={status} />
      <main className="mx-auto grid w-full max-w-4xl flex-1 grid-cols-1 gap-6 p-6 md:grid-cols-2">
        <DeviceStatusCard status={status} />
        <WifiPanel />
      </main>
      <footer className="px-6 pb-4 text-center text-xs text-gray-600">
        Served directly from the device's {status?.web_root_source ?? '…'} storage on port 80.
      </footer>
    </div>
  )
}

export default App
