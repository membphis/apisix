# Step-1 validation evidence (2026-11-09)

| File | Content |
|---|---|
| `parity.log`, `parity.json` | Same-batch functional parity before the benchmark: stable Lua, C step 1 (native-Lua), Rust step 1, C v2, Rust v2. The unchanged v2 suite (47 cases, 128 concurrent authentications, keepalive, rejected requests never proxied, retry-off), the response headers owned by the native header filter, and the generation lifecycle for Rust step 1, C v2 and Rust v2 |
| `old-rust-v2-headers.json` | Response headers of the Rust v2 module measured in `bench-20261109T010224Z` (rebuilt from `84056c5`, same SHA-256 `d36742784549d79a...`) next to C v2: `Server: openresty` instead of `APISIX/3.19.0`, no `X-APISIX-Upstream-Status` on a 502 |
| `rust-build.json`, `c-build.json` | Build metadata of the measured modules (Rust release `770b3e8b8bd023f7...`, C O3) |
| `panic-v2.log` | v2 pipeline, `panic-test` build: 20 panics in the access handler, each answered with 500, worker PID unchanged |
| `panic-step1.log`, `panic-step1-error-excerpt.log` | Step 1, `panic-test` build: a panic in `engine.start_phase` closes the connection and aborts the worker (`Rust panics must be rethrown`, SIGABRT); the master respawns it (3 times) |
| `final-core-tests.log` | `apisix-rs-core` unit tests (20) on the final tree |
| `final-rust-build.json` | Final release rebuild: SHA-256 identical to the measured module |
| `final-parity.log`, `final-parity.json` | The five-path parity rerun on the final tree after the benchmark |
| `code-stats.json` | `tools/code_stats.py` on the final tree |
