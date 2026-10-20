---
title: Astro Low-FPS Attribution and Fixes - Plan
type: performance
status: proposed
date: 2026-10-20
artifact_contract: ce-unified-plan/v1
product_contract_source: measured-probe-baseline
execution: code
---

# Astro Low-FPS Attribution and Fixes - Plan

## Goal Capsule

- **Objective:** Raise ASTRO BOT (PPSA21564) gameplay frame rate from the measured 10-19 fps toward the 60 fps vblank ceiling, and remove the 2-3 fps stalls, with every improvement attributable to a measured counter.
- **Means:** Replace the synthetic occlusion-counter result with real host occlusion queries, widen GPU fault resolution from one byte to the containing cached range, coalesce redundant `vkQueueSubmit` calls, and stop compiling new pipelines synchronously on the render thread.
- **Authority:** The `KYTY_FPS_PROBE` measurements in this document, current source behavior, Vulkan externally-synchronized-object rules, and the existing guest-GPU/cache ownership model.
- **Scope:** The four measured cost centres plus the measurement harness. No renderer rewrite, no new queue/thread model for command buffers, no resolution-scaling feature, no host-GPU-specific tuning.
- **Stop condition:** Stop and re-measure if a change does not move its target counter, or if it changes rendered output (geometry disappearing, wrong pass ordering) rather than only reducing cost.

## Evidence and Current Behavior

Measurements from `KYTY_FPS_PROBE=1` (commit `b3fab3a9`), Windows, Astro boot and early scene, one-second windows.

| scene | fps | frame avg | draws/s | disp/s | submits/s |
|---|---|---|---|---|---|
| menu / light | 55-60 | 17 ms | ~700 | ~4000 | ~19 000 |
| heavy gameplay | 10-19 | 53-100 ms | ~4300 | ~1200 | ~5500 |

Phase cost, milliseconds per second of wall clock. `process` is the whole guest submission processing and contains the others:

| phase | ms/s | source |
|---|---|---|
| process | ~1010-1100 | `GuestGpu::Process` |
| ├ gpu-wait | 500-920 | `MasterSemaphore::Wait` |
| ├ fault | 230-586 | `KytyExceptionHandler` → `HandleGpuFault` |
| ├ submit | 50-240 | `CommandScheduler::Submit` |
| └ bind | 20-35 | `RenderExecutor::PrepareBindings` |
| shader / pipeline | 0 steady, bursts | `pipelineCache.cpp` |

Derived facts:

- The GPU thread is saturated: `process` ≈ 100 % of one core of wall time, of which `gpu-wait` is roughly half to ninety percent. The thread is blocked on the host GPU, not computing.
- `late-vblank 0` and `drop 0` in every window: presentation keeps up; the guest is the slow producer.
- `emu-insn 0` and `unresolved 0` in every window: no guest instruction emulation and no unresolved imports. Host CPU vendor is not implicated.
- Fault rate ≈ draw rate (4300 draws/s → 4300 faults/s), i.e. roughly one page fault per draw.
- Submits ≈ guest submissions: ~320 `vkQueueSubmit` per frame in the light scene, ~305 per frame in the heavy scene. Draws already batch into one command buffer, so the submits come from flush points, not draws.
- Log emits once: `Warning: game uses occlusion queries, which are currently treated as always visible; GPU usage may be higher and FPS may be lower.`

Code anchors:

- `src/graphics/guest_gpu/graphicsRun.cpp:1329-1352` — `EVENT_WRITE` type `0x39` (occlusion counter dump). Publishes `ready_bit | m_synthetic_occlusion_counter` into 16 interleaved `(begin, end)` u64 slots at `event_address` and increments a counter, so every reported count is non-zero and monotonic.
- `src/graphics/host_gpu/renderer/renderContext.cpp:57-71` — `HandleFault` treats the fault as **1 byte** (`constexpr uint64_t fault_size = 1`), then calls `InvalidateMemory(vaddr, 1)` on both caches or `ReadMemory(vaddr, 1)`.
- `src/graphics/host_gpu/regionDefinitions.h:12-16` — tracking granularity is `TRACKER_PAGE_SIZE = 4 KiB`, `TRACKER_REGION_SIZE = 4 MiB`.
- `src/graphics/host_gpu/memoryTracker.h:38-64` — `InvalidateRegion` takes a per-region lock, tests `IsModified<Gpu>`, changes CPU state, and queues a BDA sync, per region touched.
- `src/graphics/host_gpu/renderer/cache/bufferCache.cpp` — `m_buffers` is an ordered, non-overlapping map; `IsRegionRegistered` already resolves the single cached buffer that intersects a query range but returns only a bool.
- `src/graphics/guest_gpu/graphicsRun.cpp:254-256` — `BufferFlush()` → `GetScheduler().Flush()` → `Flush(SubmitInfo&)` → `Submit`. Flush call sites: `609`, `630`, `643`, `1052`, `1369`, `1389`, `1418`, `1435`.
- `src/graphics/host_gpu/renderer/pipeline/shaders.cpp:244,595` — `CreatePipelineInternal` (graphics and compute) calls `createGraphicsPipelines`/`createComputePipelines` synchronously, driven from `pipelineCache.cpp:1020-1030` and `1055-1065` on first use.

## Product Contract

### Requirements

- **R1. Measurable attribution:** Every change in this plan is accepted or rejected on its target counter from the probe, not on subjective feel. Baseline numbers above are the reference.
- **R2. Real occlusion results:** Occlusion query results returned to the guest reflect actual host depth-test sample counts for the draws that belong to that query, replacing the synthetic always-visible counter.
- **R3. Culling correctness:** With real occlusion results, no geometry that the guest would consider visible may be dropped. A regression toggle restores the synthetic path for A/B comparison.
- **R4. Bounded fault work:** A single GPU page fault resolves the whole containing cached range rather than one byte, so fault count drops roughly to one per distinct buffer per ownership transition instead of one per page touched.
- **R5. Fault safety:** Widening fault resolution must not allocate, block on the GPU, or issue Vulkan calls, and must remain correct when the faulting address belongs to no cached range.
- **R6. Reduced submission count:** Adjacent guest submissions that have no mandatory boundary between them share one `vkQueueSubmit`, reducing submits per frame without changing command ordering or guest-visible completion points.
- **R7. Mandatory boundaries preserved:** Every flush that currently guarantees guest-visible completion (flip preparation, explicit GPU synchronize, suspend point, buffer wait) still submits and still completes before the guest proceeds.
- **R8. No render-thread pipeline stalls:** A first-use pipeline no longer blocks the render thread for the duration of `createGraphicsPipelines`/`createComputePipelines`; frames lost to new materials are bounded and measurable.
- **R9. Device-loss compatibility:** All of the above honour the existing `vulkan_fault` latch and the `bool` `MasterSemaphore::Refresh`/`Wait` contract. No new path issues Vulkan work after device loss.
- **R10. Probe retained behind an opt-in flag:** The measurement harness stays available and costs nothing when disabled.

### Acceptance Examples

- **AE1. Occlusion effect:** Given a scene with heavy overdraw, when real occlusion queries are active, then `Draws` per second at equal fps falls materially versus the synthetic baseline, and rendered output matches a recording captured with the synthetic path.
- **AE2. Occlusion escape hatch:** Given the regression toggle set to synthetic, when the game runs, then behaviour and counters match the pre-change baseline.
- **AE3. Fault reduction:** Given the heavy gameplay scene, when range-scoped faults are active, then `fault` ms/s and `GpuFaults`/s both drop, and visual output is unchanged.
- **AE4. Unregistered fault:** Given a guest write to a GPU-tracked page that belongs to no cached buffer, then the fault handler still completes without allocating or exiting, falling back to the previous page-scope behaviour.
- **AE5. Submit reduction:** Given the light scene, when coalescing is active, then `submits/s` drops substantially at unchanged fps and unchanged visual output.
- **AE6. Mandatory flush:** Given a flip preparation or explicit GPU synchronize, then an intervening submission is still submitted and waited for before the guest continues.
- **AE7. Stall bound:** Given a new material during gameplay, then no frame exceeds the configured pipeline-stall budget, and the shader/pipeline counters still identify the cost.
- **AE8. Device loss:** Given a latched device loss, then every new path returns failure without issuing a submit, query, or wait.

## Settled Technical Decisions

- **KTD1. Occlusion first.** It is the largest measured lever (the GPU is the wall; gpu-wait is the dominant term) and the only change that reduces the total work both guest and host must do.
- **KTD2. Query scope follows the guest's own markers, not heuristics.** The plan does not invent which draws belong to a query; Phase 0 must locate the guest begin/end markers or establish that `0x39` is the only observable boundary. Do not guess a query window from draw counts.
- **KTD3. Fallback must stay cheap and correct.** If the guest marker cannot be resolved for a given draw set, report the synthetic visible value for that set and log once, rather than under-reporting and culling visible geometry.
- **KTD4. Fault scope is the containing cache range, not a fixed larger unit.** Use the existing ordered, non-overlapping buffer map to find the containing range. Do not widen to `TRACKER_REGION_SIZE` (4 MiB): that trades a CPU problem for a bandwidth problem and increases false sharing.
- **KTD5. Fault handler stays allocation-free and GPU-call-free.** Range lookup and state change only. Any upload or flush stays queued for the GPU thread through the existing `QueueBdaSync`/`on_flush` mechanism.
- **KTD6. Coalescing is opt-in per call site.** `Flush` gains an explicit mandatory reason; only sites that have no guest-visible boundary may defer. Do not coalesce by default and then special-case the boundaries.
- **KTD7. Pipeline work moves off the render thread without a new Vulkan threading model.** `vkCreateGraphicsPipelines` is externally synchronized on the pipeline cache, so a worker may call it while the render thread does not touch that cache; the render thread must not block on the result.
- **KTD8. Placeholder first, swap later.** A missing pipeline renders nothing for at most the configured stall budget, then the real pipeline is swapped in. Do not draw with a wrong pipeline.
- **KTD9. Keep the probe.** It is the only reason this plan has numbers; it stays opt-in and is extended with the new counters rather than left ad hoc.

## Implementation Plan

### Phase 0 - Baseline, harness, and occlusion marker spike

1. Re-capture the baseline on the current merge head with `KYTY_FPS_PROBE=1` in three states: boot, menu, heavy gameplay. Record raw log lines as the acceptance reference.
2. Reduce the probe's ad-hoc scaffolding into the permanent opt-in harness: keep `SetEnabled` gating, keep idle ticks from the present thread, and move the temporary `Probe*Ns` counters into named counters (`SubmitNs`, `FaultNs`, `ProcessNs`, `BindNs`).
3. **Spike: locate the guest occlusion-query boundaries.** Establish, from the PM4 stream, whether the guest emits explicit begin/end ZPASS markers before the `0x39` dump, or whether `0x39` is the only observable boundary and the query spans from the previous dump. Record the finding; R2/KTD2 depend on it.
4. Establish how many DBs the guest actually uses in a frame and whether the interleaved `(begin, end)` layout is per-DB across the whole frame or per-pass.
5. Confirm the current synthetic path's effect: capture one recording/screenshot set with the synthetic result as the visual reference for AE1.

**Files:** `src/graphics/presentation/performanceOverlay.{h,cpp}`, `src/common/debugCounters.h`, `src/emulator.cpp`, `src/graphics/presentation/videoOut.cpp`; notes for the spike outside source if needed.

**Tests:** probe disabled costs nothing (`Enabled()` false path); harness prints boot, menu and gameplay windows.

### Phase 1 - Real host occlusion queries

1. Add a query pool owner beside `GraphicContext` or the guest GPU: one `vk::QueryPool` of `VK_QUERY_TYPE_OCCLUSION`, sized to the DB count found in Phase 0, with reset and result-read helpers.
2. Wire the guest's begin/end markers (or the `0x39`-derived window from Phase 0) to `vkCmdBeginQuery`/`vkCmdEndQuery` on the recording command buffer.
3. On `0x39`, replace the synthetic write with `getQueryPoolResults` with `eWait`, honouring that the read must not block the GPU thread indefinitely: if results are unavailable because the responsible submission is not yet retired, either defer the dump to a priority operation after that tick or publish the previous window's counts.
4. Preserve the guest-visible layout exactly: `ready_bit` in bit 63, count in the low bits, 16 interleaved `(begin, end)` slots.
5. Add `Config::OcclusionQueries` (default on) so the synthetic path can be restored for AE2; log the chosen mode once.
6. If a window cannot be resolved, publish the synthetic visible value for that window and count it, rather than under-reporting (KTD3).

**Files:** `src/graphics/guest_gpu/graphicsRun.cpp`, `src/graphics/guest_gpu/command_processor/pm4Dispatch.cpp`, `src/graphics/host_gpu/graphicContext.{h,cpp}`, `src/graphics/host_gpu/renderer/renderContext.{h,cpp}`, `src/common/emulatorConfig.{h,cpp}`, `src/main.cpp`, `src/launcher/*`.

**Tests:** query pool create/reset/read seams with a fake device; layout test asserting the exact 16-slot interleaved write and `ready_bit`; fallback test for an unresolved window; config toggle test.

### Phase 2 - Range-scoped GPU fault resolution

1. Add a range lookup to `BufferCache` (and the texture-cache equivalent) that returns `{address, size}` for the cached buffer containing a guest address, reusing the ordered-map `lower_bound` logic already in `IsRegionRegistered`.
2. Change `RenderContext::HandleFault` to resolve the containing range once, then invalidate/read that whole range instead of one byte:
   - write fault → `InvalidateMemory(range)` on both caches;
   - read fault → `ReadMemory(range)`;
   - no containing range → keep the current 1-byte page-scope behaviour (AE4).
3. Keep the handler free of allocation and GPU calls; widen only the `InvalidateRegion`/`ReadMemory` arguments so uploads stay on the existing queued path.
4. Bound the work: if the containing range exceeds a configured maximum, fall back to page scope and count it, so a pathological multi-megabyte buffer cannot turn one fault into a full re-upload.
5. Verify the texture cache path does not double-resolve: decide explicitly whether a fault resolves its buffer range, its texture range, or both, and record the decision.

**Files:** `src/graphics/host_gpu/renderer/renderContext.{h,cpp}`, `src/graphics/host_gpu/renderer/cache/bufferCache.{h,cpp}`, `src/graphics/host_gpu/renderer/cache/textureCache.{h,cpp}`, `src/graphics/host_gpu/memoryTracker.h`.

**Tests:** containing-range lookup (inside, boundary, gap, unregistered); fault resolves whole range; oversized range falls back; counters show fault reduction; existing memory-tracker tests still pass.

### Phase 3 - Submit coalescing

1. Add a reason to the flush path, e.g. `Flush(FlushReason)` where `Mandatory` covers flip preparation, explicit synchronize, suspend point, and buffer wait, and `Opportunistic` covers the remaining in-stream flushes.
2. Classify every call site: `graphicsRun.cpp:609`, `630`, `643`, `1052`, `1369`, `1389`, `1418`, `1435`, and `BufferFlush`.
3. Defer `Opportunistic` flushes while the command buffer is open and no mandatory boundary has been crossed; submit once when a mandatory boundary, a flip, or idle arrives.
4. Keep the existing deferred-operation and priority-operation contracts intact: a deferred callback still runs only after its tick retires, and `Finish()`/`SynchronizeGpu()` still drain.
5. Ensure `SubmitInfo` accumulation (wait/signal semaphores and ticks) is merged correctly when submissions coalesce; do not drop a semaphore wait.

**Files:** `src/graphics/guest_gpu/graphicsRun.cpp`, `src/graphics/host_gpu/renderer/commandScheduler.{h,cpp}`.

**Tests:** coalescing defers an opportunistic flush; a mandatory flush still submits and waits; semaphore wait/signal sets survive a merge; suspend point still flushes and resets; device-loss path unchanged.

### Phase 4 - Move pipeline creation off the render thread

1. Measure the current stall distribution first: per-pipeline `PipelineCreateNs` peaks and frame times of the frames that contain a first use, so AE7 has a budget.
2. Introduce a pipeline worker: first use enqueues a compile request keyed by the existing cache key and returns "not ready"; the render thread records nothing (or records the draw as skipped) for that pipeline.
3. Serialize all `createGraphicsPipelines`/`createComputePipelines` and driver-cache access on one mutex owned by the pipeline cache, matching Vulkan's external-synchronization requirement, and keep the render thread from touching that cache during a compile.
4. Swap the real pipeline in when the compile completes and force the permutation to re-evaluate so the draw is recorded on the next frame.
5. Keep the driver-cache checkpoint behaviour: compilation on the worker marks the cache dirty; the existing 5-minute checkpoint cadence is respected.
6. Bound the stall: a pipeline may be missing for at most the configured budget; after that, block once and log, so a broken compile cannot loop forever.

**Files:** `src/graphics/host_gpu/renderer/pipeline/pipelineCache.{h,cpp}`, `src/graphics/host_gpu/renderer/pipeline/shaders.cpp`, `src/graphics/host_gpu/renderer/renderDraw.cpp`, `src/graphics/host_gpu/renderer/renderCompute.cpp`, `src/common/emulatorConfig.{h,cpp}`.

**Tests:** worker compile then swap; render thread never blocks past the budget; driver cache still checkpointed; device loss during compile aborts cleanly; permutation re-evaluates after swap.

### Phase 5 - Validation and regression

1. Re-run the Phase 0 capture matrix on the same scenes and compare each counter against baseline. Report per-phase deltas, not a single fps number.
2. Visual regression: capture the same frames with `OcclusionQueries` on and off and with coalescing on and off; geometry must match.
3. Run the existing test suite and the shader/resource-materialization tests, which the fault and pipeline changes touch most.
4. Re-check the 2-3 fps dips specifically: measure max frame time before and after Phase 4.
5. Record the final numbers in this document and in `docs/performance.md`.

**Files:** `docs/performance.md`, this plan.

**Tests:** full `ctest` set; bounded gameplay runs with the probe; no device loss during validation.

## Test Strategy

### Unit and seam tests

- Query pool lifecycle: create, reset, begin/end pairing, result read, unavailable-result path.
- Synthetic layout: exact 16-slot interleaved write with `ready_bit`.
- Containing-range lookup: inside range, exact boundary, gap between ranges, unregistered address.
- Fault scope selection: range resolution, oversized fallback, no-range fallback.
- Flush classification: mandatory vs opportunistic, semaphore merge, suspend point.
- Pipeline worker: request/compile/swap ordering, budget expiry, device loss during compile.

### Existing regressions to retain

- `scalar_provenance_tests`, `resource_materialization_tests`, `shader_cfg_tests` — fault and pipeline changes cross these.
- `vulkan_fault_tests` — every new path must stay loss-aware.
- Memory-tracker and page-manager tests.

### Build and runtime gates

- Release build clean at each phase boundary.
- Probe comparison captured before merging each phase.
- No visual change with the feature toggles disabled.

## Risks and Mitigations

- **Occlusion queries cull visible geometry.** Highest-severity risk, because it produces silently wrong output rather than a crash. Mitigation: keep the synthetic path behind a config toggle, capture a visual reference first, and make the fallback publish *visible* rather than zero.
- **Query results not ready when the guest dumps.** The dump may arrive before the responsible submission retires. Mitigation: defer the dump through the existing priority-operation mechanism keyed on the submission tick, or publish the previous window.
- **Range-scoped faults re-upload too much.** A large buffer turns one fault into one large upload. Mitigation: configured maximum range with page-scope fallback, and watch `BufferUploadBytes`.
- **False sharing increases bandwidth.** CPU writes one field, whole buffer re-uploads. Mitigation: accept for now because CPU is the bottleneck; measure `BufferUploadBytes` and revisit if it inverts.
- **Coalescing delays a guest-visible completion.** Mitigation: mandatory-reason classification done explicitly per call site before enabling, and the suspend/flip paths tested.
- **Pipeline worker races the driver cache.** Mitigation: single owning mutex, matching Vulkan external synchronization, no render-thread cache access during compile.
- **Measurement noise on a saturated machine.** The GPU thread is at 100 %, so other host load skews results. Mitigation: same machine, same scenes, compare counters rather than absolute fps alone.

## Out of Scope

- Resolution scaling or upscaling features.
- Reworking the command-buffer/queue architecture or adding queue threads beyond the pipeline worker.
- Host GPU vendor-specific tuning or driver workarounds.
- Rewriting shader translation, SRT walking, or the device-loss design.
- Emulated-instruction and unresolved-import paths: measured at zero on this title and CPU.
- Arbitrary fps limits, frame generation, or presenting faster than the guest produces.

## Completion Checklist

- [ ] Probe harness permanent, opt-in, and extended with named phase counters.
- [ ] Occlusion begin/end semantics documented from the PM4 stream.
- [ ] Real occlusion queries implemented with a tested synthetic fallback toggle.
- [ ] Fault resolution is range-scoped with page-scope fallback for absent/oversized ranges.
- [ ] Flush reasons classified per call site; opportunistic flushes coalesce; mandatory ones do not.
- [ ] Pipeline creation no longer blocks the render thread past the configured budget.
- [ ] Per-phase counter deltas recorded against the baseline table in this document.
- [ ] Visual output unchanged with toggles disabled.
- [ ] Device-loss paths still return failure without issuing Vulkan work.
- [ ] `docs/performance.md` updated with final numbers and the probe instructions.

## Handoff to Implementation

Start with Phase 0. The occlusion spike decides the shape of Phase 1 and must not be skipped: without the guest's begin/end semantics, any query window is a guess and R3 is unenforceable. Phases 2 and 3 are independent of Phase 1 and of each other, so they may be developed in parallel, but they must be measured one at a time so each delta is attributable. Phase 4 is last because it changes frame scheduling and would confound the other measurements.
