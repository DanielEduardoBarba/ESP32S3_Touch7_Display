import { useCallback, useEffect, useRef, useState } from 'react'

/**
 * Mirrors the ESP32's `machine_state` (see firmware/app/main/machine_state.h):
 * a single source of truth for every Machine-scene control, kept in sync
 * across this browser tab, the device's own touchscreen, and any other
 * board connected over RS485 -- all through one WebSocket connection to the
 * device (`/ws`).
 */
export interface MachineState {
  dial_value: number
  speed: number
  setpoint: number
  mode: number
  /** One entry per on/off switch; index matches TOGGLES below. */
  toggles: boolean[]
  pulse_count: number
}

/** Must match machine_state.h (ranges, toggle ids and mode order). */
export const DIAL_MAX = 100
export const SPEED_MAX = 1000
export const SETPOINT_MIN = -50
export const SETPOINT_MAX = 150
export const SETPOINT_STEP = 5
export const TOGGLES = ['Sample', 'Pump', 'Valve', 'Light']
export const MODES = ['Idle', 'Manual', 'Auto', 'Service']

const INITIAL_STATE: MachineState = {
  dial_value: 0,
  speed: 0,
  setpoint: 0,
  mode: 0,
  toggles: TOGGLES.map(() => false),
  pulse_count: 0,
}

const RECONNECT_DELAY_MS = 2000

export function useMachineSocket() {
  const [state, setState] = useState<MachineState>(INITIAL_STATE)
  const [connected, setConnected] = useState(false)
  const socketRef = useRef<WebSocket | null>(null)

  useEffect(() => {
    let cancelled = false
    let reconnectTimer: ReturnType<typeof setTimeout>

    const connect = () => {
      const socket = new WebSocket(`ws://${window.location.host}/ws`)
      socketRef.current = socket

      socket.onopen = () => {
        if (!cancelled) setConnected(true)
      }

      socket.onclose = () => {
        if (cancelled) return
        setConnected(false)
        // The device may still be booting, or the WiFi link may have
        // blipped -- keep trying instead of giving up.
        reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS)
      }

      socket.onerror = () => {
        socket.close()
      }

      socket.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data) as Partial<MachineState>
          if (!cancelled) setState({ ...INITIAL_STATE, ...data })
        } catch {
          // Ignore malformed messages rather than crashing the UI.
        }
      }
    }

    connect()
    return () => {
      cancelled = true
      clearTimeout(reconnectTimer)
      socketRef.current?.close()
    }
  }, [])

  const send = useCallback((message: Record<string, unknown>) => {
    const socket = socketRef.current
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify(message))
    }
  }, [])

  const sendDial = useCallback((value: number) => send({ type: 'dial', value }), [send])
  const sendSpeed = useCallback((value: number) => send({ type: 'speed', value }), [send])
  const sendSetpoint = useCallback((value: number) => send({ type: 'setpoint', value }), [send])
  const sendMode = useCallback((value: number) => send({ type: 'mode', value }), [send])
  const sendToggle = useCallback(
    (id: number, toggleState: boolean) => send({ type: 'toggle', id, state: toggleState }),
    [send],
  )
  const sendPulse = useCallback(() => send({ type: 'pulse' }), [send])

  return { state, connected, sendDial, sendSpeed, sendSetpoint, sendMode, sendToggle, sendPulse }
}
