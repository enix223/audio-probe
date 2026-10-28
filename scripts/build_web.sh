#!/usr/bin/env bash
# Build the React frontend into server/web/dist. The Go server embeds that
# directory (server/web/embed.go), so run this before `go build` after changing
# the UI. Node modules are not committed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/server/web"

if [[ ! -d node_modules ]]; then
  echo "installing frontend dependencies..."
  npm install --no-audit --no-fund
fi

npm run build
echo "frontend built: $ROOT/server/web/dist"
