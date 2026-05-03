#!/usr/bin/env bash
# Build and install to the emery emulator with WORKER_BASE pointed at the local
# wrangler dev server (localhost:8787) instead of production.
#
# Prerequisites:
#   - wrangler dev running in NextTrainWorker (npm run dev)
#   - emery emulator running: pebble install --emulator emery --logs

set -euo pipefail

INDEX="src/pkjs/index.js"
PROD_URL="https://nexttrainworker.sloccy.workers.dev"
DEV_URL="http://localhost:8787"

# Patch URL
sed -i "s|$PROD_URL|$DEV_URL|g" "$INDEX"

restore() {
  sed -i "s|$DEV_URL|$PROD_URL|g" "$INDEX"
}
trap restore EXIT

echo "==> Building with WORKER_BASE=$DEV_URL"
pebble build

echo "==> Installing to emery emulator"
pebble install --emulator emery

echo "==> Done. Watch the logs with: pebble logs --emulator emery"
