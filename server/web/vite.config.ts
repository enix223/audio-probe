import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Dev server: proxy API and capture files to the Go server on :8081.
// Production: `npm run build` emits dist/, embedded by server/web/embed.go.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': 'http://127.0.0.1:8081',
      '/data': 'http://127.0.0.1:8081',
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
})
