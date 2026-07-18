import type { StatusResponse } from '../lib/api'
import { DeviceStatusCard } from './DeviceStatusCard'
import { WifiPanel } from './WifiPanel'

/**
 * The web equivalent of the device's "Network" scene: device/WiFi status
 * plus the WiFi scan/connect panel (same content that used to be the
 * only thing on this page, before the Machine scene was added).
 */
export function NetworkPanel({ status }: { status: StatusResponse | null }) {
  return (
    <div className="grid grid-cols-1 gap-6 md:grid-cols-2">
      <DeviceStatusCard status={status} />
      <WifiPanel />
    </div>
  )
}
