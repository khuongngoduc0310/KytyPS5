# Phase 2: shader and pipeline hitch reduction

## Goal

Reduce cold-start and first-use gameplay hitches caused by shader translation, SPIR-V emission, shader-module creation, and Vulkan pipeline creation without changing shader semantics or introducing unsynchronized renderer work.

## Constraints

- Preserve Phase 1 telemetry and capture semantics.
- Keep cache use release-safe, versioned, device/compiler scoped, bounded, and failure-tolerant.
- Never trust corrupt, stale, or partially published cache data.
- Keep synchronous fallback behavior when cache data is absent or rejected.
- Do not move Vulkan device work to worker threads until ownership and command submission rules are characterized.
- Measure before/after with same telemetry capture route.

## Work units

### P2.1 Persistent emulator-side SPIR-V cache — implemented

Cache final SPIR-V words keyed by compiler revision, device identity, shader stage, guest shader identity, static stage shape, resource specialization, push-data layout, and linked function code. Rebuild runtime `CompiledShaderInfo` normally, but skip SPIR-V emission on a valid hit. Store bounded binary entries under `_ShaderCache/<title>.bin`; publish through temporary-file replacement. Cache failures remain non-fatal and fall back to compilation.

### P2.2 Pipeline warmup inventory

Use Phase 1 pipeline miss metrics and a bounded capture to identify first-use graphics/compute pipeline keys. Add an optional replayable warmup inventory only after key stability is proven. Warmup must run on renderer ownership thread and must not alter guest-visible ordering.

### P2.3 Async preparation

Deferred. Design worker ownership for translation and pipeline preparation only after P2.1 and P2.2 measurements. No Vulkan shader-module or pipeline creation from arbitrary workers without an explicit device-thread contract.

## Acceptance

- Cache disabled/missing/corrupt: behavior and output match current compilation path.
- Cache hit: SPIR-V compilation duration and count decrease; shader module and pipeline creation remain separately measured.
- Cache signature mismatch: cache ignored and replaced safely.
- Concurrent or interrupted save: old cache remains usable; no partial cache is loaded.
- Focused shader, pipeline, renderer, and telemetry tests pass.
- Real-title report comparison shows reduced first-use compile work without increased frame waits or GPU timing loss.
