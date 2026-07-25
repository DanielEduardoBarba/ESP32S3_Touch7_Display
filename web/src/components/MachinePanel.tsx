import { useEffect, useState, type ReactNode } from 'react'
import {
  DIAL_MAX,
  MODES,
  SETPOINT_MAX,
  SETPOINT_MIN,
  SETPOINT_STEP,
  SPEED_MAX,
  TOGGLES,
  useMachineSocket,
} from '../lib/useMachineSocket'

/** Card shell, matching the touchscreen scene's look. */
function Card({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="rounded-xl border border-white/10 bg-[#1c2128] p-5">
      <h2 className="mb-3 text-sm font-medium text-gray-400">{title}</h2>
      {children}
    </div>
  )
}

/**
 * A range input whose value is only transmitted on release, exactly like
 * the touchscreen only transmits on LV_EVENT_RELEASED -- not on every
 * intermediate tick while dragging. While the user is actively dragging,
 * their in-progress value is shown instead of snapping back to whatever the
 * device last reported, otherwise a state update arriving mid-drag would
 * yank the slider out from under their finger.
 */
function CommitSlider({
  value,
  max,
  onCommit,
}: {
  value: number
  max: number
  onCommit: (value: number) => void
}) {
  const [dragging, setDragging] = useState(false)
  const [local, setLocal] = useState(value)

  useEffect(() => {
    if (!dragging) setLocal(value)
  }, [value, dragging])

  const commit = () => {
    setDragging(false)
    onCommit(local)
  }

  return (
    <div className="flex items-center gap-4">
      <input
        type="range"
        min={0}
        max={max}
        value={local}
        onInput={(e) => {
          setDragging(true)
          setLocal(Number((e.target as HTMLInputElement).value))
        }}
        onMouseUp={commit}
        onTouchEnd={commit}
        onKeyUp={commit}
        className="h-2 flex-1 cursor-pointer accent-emerald-500"
      />
      <span className="w-12 text-right text-lg font-semibold text-white">{local}</span>
    </div>
  )
}

/**
 * The web equivalent of the device's "Machine" scene. Anything done here
 * behaves on the ESP32 exactly as if it were done on the touchscreen itself
 * (see machine_state.h): every control sends the same RS485 packet the
 * touchscreen would, and every control here updates live whenever the
 * device's state changes for any reason (touchscreen or another board over
 * RS485).
 */
export function MachinePanel() {
  const { state, connected, sendDial, sendSpeed, sendSetpoint, sendMode, sendToggle, sendPulse } =
    useMachineSocket()

  // Brief flash of the pulse indicator whenever the counter moves, however
  // it moved (this browser, the touchscreen, or the peer board).
  const [flash, setFlash] = useState(false)
  useEffect(() => {
    if (state.pulse_count === 0) return
    setFlash(true)
    const timer = setTimeout(() => setFlash(false), 450)
    return () => clearTimeout(timer)
  }, [state.pulse_count])

  const stepSetpoint = (delta: number) => {
    const next = Math.min(SETPOINT_MAX, Math.max(SETPOINT_MIN, state.setpoint + delta))
    sendSetpoint(next)
  }

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <p className="text-xs text-gray-500">
          Every control below is synced live with the device's touchscreen and any other board
          connected over RS485.
        </p>
        <span className={connected ? 'text-xs text-emerald-400' : 'text-xs text-gray-500'}>
          {connected ? '● live' : '○ connecting…'}
        </span>
      </div>

      <div className="grid gap-6 md:grid-cols-2">
        <Card title={`Dial (0-${DIAL_MAX})`}>
          <CommitSlider value={state.dial_value} max={DIAL_MAX} onCommit={sendDial} />
        </Card>

        <Card title={`Speed (0-${SPEED_MAX})`}>
          <CommitSlider value={state.speed} max={SPEED_MAX} onCommit={sendSpeed} />
        </Card>

        <Card title={`Setpoint (${SETPOINT_MIN}..${SETPOINT_MAX})`}>
          <div className="flex items-center gap-4">
            <button
              type="button"
              onClick={() => stepSetpoint(-SETPOINT_STEP)}
              className="h-10 w-12 rounded-lg bg-white/10 text-xl text-white hover:bg-white/20"
            >
              −
            </button>
            <span className="flex-1 text-center text-2xl font-semibold text-white">
              {state.setpoint}
            </span>
            <button
              type="button"
              onClick={() => stepSetpoint(SETPOINT_STEP)}
              className="h-10 w-12 rounded-lg bg-white/10 text-xl text-white hover:bg-white/20"
            >
              +
            </button>
          </div>
        </Card>

        <Card title="Mode">
          <select
            value={state.mode}
            onChange={(e) => sendMode(Number(e.target.value))}
            className="w-full rounded-lg bg-white/10 p-2 text-white"
          >
            {MODES.map((name, index) => (
              <option key={name} value={index} className="bg-[#1c2128]">
                {name}
              </option>
            ))}
          </select>
        </Card>

        <Card title="Toggles">
          <div className="grid grid-cols-2 gap-3">
            {TOGGLES.map((name, id) => (
              <label key={name} className="flex cursor-pointer items-center gap-3">
                <input
                  type="checkbox"
                  checked={state.toggles[id] ?? false}
                  onChange={(e) => sendToggle(id, e.target.checked)}
                  className="h-5 w-5 accent-emerald-500"
                />
                <span className="text-white">{name}</span>
                <span className="text-xs text-gray-500">
                  {state.toggles[id] ? 'ON' : 'OFF'}
                </span>
              </label>
            ))}
          </div>
        </Card>

        <Card title="Pulse (momentary)">
          <div className="flex items-center gap-4">
            <button
              type="button"
              onClick={sendPulse}
              className="rounded-lg bg-emerald-600 px-4 py-2 text-white hover:bg-emerald-500"
            >
              Send pulse
            </button>
            <span
              className={`h-6 w-6 rounded-full transition-colors duration-300 ${
                flash ? 'bg-emerald-400' : 'bg-emerald-900'
              }`}
            />
            <span className="text-sm text-gray-400">{state.pulse_count} pulses</span>
          </div>
        </Card>
      </div>
    </div>
  )
}
