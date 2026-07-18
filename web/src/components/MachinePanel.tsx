import { useEffect, useState } from 'react'
import { useMachineSocket } from '../lib/useMachineSocket'

/**
 * The web equivalent of the device's "Machine" scene. Anything done here
 * behaves on the ESP32 exactly as if it were done on the touchscreen itself
 * (see machine_state.h): dragging the dial and releasing sends the same
 * RS485 packet the touchscreen would, and the dial/toggle here update live
 * whenever the device's own state changes for any reason (touchscreen or
 * another board over RS485).
 */
export function MachinePanel() {
  const { state, connected, sendDial, sendToggle } = useMachineSocket()

  // While the user is actively dragging the slider, show their in-progress
  // value instead of snapping back to whatever the device last reported --
  // otherwise a state update arriving mid-drag would yank the slider out
  // from under their finger.
  const [dragging, setDragging] = useState(false)
  const [localDial, setLocalDial] = useState(state.dial_value)

  useEffect(() => {
    if (!dragging) setLocalDial(state.dial_value)
  }, [state.dial_value, dragging])

  const commitDial = () => {
    setDragging(false)
    sendDial(localDial)
  }

  return (
    <div className="space-y-6">
      <div className="rounded-xl border border-white/10 bg-[#1c2128] p-5">
        <div className="mb-3 flex items-center justify-between">
          <h2 className="text-sm font-medium text-gray-400">Dial (0-100)</h2>
          <span className={connected ? 'text-xs text-emerald-400' : 'text-xs text-gray-500'}>
            {connected ? '● live' : '○ connecting…'}
          </span>
        </div>

        <div className="flex items-center gap-4">
          <input
            type="range"
            min={0}
            max={100}
            value={localDial}
            onInput={(e) => {
              setDragging(true)
              setLocalDial(Number((e.target as HTMLInputElement).value))
            }}
            // Sent on release (mouse/touch/keyboard), just like the
            // touchscreen only transmits on LV_EVENT_RELEASED -- not on
            // every intermediate tick while dragging.
            onMouseUp={commitDial}
            onTouchEnd={commitDial}
            onKeyUp={commitDial}
            className="h-2 flex-1 cursor-pointer accent-emerald-500"
          />
          <span className="w-10 text-right text-lg font-semibold text-white">{localDial}</span>
        </div>

        <p className="mt-3 text-xs text-gray-500">
          Synced live with the device's touchscreen and any other board connected over RS485.
        </p>
      </div>

      <div className="rounded-xl border border-white/10 bg-[#1c2128] p-5">
        <h2 className="mb-3 text-sm font-medium text-gray-400">Sample toggle</h2>
        <label className="flex cursor-pointer items-center gap-3">
          <input
            type="checkbox"
            checked={state.toggle_state}
            onChange={(e) => sendToggle(e.target.checked)}
            className="h-5 w-5 accent-emerald-500"
          />
          <span className="text-white">{state.toggle_state ? 'ON' : 'OFF'}</span>
        </label>
      </div>
    </div>
  )
}
