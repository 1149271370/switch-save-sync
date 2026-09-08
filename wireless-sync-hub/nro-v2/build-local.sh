#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

docker run --rm \
  -v "$PWD":/work \
  -w /work \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITA64=/opt/devkitpro/devkitA64 \
  -e PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/portlibs/switch/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  devkitpro/devkita64:latest \
  make

echo "Built: $PWD/SwitchSaveSyncHub.nro"
