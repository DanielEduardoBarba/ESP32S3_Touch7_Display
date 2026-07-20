import { useEffect, useRef, useState } from 'react'
import { api } from '../lib/api'

/**
 * Mirrors the device's "Debug" scene: the same log lines the firmware
 * prints to the serial console, served from its in-RAM ring buffer
 * (last APP_LOG_STORE_MAX lines).
 */
export function DebugPanel() {
  const [lines, setLines] = useState<string[] | null>(null)
  const [follow, setFollow] = useState(true)
  const preRef = useRef<HTMLPreElement>(null)

  useEffect(() => {
    let cancelled = false
    const poll = async () => {
      try {
        const res = await api.logs()
        if (!cancelled) setLines(res.lines)
      } catch {
        /* device busy -- retry next tick */
      }
    }
    poll()
    const id = setInterval(poll, 2000)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  useEffect(() => {
    if (follow && preRef.current) {
      preRef.current.scrollTop = preRef.current.scrollHeight
    }
  }, [lines, follow])

  return (
    <div className="space-y-3">
      <div className="flex items-center justify-between">
        <div>
          <h2 className="text-lg font-semibold text-white">Debug log</h2>
          <p className="text-sm text-gray-400">Live device log (same lines as the serial console)</p>
        </div>
        <label className="flex items-center gap-2 text-sm text-gray-400">
          <input
            type="checkbox"
            checked={follow}
            onChange={(e) => setFollow(e.target.checked)}
          />
          Follow tail
        </label>
      </div>

      <pre
        ref={preRef}
        className="h-[32rem] overflow-auto rounded-lg border border-white/10 bg-black/40 p-4 font-mono text-xs leading-5 text-emerald-300"
      >
        {lines === null ? 'Loading…' : lines.length === 0 ? 'No log lines yet.' : lines.join('\n')}
      </pre>
    </div>
  )
}
