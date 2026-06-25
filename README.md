## Prerequisites

- [pixi](https://pixi.sh) (provides cmake, ninja, python).
- **emscripten** — not vendored. Install once from
  <https://emscripten.org/docs/getting_started/downloads.html> and activate it
  before building:
  ```bash
  source /path/to/emsdk/emsdk_env.sh   # or: export EMSDK=/path/to/emsdk
  ```

## Build & run

```bash
git clone --recursive <this-repo-url>
cd threecpp-test
pixi run build      # fetches the threepp submodule, patches it, builds to dist/
pixi run serve      # http://localhost:8016/
```

If you cloned without `--recursive`, `pixi run build` initializes the submodule
for you.

## Layout

```
src/main.cpp        the spike (scene, editor, on-demand loop)
web/shell.html      emscripten HTML shell (stats.js panel, red dot, resize hook)
web/stats.module.js the exact stats.js the three.js clone uses
CMakeLists.txt      links the threepp submodule, emscripten WebGL flags
third_party/threepp threepp, as a git submodule (pinned)
patches/            one-line emscripten WebGL-context fix applied to threepp
scripts/build.sh    submodule + patch + emcmake build + stage dist/
scripts/serve.py    static server for dist/
```

## Note on the threepp patch

threepp's current `Canvas.cpp` forces `GLFW_CLIENT_API = GLFW_NO_API` on
emscripten (added for its WebGPU path), which stops emscripten's GLFW from
creating a **WebGL** context — so the OpenGL renderer crashes on startup. The
patch in `patches/` only suppresses the context for WebGPU/Cross, restoring it
for OpenGL. `scripts/build.sh` applies it to the submodule (idempotently).
