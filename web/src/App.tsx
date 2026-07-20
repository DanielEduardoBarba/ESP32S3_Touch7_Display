import { useEffect, useState } from 'react'
import { DebugPanel } from './components/DebugPanel'
import { Header } from './components/Header'
import { MachinePanel } from './components/MachinePanel'
import { NetworkPanel } from './components/NetworkPanel'
import { PortsPanel } from './components/PortsPanel'
import { UpdatePanel } from './components/UpdatePanel'
import { api, type StatusResponse } from './lib/api'

// Mirrors the scenes on the device's own touchscreen (see
// firmware/app/main/ui/ui_scene_manager.h): "Machine" is the default view.
type Scene = 'machine' | 'network' | 'ports' | 'update' | 'debug'

function App() {
  const [status, setStatus] = useState<StatusResponse | null>(null)
  const [scene, setScene] = useState<Scene>('machine')

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

      <nav className="mx-auto flex w-full max-w-4xl gap-2 px-6 pt-4">
        {(['machine', 'network', 'ports', 'update', 'debug'] as const).map((s) => (
          <button
            key={s}
            onClick={() => setScene(s)}
            className={`rounded-md px-4 py-2 text-sm font-medium capitalize transition-colors ${
              scene === s ? 'bg-emerald-600 text-white' : 'bg-white/5 text-gray-400 hover:bg-white/10'
            }`}
          >
            {s}
          </button>
        ))}
      </nav>

      <main className="mx-auto w-full max-w-4xl flex-1 p-6">
        {scene === 'machine' && <MachinePanel />}
        {scene === 'network' && <NetworkPanel status={status} />}
        {scene === 'ports' && <PortsPanel />}
        {scene === 'update' && <UpdatePanel />}
        {scene === 'debug' && <DebugPanel />}
      </main>

      <footer className="px-6 pb-4 text-center text-xs text-gray-600">
        Served directly from the device's {status?.web_root_source ?? '…'} storage on port 80.
      </footer>
    </div>
  )
}

export default App
