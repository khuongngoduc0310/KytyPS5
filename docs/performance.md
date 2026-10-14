# Gameplay performance telemetry

KytyPS5 includes opt-in release-build telemetry for repeatable gameplay measurements. Telemetry is disabled unless `--profile` or `--performance-report` is supplied. Disabled telemetry creates no Vulkan query pool, records no samples, and writes no report.

## Capture workflow

Use a stable scene, fixed emulator settings, and same driver/power configuration for every comparison. Create report parent directory before launch.

```powershell
New-Item -ItemType Directory -Force captures | Out-Null
.\kyty_emulator.exe --game "D:\Games\ExampleGame" `
  --performance-report captures\baseline.json `
  --performance-warmup-frames 120 `
  --performance-capture-frames 600
```

```bash
mkdir -p captures
./kyty_emulator --game /games/ExampleGame \
  --performance-report captures/baseline.json \
  --performance-warmup-frames 120 \
  --performance-capture-frames 600
```

Options:

- `--performance-report <path>` enables telemetry and writes schema-versioned JSON.
- `--profile` enables telemetry projection into Tracy. Report output remains optional.
- `--performance-warmup-frames <count>` excludes first `count` successful guest frames.
- `--performance-capture-frames <count>` completes after exactly `count` captured guest frames. Zero captures until shutdown.

Frame limits require `--profile` or `--performance-report`. Values must be unsigned decimal integers. Capture completes automatically when bounded frame count is reached; later activity does not alter completed results.

Run baseline and candidate builds at least three times. Compare medians first, then p95/p99, wait counts, compilation misses, transfer volume, and sample-loss fields. Do not compare GPU durations across different GPUs or timestamp periods without accounting for hardware differences.

## Shader and pipeline caches

Release builds with known clean Git identity persist emulator-side SPIR-V modules under `_ShaderCache/<title-id>.bin`. Cache entries are scoped to compiler revision, Vulkan device identity, shader permutation state, and resource specialization. Missing, stale, corrupt, or failed cache data falls back to normal compilation. Writes use temporary-file replacement.

Vulkan driver pipeline binaries remain separate under `_PipelineCache/<title-id>.bin`. Clear both directories for cold-start measurements. `compile.shader_cache_hits` and `compile.shader_cache_misses` show emulator-side cache behavior in telemetry reports.

## Frame semantics

Successful guest presentation defines gameplay frame and advances warm-up/capture windows. Other presentation outcomes remain separate:

- `guest`: new guest image presented successfully
- `repeat`: previous guest image repeated
- `blank`: blank frame presented
- `paused`: presentation suppressed while paused
- `overlay_only`: host overlay without guest image
- `failed`: acquire, submit, or present failed

Only guest frames contribute `frames.guest` and `frames.guest_interval`.

## Report format

Top-level fields:

- `schema_version`: compatibility version; consumers must reject unsupported versions.
- `completion_reason`: `complete`, `shutdown_partial`, or `quick_exit_partial`.
- `capture`: requested window, captured guest-frame count, bounded state, and Tracy state.
- `metadata`: build revision/label, OS, CPU, title ID, GPU device, and GPU driver.
- `frame_classifications`: presentation totals by class.
- `metrics`: fixed metric catalog.
- `sample_loss`: unsupported GPU timing, query exhaustion, unavailable results, and pending samples.

Metric kinds:

- `counter`: cumulative event or byte total.
- `gauge`: latest value; matching `*_high_water` metrics retain maximum value.
- `histogram`: `count`, `total`, `minimum`, `maximum`, `mean`, `p50`, `p95`, and `p99`.

Durations use nanoseconds. Percentiles are approximate upper bounds from bounded base-2 histogram buckets, not exact samples. Zero-count histograms contain zero summaries.

Metric namespaces identify measured subsystem: `frames`, `presents`, `vblank`, `guest_gpu`, `scheduler`, `render`, `barriers`, `compile`, `program`, `pipeline`, `descriptors`, `transfer`, `stream`, `gc`, `wait`, `gpu`, and `gpu_timing`.

## Reproducible scenarios

Record title/build, route or save point, emulator settings, resolution, vblank mode, cache state, warm-up/capture counts, OS power mode, CPU, GPU, and driver with each comparison. Keep camera path and input timing fixed where possible.

| Scenario | Procedure | Primary signals |
|---|---|---|
| Idle | Leave stable menu or motionless scene untouched | frame interval, presentation, queue depth |
| Cold start | Clear only documented emulator caches; launch to fixed first frame | shader/pipeline compilation, uploads |
| Warm gameplay | Re-run fixed route after caches and scene stabilize | frame median/tails, GPU submissions, waits |
| Draw heavy | Use fixed view with highest stable object count | draws, graphics pipeline misses, descriptors, barriers |
| Compute heavy | Use fixed effect/workload dominated by compute | dispatches, compute pipeline misses, GPU duration |
| Upload heavy | Traverse fixed route that streams known scene assets | upload bytes/ranges, stream wraps, fallback count |
| Readback heavy | Exercise fixed capture/query or GPU-dirty synchronization path | download bytes, readback waits, dirty GC bytes |
| Presentation focused | Hold renderer workload stable; compare vblank/presentation settings | presentation CPU/GPU, repeats, late opportunities |

For overhead validation, alternate five no-telemetry runs with five report-only runs instead of batching one mode first. Use warm draw-heavy route and identical 120-frame warm-up plus 600-frame capture. Compare median and p95 `frames.guest_interval`; target no more than 3% median and 5% p95 regression. Confirm wait counts do not gain telemetry-specific waits and save hardware/driver plus all ten report/result references.

## Validation status

Automated validation from the Windows Clang/Ninja build:

- Focused telemetry, CLI, scheduler, transfer, tiler, and timestamp tests pass.
- Vulkan validation-layer run passes `command_scheduler_timeline`, `gpu_timestamp_collector`, `gpu_tiler`, `image_transfer_layout`, and `buffer_cache_dirty_gc`.
- Full CTest: 58/59 pass. `kernel_file_system` remains a pre-existing failure with exit code `0xc0000409` at `Net unknown socket options return protocol-option errors`.
- Disabled/help smoke creates no performance report.

Real-title Tracy capture, report-only capture, and five-run overhead acceptance require game content and fixed gameplay route. Do not mark Phase 1 overhead acceptance complete until those captures exist.

## GPU timing and loss

Guest and presentation submissions use separate Vulkan timestamp streams. Query storage is fixed and bounded. Results are read only after timeline completion and always with availability checks; telemetry never uses `VK_QUERY_RESULT_WAIT_BIT` or adds a GPU wait.

GPU duration absence is not equivalent to zero work. Check `sample_loss` before interpreting `gpu.guest_submission` or `gpu.presentation_submission`:

- `gpu_timing_unsupported`: queue/device lacks usable timestamps or query-pool creation failed.
- `gpu_query_exhaustion`: bounded query slots were occupied; sample dropped.
- `gpu_unavailable_results`: completed timeline work had unavailable/invalid timestamps, or shutdown left a sample pending.

## Publication and privacy

Report publication writes a sibling `.tmp` file, flushes it, then replaces destination. Write failures log once and do not terminate emulation. Normal shutdown completes an unlimited capture. Stopping before a bounded target reports `shutdown_partial`; quick exit reports `quick_exit_partial`. Completed bounded captures retain `complete`.

Reports contain aggregate performance data and listed host/build/title metadata. They do not contain guest memory, shader source, command streams, screenshots, input events, file paths, account data, or per-frame raw samples. Review title and hardware metadata before sharing reports publicly.
