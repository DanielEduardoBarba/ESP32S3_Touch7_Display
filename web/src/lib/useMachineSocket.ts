import { useCallback, useEffect, useRef, useState } from 'react'

/**
 * Mirrors the ESP32's `machine_state` (see firmware/app/main/machine_state.h):
 * a single source of truth for the dial value + sample toggle, kept in sync
 * across this browser tab, the device's own touchscreen, and any other
 * board connected over RS485 -- all through one WebSocket connection to the
 * device (`/ws`).
 */
export interface MachineState {
  dial_value: number
  toggle_state: boolean
}

const RECONNECT_DELAY_MS = 2000

export function useMachineSocket() {
  const [state, setState] = useState<MachineState>({ dial_value: 0, toggle_state: false })
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
          const data = JSON.parse(event.data) as MachineState
          if (!cancelled) setState(data)
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

  const sendDial = useCallback((value: number) => {
    const socket = socketRef.current
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({ type: 'dial', value }))
    }
  }, [])

  const sendToggle = useCallback((toggleState: boolean) => {
    const socket = socketRef.current
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({ type: 'toggle', id: 0, state: toggleState }))
    }
  }, [])

  return { state, connected, sendDial, sendToggle }
}
