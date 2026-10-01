#!/bin/bash
set -e

# Local AppImage build. tauri.conf.json enables createUpdaterArtifacts for CI,
# which requires TAURI_SIGNING_PRIVATE_KEY; disable it here unless a key is set.
if [ -z "$TAURI_SIGNING_PRIVATE_KEY" ]; then
  UPDATER_OVERRIDE=(--config '{"bundle":{"createUpdaterArtifacts":false}}')
fi

pnpm run tauri:build:all-in-one --bundles appimage "${UPDATER_OVERRIDE[@]}"
