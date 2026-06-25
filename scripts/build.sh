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

# --- configure (once) + build -----------------------------------------------
# Configure only when the build tree is missing: a no-op re-configure
# regenerates threepp's generated sources and forces a needless relink on every
# run. ninja still re-runs cmake automatically when CMakeLists.txt changes, so
# incremental rebuilds stay correct -- repeat `pixi run build` is now a no-op.
if [[ ! -f build/build.ninja ]]; then
    emcmake cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
fi

# Full parallelism (-j nproc). The -O3 compile of threepp's ~180 files is heavy;
# if a single parallel job is killed (e.g. OOM on a smaller machine) the static
# library archive can fail with a "missing object" error. ninja is incremental
# and idempotent, so retry a few times -- each pass only rebuilds what is missing
# (so memory pressure drops sharply) and the build converges.
JOBS="$(nproc)"
build_ok=0
for attempt in 1 2 3; do
    if cmake --build build -j"${JOBS}"; then
        build_ok=1
        break
    fi
    echo ">> build attempt ${attempt} failed; retrying (ninja resumes only what is missing)..." >&2
done
if [[ "${build_ok}" -ne 1 ]]; then
    echo "ERROR: build failed after 3 attempts. See the log above." >&2
    exit 1
fi

# --- stage web bundle -------------------------------------------------------
mkdir -p dist
cp build/spike.html  dist/index.html
cp build/spike.js    dist/spike.js
cp build/spike.wasm  dist/spike.wasm
cp web/stats.module.js dist/stats.module.js

echo
echo "Built. Serve it with:  pixi run serve   (http://localhost:8016/)"
