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

### Phase 0 spike result - occlusion marker semantics (resolved)

Measured with `KYTY_PM4_PROBE=1`, which logs every `EVENT_WRITE` type plus the draw-count delta and the event sequence between consecutive `ZPASS_DONE` (`0x39`) events.

- **There are no dedicated occlusion begin/end event types.** The guest uses `ZPASS_DONE` only. All other types on this title are cache flush / partial flush / writeback (`0x10`, `0x16`, `0x31`, `0x2a`, `0x2c`, `0x2e`, `0x07`, `0x0f`) plus the ignored writeback set.
- **A query is a pair of consecutive `ZPASS_DONE` writes 8 bytes apart:** `(A, A+8)`, where `event_index` is always `0x1`.
- **Exactly one draw happens between the pair members.** Observed deltas across 48 events: pair-first deltas `11` (×20) and `8` (×3), pair-second delta `1` (×24). So the shape is *begin snapshot → 1 culling draw → end snapshot*, and the culling draw is what the count describes.
- **Queries rotate through fixed slots.** Three slots per pass, `0x1C0` apart (`...6f80`, `...6dc0`, `...6c00`, and a second pass block at `...6640`, `...6800`, `...69c0`), reused across frames. At least two passes are active, so ≥6 concurrent query objects.
- **The current synthetic write does not match this layout.** It writes 16 slots at 16-byte stride starting at `event_address`, whereas the guest uses one u64 per event at that event's own address with an 8-byte begin/end offset. Any real implementation must write a single u64 per `ZPASS_DONE`, not a 16-slot fan-out.
- Open question carried into Phase 1: which member of the pair the guest polls for `ready_bit`. The end slot is the natural answer, but it must be verified before writing the result (see Phase 1 step 6).

### Fix B result - reverted: it device-lost deterministically

Fix B (retain the first permutation of a "static" plan and reuse it) was implemented, measured, and then **reverted**. It is unsound.

Evidence, all runs same build, same command:

| build | last shader counts reached | max draws/s | device lost |
|---|---|---|---|
| pre-B (attribution run) | VS57 / PS64 / CS67 / GS5 | 5314 | 0 |
| B on | VS22 / PS36 / CS48 / GS4 | 1099-1230 | 2, every one of 4 runs |
| B off (kill switch) | VS96 / PS94 / CS86 / GS7 | 17871 | 0 |

With B enabled the emulator stalls at the same boot point every time - one ~1.2 s frame with a shader/pipeline compile burst, then `Vulkan device lost: operation=semaphore-wait` - and never progresses past `CS48`. With B disabled it sails past and reaches scenes 3.4x heavier than the pre-B build ever did.

Why it fails is not yet established. Two candidate explanations: the fast path skipped `MaterializeResources`' per-call reset of `resource_plan.selector_keys[].resolved`, or a descriptor genuinely still varied for a plan the predicate admitted. Either way the predicate was proven insufficient by execution, so the code is removed rather than left behind a flag.

### Step 1 (range bookkeeping) - no measurable effect; Step 2 accounting closed

Step 1 collapsed consecutive duplicate ranges and reserved the range vector once per materialisation. Measured before/after at equal draw rates:

| | `srt` ms/s | reads/s |
|---|---|---|
| before | 259-324 | 1.11-1.35 M |
| after | 262-322 | 1.11-1.26 M |

No change. The walk almost never re-reads the same word consecutively, and the vector's capacity was already amortised by the first frames. Kept because it is neutral and shrinks the recorded-range set that later overlap analysis consumes, but it is not a win and should not be counted as one.

Step 2 split `prog` into `PrepareProgram` and the cache-lookup wrapper. The accounting now closes:

`prog` 354-538 ms/s at 15-16.8 k draws/s = `PrepareProgram` 30-35 + cache lookup 322-354, and the cache lookup is `srt` walk 262-322 + descriptors 34-40 + build 2.

So at high draw rates the split is: **SRT walk 74-83%, descriptor materialisation ~10%, `PrepareProgram` ~9%, `BuildResourceSpecialization` ~0.5%**.

Budget for the same windows (`process` ~1000 ms/s, one saturated thread):

`gpu-wait` 719-858, `prog` 354-538 (containing the walk, which itself provokes most of `fault` 183-392 because the walk reads GPU-tracked guest memory), `submit` 72-109.

Conclusion: the emulator is now GPU-bound (`gpu-wait` dominates), and the remaining CPU cost is the SRT walk's own evaluation work per read (~0.25 us), not allocation. Cutting it any further means either reworking upstream's walker or the caching route that already failed twice, so CPU-side work should stop here.

### Inside MaterializeResources

Timers around `RefreshFlatBuffer`, the buffer/image/sampler descriptor loops and `BuildResourceSpecialization`. Heavy windows (8-17.8 k draws/s), 300 s run, no device loss, reached VS66/PS71/CS61/GS5:

| section | ms/s | share of `prog` |
|---|---|---|
| `RefreshFlatBuffer` (SRT walk) | 202-323 | 57-65% |
| descriptor materialisation | 21-39 | ~6% |
| `BuildResourceSpecialization` | 1.2-2.3 | ~0.4% |
| **unaccounted inside `prog`** | **~130-420** | **~25-35%** |

`prog` itself is 352-645 ms/s in these windows.

Two conclusions:

1. The walk dominates the materialiser, and it is read-bound: 0.75-1.35 M `srt_reads`/s at 9-17.8 k draws/s, i.e. roughly 40 guest-memory reads per draw-stage at ~0.24 us each. That per-read cost is far above a 4-byte copy, which points at per-read bookkeeping rather than the copy - `SrtReadCapture::ReadOrdinary` appends to `capture.m_ranges` on every read, so a vector grow/allocation happens tens of times per draw-stage.
2. Roughly a third of `prog` is **outside** the materialiser entirely: `prog` wraps `GetGraphicsPrograms`, which also runs `PrepareProgram` twice (global-mutex `ShaderGetMappedData`, `ShaderMapEntry`/`ShaderParams` copies, static vertex-input decode) and the `ProgramCache::Get` wrapper.

Next single measurement, if wanted: split `prog` into `PrepareProgram` versus the cache-lookup remainder. Next cheap optimisation candidate: pre-size or dedupe `SrtReadCapture::m_ranges`.

### Fix A result - reverted as well

Per-permutation memo key (hash of shader_base, indirect args, push-data cursor, workgroup counts, call-source revision and all user-data words), storing each permutation's own `ResourceSnapshot`, with the `selector_keys` reset reproduced on the hit path. Same binary, same command, 300 s each:

| mode | last shaders reached | max draws/s | device lost | windows |
|---|---|---|---|---|
| memo on | VS53 / PS64 / CS65 / GS5 | 9839 | 2 | 41 (stalled) |
| memo off | VS145 / PS125 / CS95 / GS7 | 22931 | 0 | 296 |

So A fails the same way B did, and further than B before it does. Conclusion: caching the materialised specialization is not sound against this codebase as-is. Both attempts skip `MaterializeResources`, and something it produces beyond `resources`/`specialization` is consumed by later stages - the mutable per-plan state (`selector_keys`, `material_keys`, the evaluation scratch) is the likely carrier, since upstream added indexed material keys for pointer-table image selection.

Stop condition honoured: two independent attempts at the same cache both produced deterministic device loss, so no third blind attempt. The per-draw materialisation appears to be inherent to upstream's design; the remaining lever is its internal cost, not skipping it.

### The surviving win - the memory-probe regression fix

The other half of that change was sound and is kept: `SrtReadCapture::ReadOrdinary` called `HostMemoryRangeIsReadable` per SRT read, and that helper does a `VirtualQuery` on Windows and opens `/proc/self/maps` on Linux. Resource materialisation runs per draw and per stage, so the probe sat on a very hot path. It is replaced by a single integer compare against a 4 GiB guest-address floor, which still rejects the non-guest `0x18` case that motivated the guard.

Measured on the heavy scene, safe build:

| build | draws/s | `prog` ms/s | prog per draw | fps |
|---|---|---|---|---|
| pre-fix | 2224-3553 | 652-783 | ~0.26 ms | 11.8-27.5 |
| fixed | 4900-9800 | 186-293 | ~0.037 ms | 19-41 |
| fixed | 14000-15764 | ~400-500 | ~0.03 ms | 22-27 |

Roughly 7x cheaper per draw, and the emulator now reaches 17.9 k draws/s where the pre-fix build peaked at 5.3 k. The 20x `prog` reduction seen in the light scene earlier was therefore mostly this, not B.

Also fixed in the same session: ImGui's `CheckVulkanResult` asserted during teardown after a device loss, converting a diagnosable failure into a fatal one. It now ignores `VK_ERROR_DEVICE_LOST`.

### Root cause of the program-selection cost

`pipelineCache.cpp` `SourceEntry::FindPermutation` calls `MaterializeResources` before comparing any candidate, so every lookup runs the full SRT walk (guest-memory reads included) per draw, per stage:

```cpp
EXIT_IF(!MaterializeResources(resource_plan, runtime, resources, specialization));
for (auto& candidate: permutations) { ... if (candidate.specialization != specialization) continue; ... }
```

At ~2900 draws/s that is ~5800 materialisations/s. No cache can absorb it today because `SourceEntry` holds a single shared `resources`/`specialization` pair for all permutations and the caller reads `&entry->second.resources`, so there is nowhere to retain a per-state result. `PrepareProgram` adds a global-mutex lookup plus a `ShaderMapEntry` copy per stage; `GetGraphicsPipeline`, vertex/index acquisition and target resolution are all negligible by measurement.

Two candidate fixes:

- **A. Per-permutation memo key.** Give each `Permutation` its own `ResourceSnapshot` and a `memo_key` = hash(user_data words, shader_base, push_data_cursor, workgroup/indirect state); keep `unordered_map<uint64_t, Permutation*>` per `SourceEntry`. One walk per distinct state instead of per draw. Real fix, medium risk, grows memory per distinct state.
- **B. Static-plan fast path.** When `!requires_specialization_memory && source_reads.empty() && !capture_specialization_reads` and there is no indirect descriptor, `specialization` cannot depend on `user_data`, so the first permutation can be returned without materialising. Small, low risk, payoff unknown until measured.

Prefer B first: it is a contained predicate plus an early return, and it tells us how many programs are actually state-dependent before committing to A's memory and structural cost.

### Per-draw attribution result - the GPU thread is spent in shader program selection

Timers added around the four unmeasured steps of `RenderExecutor`'s draw path. Gameplay windows (ms/s of wall clock, all nested inside `process`):

| phase | ms/s | share of `process` (~1010) |
|---|---|---|
| `GetGraphicsPrograms` (`RefreshShaders`) | **486-736** | **48-73%** |
| `gpu-wait` | 288-497 | 29-49% |
| `fault` | 132-261 | 13-26% |
| `submit` | 43-98 | 4-10% |
| `bind` (`PrepareBindings`) | 20-24 | ~2% |
| `GetGraphicsPipeline` | ~1.0 | ~0.1% |
| vertex/index acquisition | 2.1-2.6 | ~0.2% |
| render-target acquisition | 2.6-21.5 | ~0.3-2% |

In the light scene the same timer falls to 155-161 ms/s, confirming the cost scales with draws per frame rather than being a one-off.

Conclusion: the dominant CPU cost is shader program selection and its per-draw resource materialization, not pipeline creation, buffer acquisition or target resolution. Pipeline/buffer/target timers are all negligible, so Phase 3 (submit coalescing) and further cache work are not where the remaining time is.

Second finding from the same run: `frange` 174-545 vs `fbyte` 4218-6862 per second, i.e. **roughly 95% of faults still fall back to byte scope** - most faulting addresses have no bounded containing buffer, or the buffer exceeds the 256 KiB cap. Phase 2's measured win therefore came from the small slice that did resolve. Widening to textures or raising the cap is a smaller further win; the real fault cost is that most faults are not buffer-covered at all.

### Phase 2 result - fault cost halved

Implementation: `BufferCache::FindRegisteredRange` resolves the cached buffer containing the fault address (ordered, non-overlapping map), and `RenderContext::HandleFault` now invalidates or reads that whole range instead of one byte, capped at 256 KiB. Ranges without a bounded containing buffer keep the previous byte scope. Textures deliberately stay byte-scoped: their aliases are tracked separately and no benefit was measured there.

Measured on comparable windows (same scene shape: ~3.6-4.1 k draws/s, ~0.9-1.2 k dispatches/s):

| counter | before | after |
|---|---|---|
| `fault` ms/s | 510-610 | **238-282** |
| faults/s | 3100-3560 | 2403-3129 |
| fps | 11.8-15 | 13.8-18 |

Two conclusions:

1. **Roughly half the per-fault cost is gone.** Fault *count* fell only ~20% while total fault time fell ~50%, which fits the mechanism: `BufferCache::ReadMemory` does a synchronous GPU-thread round-trip, so a wider resolution both removes round-trips and unprotects the pages that would have faulted next (`ChangeState<Cpu,true>` clears the watch over the whole range).
2. **A meaningful share of faults still fall back to byte scope**, since resolving every fault against its buffer would have cut the count far more than 20%. `frange`/`fbyte` counters now report that split on the next run; widening to textures or raising the 256 KiB cap depends on that number.

Caveat: the fps move is confounded by scene variance (dispatch rate differed between windows). The trustworthy result is `fault` ms/s, which is a direct measurement of the handler.

### Keep-depth-on-clear experiment - falsified

Preserving `m_depth_render_target` across `ContextStateOperation::Clear` (`KYTY_KEEP_DEPTH_ON_CLEAR=1`) does not fix the cull draws. They still report:

```
cull-draw #0 slot=0 z_enable=1 ... depth_addrs r=0x0 w=0x0 htile=0x0
```

and the occlusion outcome is unchanged or worse: `zero 0`, `samples` 314-661 M/s, draws 14.9-26.5 k/s. The hypothesis that a context clear drops a live depth binding is therefore wrong for this path - the binding is already zero at the cull draw for another reason.

Occlusion remains inert and its mechanism unresolved. The GPU-side work is parked here.

### Depth-base trace, sequenced - the ambiguity is resolved

A monotonic event id on all three traces gives the ordering:

```
[ev 60] zbase z-write-lo base=0x513560000      <- real depth bound
[ev 61] zbase z-read-hi  base=0x513560000
[ev 62] zbase z-write-hi base=0x513560000
[ev 63] ctx push-clear base_before r=0x0      w=0x0
[ev 64] ctx push-clear base_before r=0x0      w=0x0
[ev 65] ctx push-clear base_before r=0x513560000 w=0x513560000 z_enable=1
...
[ev 69] ctx clear      base_before r=0x513560000 w=0x513560000   <- wipes a live binding
[ev 70] ctx clear      base_before r=0x0      w=0x0
[ev 71..79] push-clear/clear, base 0
[ev 80] cull-draw #0 slot=0 z_enable=1 ... depth_addrs r=0x0 w=0x0
[ev 81..]  cull-draw #1..   same
```

So the cull draws inherit a depth binding that was wiped by a `context state op` and not re-emitted, not a binding the guest never made. Push/pop is otherwise faithful - `[ev 69]` shows `0x513560000` alive again after the push-clear at `ev 65`, i.e. save/restore works.

Hypothesis: on hardware the guest's context-state CLEAR does not drop `DB_Z_READ_BASE`/`DB_Z_WRITE_BASE`, because the guest proceeds to draw with `z_enable=1` and does not re-emit them. The emulator's `Clear` does `*this = Context()`, which drops the depth attachment, so the cull pass ends up depth-less and every query reports visible.

This is a hypothesis and not yet a fix. The candidate experiment is to keep `m_depth_render_target` across `Clear` only (not `PushClear`), behind an env switch so the two modes can be compared in one build. It is the same *class* of change as fixes B and A - state semantics rather than measured overhead - so it needs an A/B run and a visual sanity check, not a commit on reasoning alone.

### Depth-base trace - ambiguity remains, no fix attempted

Traced `DB_Z_READ_BASE`/`DB_Z_WRITE_BASE` (lo and hi) writes and context-state operations.

What the trace establishes:

- The guest **does** bind real depth surfaces: `0x513560000` arrives through both the read and write base registers (20 nonzero writes in the 48-event window). The register decoding is therefore correct.
- The guest **also** writes zero to those registers explicitly, in lo/hi quadruplets.
- A context clear can arrive while a live base is bound: `[ctx] clear base_before r=0x513560000 w=0x513560000`.

What it does not establish: the interleaving. The depth-base trace and the cull-draw observations are separate streams with no ordering marker, so two explanations remain open and I cannot separate them from this data:

1. The guest genuinely unbinds depth for the cull pass (writes zero, draws, rebinds). Then the emulator is faithful and the queries are meaningless on hardware too, which would mean the occlusion theory is wrong.
2. The guest relies on the depth binding surviving a context clear, which the emulator cannot reproduce because `Clear`/`PushClear` reset `m_depth_render_target`.

No fix was attempted. Changing `Clear`/`PushClear` to preserve the depth attachment is a speculative semantic change to emulator state handling - the same class of change as fixes B and A, both of which produced deterministic device loss. It needs ordering evidence first.

Cheapest next step: put a monotonic event counter in both traces (and in the cull-draw trace) so the sequence `bind -> clear -> cull draw` is directly visible. One run answers it.

### GPU-side root cause of the inert occlusion queries

Probes: `KYTY_OCCLUSION_PROBE=1` logs the depth state of the draw between each `ZPASS_DONE` pair, and tallies every draw as depth-bound or depth-unbound.

Depth tracking itself is healthy: 98-100% of draws in heavy windows (14-26 k draws/s) resolve with a real depth attachment, and only 0-2.4% are unbound.

The occlusion cull draws are the anomaly. All 64 sampled pair draws report:

```
z_enable=1 z_write=0 zfunc=3 depth_addrs r=0x0 w=0x0 htile=0x0 idx=6 inst=1
```

Depth test enabled, a 6-index quad (2 triangles, full-screen-ish given ~3.7 M samples/query), but **no depth surface bound**. `ResolveRenderDepthTarget` sees `z_read_base_addr == 0 && z_write_base_addr == 0`, classifies the attachment as unbound and returns without a depth attachment, so the depth test does nothing and every rasterised sample passes. That is exactly why every query reports "visible": there is nothing to be occluded by.

So the ordering is: no culling -> the guest submits every object -> ~15-25 k draws/s -> GPU-bound (`gpu-wait` 660-1190 ms/s of a ~1000 ms/s thread budget). The inactive occlusion queries are a *cause* of the GPU load, not a separate issue.

`ApplyContextStateOperation` copies the whole context on push/pop, so push/pop is faithful. `Clear` and `PushClear` wipe it, including `m_depth_render_target`. The leading suspect is a cull pass that runs inside a cleared context and expects the depth base to survive, which the emulator cannot reproduce.

Next measurement, if wanted: count `DB_Z_READ_BASE`/`DB_Z_WRITE_BASE` writes by value and the context-state operations around each pair draw, to prove whether the base is cleared (emulator modelling) or simply never emitted (guest behaviour).

### Phase 1 result - implemented, faithful, currently inert

Implementation: `RenderContext` owns a `VK_QUERY_TYPE_OCCLUSION` pool of 64 queries; slots are keyed by the pair base address; the pair-first event issues `vkCmdBeginQuery`, the pair-second issues `vkCmdEndQuery`; the count is read from a deferred priority operation one cycle later.

Two findings changed the design:

1. **Value shape is a delta, not an absolute.** Publishing one absolute count makes the guest react violently: same build, same session, draws per frame doubled (291 -> 624), dispatches collapsed 830 -> 340/s, fps 12-15 -> 5-6. The guest consumes the pair and computes `end - begin`. The implementation therefore publishes `ready|0` at the begin member and `ready|last count` at the end member.
2. **Publishing only after retirement serialises the guest.** With the value written solely by the deferred operation, the guest round-trips to the GPU for every query and fps fell to 4-6. Writing a stale-but-ready value inline (hardware-like latency) restored parity.

Measured with delta semantics, same build, `--occlusion-queries` on vs off: fps 11.8-14.8 vs 12-15, draws 3617-3969 vs 3580-3990/s, dispatches 830-1033 vs 830-1030/s, submits 4087-5066 vs 4070-5000/s, `fb 0`. **Parity, no regression and no win.**

Count distribution explains the lack of a win: over ~38 queries/s, `zero 0` and `samples` 16-19 M/s, i.e. every query reports hundreds of thousands of samples and none is ever fully occluded. The guest's culling proxy draws pass depth in the emulator, so real occlusion data says "everything visible" - the same answer the synthetic path gave.

Decision: the feature is **opt-in** (`--occlusion-queries`, default off). It is faithful and costs nothing meaningful, but shipping it on by default would change behaviour with no measured benefit and no proof the counts are correct.

Follow-up before this can help: establish why the proxy draws between the pair members pass depth (depth state, Hi-Z, or the wrong draw being bracketed). Until then occlusion cannot cull and the GPU-overdraw theory is unconfirmed; treat the Phase 2/3 CPU costs as the actionable levers.

## Stage-loading crash - root cause and fix

Entering the hub crash-site stage killed the emulator. Guest thread `RoomLoad_ATQT` / `ProductNextLoad_ATQT` dereferenced null (`0x28`, then `0xffffffffffffffff`), after the game logged:

```
Xml file "data/prein/levels/hub_crashsite_island_4/32D593B1-....lvx" is broken and failed to load
```

The file is present and complete (2,227,437 bytes, ends with `</Level>`). The emulator log showed why:

```
Read 0 bytes (asked 2227437, pos 0, size 2227437) from: .../32D593B1-....lvx
```

A full-size read at offset 0 returned zero. `ReadFile` failed with `ERROR_INVALID_USER_BUFFER` (1784), and `SysFileRead` discarded the error, so the file layer reported EOF:

```
buf=0x100e603700 first(state=COMMIT protect=PAGE_READWRITE) last(state=COMMIT protect=PAGE_READONLY)
```

The destination is committed and starts writable but **ends in a `PAGE_READONLY` page** - a GPU page watcher. `ReadFile` probes the whole range before copying and refuses any range it cannot write end to end. Guest writes never hit this because a user-mode write faults, and the access-violation handler unprotects the page; a kernel-mode copy fails instead. This was not specific to the level file: 528 KB and 4.8 KB reads failed the same way, so loads were being silently truncated all along.

Fix: `ReadIntoGuestMemory` stages guest reads through a bounded buffer and `memcpy`s into guest memory, so the write takes the normal fault path and the page is unprotected exactly as for guest code. Applied to `KernelRead` and `KernelPread`. `SysFileRead` now also logs a failed `ReadFile` with its error code and the destination page state instead of reporting EOF.

Verification (300 s run): `ReadFile failures 0`, `Xml broken 0`, the 2.2 MB level read returns its full size, no guest fault, and the run continued to VS118 before the 5-minute pipeline-cache checkpoint.

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
- **KTD2. Query scope is the observed `(A, A+8)` `ZPASS_DONE` pair.** Phase 0 resolved this: no separate begin/end event types exist, and exactly one draw sits between the pair members. Reopen this decision only if a title is found that emits a different shape.
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

1. Add a query pool owner beside `GraphicContext` or the guest GPU: `vk::QueryPool` of `VK_QUERY_TYPE_OCCLUSION`, sized from the observed slot count (Phase 0: 3 slots per pass, ≥2 passes; size for 16 and index by slot address) with `reset`, `begin`, `end` and deferred result-read helpers.
2. Map a slot address to a query index keyed by the pair base (`A`), derived from the observed addresses rather than a fixed stride. Do not assume the 16-interleaved layout the synthetic path used.
3. At the pair-first event (`A`), `vkCmdBeginQuery` on the slot's query. At the pair-second event (`A+8`), `vkCmdEndQuery`. Exactly one draw sits between them (Phase 0), so the query brackets the culling draw.
4. Write a **single u64** at the event address on resolution, not a 16-slot fan-out. Publish `ready_bit` in bit 63 and the real sample count in the low bits.
5. Result availability: results are only valid after the submission containing `vkCmdEndQuery` retires. Use the existing `DeferPriorityOperation` keyed on that submission tick to read `getQueryPoolResults` and write guest memory, or publish the previous frame's count for that slot. Do not block the GPU thread on `eWait` indefinitely.
6. **Verify which slot the guest polls** for `ready_bit` (pair-first vs pair-second) before choosing where to publish. Log the first 16 resolutions with both slot values and the guest's subsequent behaviour; if the poll target cannot be determined, publish the same count to both members of the pair.
7. Add `Config::OcclusionQueries` (default on) so the synthetic path can be restored for AE2; log the chosen mode once.
8. If a pair cannot be resolved (missing half, unknown slot, result unavailable for too long), publish the synthetic visible value for that slot and count it, rather than under-reporting (KTD3).

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
- [x] Occlusion begin/end semantics documented from the PM4 stream (pair `(A, A+8)`, one draw between).
- [~] Real occlusion queries implemented, delta-correct, opt-in; counts are currently all-visible so no culling win yet (see Phase 1 result).
- [x] Fault resolution is range-scoped with byte-scope fallback for absent/oversized ranges (fault ms/s 510-610 -> 238-282).
- [ ] Flush reasons classified per call site; opportunistic flushes coalesce; mandatory ones do not.
- [ ] Pipeline creation no longer blocks the render thread past the configured budget.
- [ ] Per-phase counter deltas recorded against the baseline table in this document.
- [ ] Visual output unchanged with toggles disabled.
- [ ] Device-loss paths still return failure without issuing Vulkan work.
- [ ] `docs/performance.md` updated with final numbers and the probe instructions.

## Handoff to Implementation

Start with Phase 0. The occlusion spike decides the shape of Phase 1 and must not be skipped: without the guest's begin/end semantics, any query window is a guess and R3 is unenforceable. Phases 2 and 3 are independent of Phase 1 and of each other, so they may be developed in parallel, but they must be measured one at a time so each delta is attributable. Phase 4 is last because it changes frame scheduling and would confound the other measurements.
