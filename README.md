# NextTrain — RTD Pebble App

Real-time RTD (Denver) arrivals on your Pebble. Favorites, alerts, and live countdowns for the Emery platform.

## Architecture

- **C firmware** (`src/c/`): Pebble watchapp — windows, state, AppMessage protocol
- **JS phone side** (`src/pkjs/`): Runs on the paired phone; fetches live arrivals from the Cloudflare Worker and pushes data to the watch over Bluetooth
- **Cloudflare Worker** (`nt.sloccy.workers.dev`): Parses live GTFS-RT feed; static schedule built nightly by GitHub Actions and stored in KV
- **AppMessage protocol** (`src/c/protocol.h`): Binary wire format; large payloads (stations, alert detail) are chunked across multiple 1848-byte messages

## Building

```
pebble build
```

Requires the Pebble SDK 4.9 toolchain. The build runs with `-Werror` — all compiler warnings are errors.

## Code quality standards

### JavaScript (`src/pkjs/`)

Linted with ESLint flat config (`eslint.config.mjs`). Run:

```
npx eslint src/pkjs/
```

Target: **0 errors, 0 warnings**.

Key rules enforced:
- `ecmaVersion: 5` — Pebble pkjs runtime is ES5; no arrow functions, template literals, destructuring, etc.
- `sourceType: "commonjs"` — files are webpack-bundled CommonJS modules
- `no-undef` — all globals must be declared (Pebble SDK globals enumerated in config)
- `no-unused-vars` — unused variables are errors; prefix with `_` to intentionally suppress (e.g. `_err`, `_data`)
- `no-redeclare` — no duplicate `var` declarations in the same scope
- `no-empty` — empty catch blocks are errors (use `catch (_e) {}` if the error is intentionally swallowed)
- `eqeqeq` — always use `===`/`!==` (except `== null`)
- `no-implied-eval`, `no-self-assign`, `no-self-compare`, `no-throw-literal`

### C (`src/c/`)

All files are reviewed cover-to-cover on each substantive change. Checklist:

- Buffer bounds: every `strncpy`/`memcpy`/`snprintf` target is null-terminated and size-guarded
- `malloc`/`free` pairing: every heap allocation is freed in the matching error and success paths
- Window lifecycle: every `prv_window_unload` calls `window_destroy(win)` before clearing the static pointer
- Sequence-point safety: multi-byte reads from pointer arithmetic use separate statements (no `*p++ | *p++`)
- No silent data corruption on chunk overflow: log the error and abort the reassembly, don't silently skip
- JSON encoding: stage each entry before appending to avoid trailing commas on truncation
