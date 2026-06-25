#!/usr/bin/env bash
# Build the threepp WebGL spike for the browser and stage it into dist/.
#
#   1. fetch the threepp submodule (pinned commit)
#   2. apply the emscripten WebGL-context patch (idempotent)
#   3. configure + build with emscripten
#   4. copy the web bundle (index.html + .js + .wasm + stats.module.js) to dist/
#
# emscripten (emcc/emcmake) is NOT vendored -- install it once from
# https://emscripten.org and activate it (`source <emsdk>/emsdk_env.sh`) before
# running, or set EMSDK to your emsdk directory.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# --- emscripten toolchain ---------------------------------------------------
if ! command -v emcmake >/dev/null 2>&1; then
    if [[ -n "${EMSDK:-}" && -f "$EMSDK/emsdk_env.sh" ]]; then
        # shellcheck disable=SC1091
        source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
    fi
fi
if ! command -v emcmake >/dev/null 2>&1; then
    echo "ERROR: emcmake not found. Install emscripten (https://emscripten.org)" >&2
    echo "       and activate it:  source <emsdk>/emsdk_env.sh   (or set EMSDK)." >&2
    exit 1
fi

# --- submodule --------------------------------------------------------------
git submodule update --init --recursive

# --- patch threepp (idempotent) ---------------------------------------------
PATCH="$ROOT/patches/threepp-emscripten-webgl-context.patch"
if git -C third_party/threepp apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    echo "threepp patch already applied."
else
    git -C third_party/threepp apply "$PATCH"
    echo "threepp patch applied."
fi

# --- configure + build ------------------------------------------------------
emcmake cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# --- stage web bundle -------------------------------------------------------
mkdir -p dist
cp build/spike.html  dist/index.html
cp build/spike.js    dist/spike.js
cp build/spike.wasm  dist/spike.wasm
cp web/stats.module.js dist/stats.module.js

echo
echo "Built. Serve it with:  pixi run serve   (http://localhost:8016/)"
