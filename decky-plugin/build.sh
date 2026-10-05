#!/usr/bin/env bash
# Builds the installable plugin zip: out/ds5-bridge.zip
#
#   ./build.sh            build daemon + frontend, run the daemon tests, package
#   ./build.sh --no-test  skip the tests
#
# Needs: cmake, a C/C++ compiler, python3, node + pnpm. Run it on the Deck
# (desktop mode) or any x86_64 Linux box. For maximum compatibility with
# SteamOS build the daemon with the Decky CLI (backend/Dockerfile) instead.
set -euo pipefail
cd "$(dirname "$0")"

RUN_TESTS=1
[[ "${1:-}" == "--no-test" ]] && RUN_TESTS=0

# The daemon uses the firmware's Opus submodule when it is checked out.
if [[ -f ../.gitmodules && ! -f ../lib/opus/CMakeLists.txt ]]; then
  git -C .. submodule update --init lib/opus || true
fi

echo "==> building ds5bridged"
make -C backend
if [[ $RUN_TESTS == 1 ]]; then
  echo "==> testing ds5bridged"
  make -C backend test
fi

echo "==> building the frontend"
pnpm install --frozen-lockfile 2>/dev/null || pnpm install
pnpm run build

echo "==> packaging"
STAGE=out/ds5-bridge
rm -rf out
mkdir -p "$STAGE/bin" "$STAGE/dist"
cp backend/out/ds5bridged "$STAGE/bin/"
cp dist/index.js "$STAGE/dist/"
cp main.py plugin.json package.json README.md LICENSE "$STAGE/"
(cd out && zip -qr ds5-bridge.zip ds5-bridge)
echo "==> out/ds5-bridge.zip"
