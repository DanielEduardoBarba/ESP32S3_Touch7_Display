import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react(), tailwindcss()],
  base: './',
  build: {
    // Keep the flashed SPIFFS image small -- this app is served from a
    // microcontroller, not a CDN.
    sourcemap: false,
    assetsInlineLimit: 4096,
  },
  server: {
    proxy: {
      // During `npm run dev`, forward API calls to a real device on your
      // network so you can iterate on the UI without reflashing.
      // Set VITE_DEVICE_HOST=http://<device-ip> when running `npm run dev`.
      '/api': process.env.VITE_DEVICE_HOST || 'http://192.168.4.1',
    },
  },
})
