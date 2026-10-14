---
title: Vulkan Device-Loss Isolation and Astro Validation - Plan
type: bugfix
status: proposed
date: 2026-10-14
artifact_contract: ce-unified-plan/v1
product_contract_source: incident-evidence
execution: code
---

# Vulkan Device-Loss Isolation and Astro Validation - Plan

## Goal Capsule

- **Objective:** Turn Astro's `VK_ERROR_DEVICE_LOST` into a diagnosable, terminal emulator failure instead of a misleading timeline wait, black screen, or unsafe continuation.
- **Means:** Add shared one-shot device-loss state, preserve first-failure metadata, stop new Vulkan work, make scheduler/presentation waits loss-aware, cancel CPU-side presentation work, and perform loss-safe teardown.
- **Authority:** Current source behavior, Astro logs, Vulkan result semantics, and existing ownership/threading rules.
- **Scope:** Fault isolation and bounded validation only. No in-process Vulkan recovery, async Vulkan redesign, TDR policy changes, or broad renderer rewrite.
- **Stop condition:** Stop implementation if a proposed path requires issuing Vulkan calls after loss, fabricating timeline completion, or continuing guest execution against a lost device.

## Evidence and Current Behavior

- Astro reaches substantial workload before failure: approximately 46 vertex, 61 pixel, and 28 compute shaders in observed run.
- Failure is `vk::Result::eErrorDeviceLost (-4)` while waiting for timeline tick `421168` in `src/graphics/host_gpu/renderer/masterSemaphore.cpp`.
- `MasterSemaphore::Wait()` first refreshes the timeline, then performs an unbounded `waitSemaphores(..., UINT64_MAX)`. Any non-success result currently reaches `EXIT_NOT_IMPLEMENTED`.
- `CommandScheduler::Submit()` reserves `MasterSemaphore::NextTick()` before `queue.submit`. A failed submit leaves a reserved tick that will never be signaled, but terminal handling currently does not prevent later users from waiting on it.
- Guest and presentation `CommandScheduler` instances own separate master timelines.
- Presentation uses per-slot acquire semaphores, presentation scheduler ticks, a render-complete semaphore, and `vkQueuePresentKHR`. Acquire/present handling covers resize and surface statuses but not device loss.
- Destructors and shutdown paths call timeline waits, `queue.waitIdle`, and `device.waitIdle`; these are unsafe or meaningless after device loss.
- No shared loss state stops guest submission and presentation threads. No CPU-visible fatal-state UX exists beyond logs/stdout.
- Depth-comparison descriptor and exact-format fixes removed the original validation error. They remain prerequisite baseline, not target of this plan.

## Product Contract

### Requirements

- **R1. One-shot loss latch:** First device-loss result records operation, Vulkan result, scheduler domain, tick, command debug metadata, and thread-safe timestamp/context. Later failures do not overwrite first cause; they increment a bounded occurrence counter.
- **R2. Terminal semantics:** Device loss is terminal for current emulator process. No swapchain-only recovery, device recreation, fake semaphore signal, or continued guest execution.
- **R3. No post-loss GPU submission:** Guest scheduler, presentation scheduler, deferred GPU callbacks, acquire, present, timeline refresh/wait, queue-idle, and device-idle paths become no-ops or return failure after latch.
- **R4. Explicit failure propagation:** Submission and presentation code can distinguish successful submission from terminal failure. A reserved tick is never reported as completed.
- **R5. Loss-safe shutdown:** Shutdown joins/stops CPU threads and releases CPU-owned state without waiting on lost Vulkan queues or timelines. Vulkan destruction uses one explicit documented lost-device policy.
- **R6. Presentation cancellation:** Pending, reserved, recording, and queued flip requests are resolved/cancelled; condition variables and guest-visible waiters are notified; frames are discarded without GPU waits.
- **R7. Diagnostics:** First-failure log identifies operation, result, scheduler domain, tick, command debug fields, acquire/present frame index when applicable, and whether failure was observed during submit, query, wait, acquire, present, idle, or creation.
- **R8. User-visible terminal status:** Emulator exposes a clear CPU-side fatal status through existing logging and window/application shutdown signaling. It must not promise a black frame after device loss.
- **R9. Telemetry correctness:** Device-loss reports mark capture incomplete, record first failure and dropped/cancelled work, and do not count ignored failed waits or submissions as successful completion.
- **R10. Regression coverage:** Tests cover loss latching, idempotence, submit-result propagation, loss-aware waits, cancellation notifications, and telemetry finalization without requiring a real lost Vulkan device.
- **R11. Bounded Astro validation:** Validation resumes only after GPU recovery/reboot and uses short, repeatable runs with explicit stop criteria. No sustained run while fault remains unidentified.

### Acceptance Examples

- **AE1. Submit loss:** Given `queue.submit` returns `eErrorDeviceLost`, when scheduler submits, then first-loss metadata is latched, result is reported as failure, no success tick is published, and later `Submit`, `BeginCommand`, or `Flush` calls do not issue Vulkan work.
- **AE2. Wait loss:** Given `waitSemaphores` returns `eErrorDeviceLost`, when a scheduler waits, then wait returns failure, telemetry records terminal loss, no `Refresh()` follows the failed wait, and shutdown does not wait on that timeline again.
- **AE3. Acquire/present loss:** Given acquire or present returns device loss, then shared loss state is latched with operation and frame index, present loop stops, queued flips are cancelled, and no retry treats the result as resize or surface recovery.
- **AE4. Failed-submit tick:** Given tick `N` was reserved and submit fails, then no callback, resource, command pool slot, or presentation frame may treat `N` as GPU-complete.
- **AE5. Loss-safe shutdown:** Given loss is latched while guest and present threads are active, shutdown wakes condition variables, joins threads, skips `waitIdle` and timeline waits, and exits without another fatal assertion caused by expected device-loss results.
- **AE6. Diagnostics:** Given multiple threads observe loss, exactly one first-failure record is emitted; subsequent observations identify the already-latched state without replacing root-cause metadata.
- **AE7. Normal path:** Given all Vulkan calls succeed, existing scheduler ticks, command reuse, acquire retirement, presentation, telemetry, and shutdown behavior remain unchanged.
- **AE8. Astro bounded run:** After implementation, a bounded Astro run either reaches its configured frame/time limit with no device loss, or exits at first loss with actionable first-failure metadata and no black-screen hang.

## Settled Technical Decisions

- **KTD1. Shared state belongs beside `GraphicContext`.** Add a small process/device-scoped fault state reachable by guest and presentation schedulers. Do not create independent scheduler-local loss flags; separate flags can race and continue one producer after another producer sees loss.
- **KTD2. Device loss is terminal.** Vulkan device objects, queues, command pools, timelines, swapchain images, descriptors, allocators, and caches are not recoverable in place. Recovery means process exit and a later fresh device initialization.
- **KTD3. Preserve first cause.** Use an atomic latch plus mutex-protected metadata or equivalent one-shot synchronization. Result observation must be lock-safe and non-blocking where called from error paths.
- **KTD4. Status-bearing scheduler boundaries.** Replace ambiguous `uint64_t` success-only assumptions at submission/presentation boundaries with an explicit success/failure result carrying tick only on success. Update all internal call sites rather than returning sentinel ticks.
- **KTD5. Failed submission never completes its tick.** A reserved tick may remain numerically consumed, but it is not inserted into completion bookkeeping and must not be waited on after loss. Terminal state prevents all later consumers.
- **KTD6. No Vulkan recovery after latch.** Result handling may report and short-circuit. It must not call `getSemaphoreCounterValue`, `waitSemaphores`, `queue.waitIdle`, `device.waitIdle`, swapchain recreation, or resource creation once loss is known.
- **KTD7. Keep Vulkan ownership unchanged.** Do not introduce asynchronous Vulkan calls, new queue threads, queue-family changes, or cross-thread device ownership while isolating this fault.
- **KTD8. Diagnostics first, mitigation second.** Add operation-level metadata and bounded logging before changing command content or synchronization policy. Do not hide the fault by weakening `MasterSemaphore::Wait` or converting loss to success.
- **KTD9. CPU fallback only for reporting.** Window title/log/application state may expose fatal status. A GPU-generated blank/black fallback is not valid after device loss.

## Implementation Plan

### Phase 0 - Establish clean baseline

1. Commit or isolate current depth-comparison descriptor changes before device-loss work. Keep generated `_SaveData/` out of source commits.
2. Record current branch, build configuration, GPU driver version, validation-layer setting, Astro title/version, and exact launch command.
3. Add a bounded diagnostic run script/log convention. Capture first loss, process exit code, and whether the window recovers after GPU reset.
4. Do not run sustained Astro validation until GPU has recovered/rebooted and the instrumentation build exists.

**Files:** existing descriptor fix; `docs/plans/2026-10-14-device-loss-isolation-plan.md`; validation notes outside source if needed.

### Phase 1 - Add shared device-loss state and result policy

1. Add `VulkanFaultState`/`DeviceLossState` near `GraphicContext` with:
   - atomic latched flag;
   - first operation and result;
   - scheduler domain and optional tick;
   - command debug fields and presentation frame/index fields;
   - first-observation timestamp/thread identifier where available;
   - bounded repeat-observation count;
   - query methods that do not touch Vulkan.
2. Add one common record function, conceptually:
   ```cpp
   bool LatchDeviceLoss(VulkanOperation operation, vk::Result result, FaultContext context);
   ```
   It should latch only `eErrorDeviceLost` (and optionally a separately classified fatal-device result if repository policy requires it), emit first-failure diagnostics once, and return whether caller won first-record ownership.
3. Add a shared `IsDeviceLost()` guard for producers and a result helper for Vulkan calls. Keep ordinary unsupported/error behavior distinct from device loss; do not turn every failed Vulkan call into device loss.
4. Define user-visible terminal notification through existing application/window lifecycle. The notification must be CPU-side and safe when the graphics device is already lost.
5. Document that state is process-lifetime terminal state and is not reset during swapchain recreation.

**Files:** `src/graphics/host_gpu/graphicContext.h` and new focused state implementation if needed; `src/graphics/host_gpu/vulkanCommon.cpp` or existing logging boundary; `src/common/assert.*` only if a non-fatal reporting seam is required; application/window status owner for UX.

**Tests:** pure state tests for first-writer wins, repeat count, metadata preservation, and non-device-loss result classification.

### Phase 2 - Make master timeline and scheduler loss-aware

1. Update `MasterSemaphore::Refresh()` to return success/status. On failed counter query, record operation/result and do not update cached completion.
2. Update `MasterSemaphore::Wait()` to return status. On failure, record loss and return without calling `Refresh()` afterward. Preserve wait telemetry as cancelled/failed rather than successful elapsed wait.
3. Guard `MasterSemaphore` destructor and all timeline operations. Destruction may destroy handles under the selected lost-device policy, but must never perform a wait or idle operation after loss.
4. Add scheduler `Active`/terminal checks before `BeginCommand`, `Commit`, `Flush`, `Finish`, `Wait`, deferred operation queuing, and priority-operation execution. Wake scheduler condition variables when loss latches.
5. Change `CommandScheduler::Submit()` to a status-bearing return. Reserve the tick and prepare submission as before, but after `queue.submit` failure:
   - latch device loss with scheduler/debug context;
   - do not call success telemetry or timestamp completion paths;
   - do not clear state as if command buffer was submitted;
   - return failure to caller and prevent all later GPU calls.
6. Ensure `CurrentTick()`/`NextTick()` semantics are documented around reserved versus submitted ticks. A failed reserved tick is never considered free or completed.
7. Update all call sites in `commandScheduler.cpp`, swapchain code, and any other `Submit()` users to branch on failure. Do not use `0`, `CurrentTick()`, or a failed reserved tick as a completion sentinel.
8. Make command-pool reuse and timestamp collection stop cleanly after loss. No `Refresh()` from pool allocation, timestamp polling, deferred callbacks, or garbage collection after latch.

**Files:**
- `src/graphics/host_gpu/renderer/masterSemaphore.h/.cpp`
- `src/graphics/host_gpu/renderer/commandScheduler.h/.cpp`
- `src/graphics/host_gpu/renderer/gpuTimestampCollector.cpp` and header if its polling contract needs status
- `src/graphics/host_gpu/renderer/render.h` if submission result type is shared
- deferred-resource users such as `bufferCache.cpp`, `streamBuffer.cpp`, `faultManager.cpp`, and descriptor heap paths where waits are invoked

**Tests:** scheduler tests using injected/fake result seams; verify no second Vulkan call after latched loss, failed tick is not completed, normal successful tick behavior unchanged, and shutdown skips timeline waits.

### Phase 3 - Make acquire, presentation, and flip queues terminal-safe

1. Add explicit `eErrorDeviceLost` handling to `Swapchain::AcquireNextImage`, `Swapchain::Present`, and presentation submit. Record operation/frame-slot metadata and return terminal failure, not resize/surface retry.
2. Guard swapchain acquire, command recording, submit, present, resize/recreate, and semaphore retirement with shared loss state.
3. Update `Presenter`/`FlipQueue` to distinguish `Presented`, `Skipped`, `CancelledByDeviceLoss`, and ordinary retryable statuses as needed. Do not report a cancelled request as presented.
4. Add a device-loss cancellation path that walks pending CPU-owned requests and:
   - transitions reserved/recording requests to cancelled;
   - discards owned frames without GPU waits;
   - updates flip counters/status fields consistently;
   - signals done/submit-slot/request condition variables;
   - resolves or releases guest-visible wait state according to existing video-out semantics.
5. Stop `PresentThread` when shared loss is latched. Check state before vblank work, flip dequeue, overlay fallback, blank-frame preparation, and each presentation attempt.
6. Ensure `FlipQueue::Cancel` cannot wait forever for a presenting request after loss. Loss path must not invoke GPU work to finish that request.
7. Preserve acquire-semaphore retirement fix on normal path. After loss, do not attempt retirement waits or semaphore reuse.

**Files:**
- `src/graphics/presentation/window/swapchain.cpp`
- `src/graphics/presentation/window/vulkanWindow.cpp`
- `src/graphics/presentation/videoOut.cpp`
- `src/graphics/presentation/presenter.h` and implementation owner
- `src/graphics/presentation/window/window.cpp` if terminal window status is owned there

**Tests:** status mapping tests for acquire/present; flip-queue cancellation tests for reserved/recording/pending states; condition-variable wakeup tests; normal out-of-date/surface-lost behavior remains unchanged.

### Phase 4 - Make teardown loss-safe

1. Define destruction policy in one documented place:
   - normal path drains schedulers and uses existing waits/idle calls;
   - lost-device path skips timeline waits, `queue.waitIdle`, and `device.waitIdle`;
   - CPU thread joins and container cleanup still occur;
   - Vulkan handle destruction follows Vulkan/device-loss project policy and is not used as a reason to issue additional queue work.
2. Update `CommandScheduler::Shutdown()` to detect loss before final submit, timeline wait, timestamp drain, and pending callback execution. Cancel pending callbacks/resources that require GPU completion rather than running them against lost state.
3. Update swapchain destruction and window context destruction to bypass idle calls after loss.
4. Update `RenderContext::ShutdownGpu`, `VideoOutShutdown`, allocator/cache teardown, and relevant resource destructors to avoid hidden waits or result assertions after loss.
5. Ensure shutdown is idempotent if multiple threads race to request stop or observe loss.
6. Publish terminal status before process exit. Keep teardown bounded; no infinite wait on a lost timeline.

**Files:**
- `src/graphics/host_gpu/renderer/commandScheduler.cpp`
- `src/graphics/host_gpu/renderer/renderContext.cpp`
- `src/graphics/presentation/window/swapchain.cpp`
- `src/graphics/presentation/window/vulkanWindow.cpp`
- `src/graphics/presentation/videoOut.cpp`
- allocator/cache/resource destruction sites identified by compile/test failures

**Tests:** shutdown tests with pre-latched loss; concurrent guest/presentation shutdown; repeated shutdown; no calls to fake wait/idle after loss.

### Phase 5 - Telemetry and diagnostics integration

1. Add device-loss fields to performance report only when telemetry is enabled, while keeping fatal diagnostics available regardless of telemetry.
2. Record:
   - first operation/result;
   - first tick and scheduler domain;
   - debug operation/submit ID/arguments;
   - acquire/present frame index when applicable;
   - submissions attempted/succeeded/failed;
   - waits attempted/completed/failed/skipped;
   - cancelled flips and deferred operations;
   - report completion state (`complete`, `device_lost`, or equivalent partial status).
3. Ensure failed `Submit`, `Wait`, `Refresh`, acquire, and present are not counted as successful work.
4. Keep logging bounded: one detailed first-failure record plus compact repeated-observation count. Avoid per-frame spam after latch.
5. Add diagnostic build/run instructions and a log signature for automated triage.

**Files:** `src/common/performanceTelemetry.*`, `src/common/profiler.*` only where existing report hooks require it; scheduler/presentation call sites; `docs/performance-telemetry.md` and incident documentation.

**Tests:** JSON schema/report tests for device-loss partial captures; telemetry disabled path; repeated loss observations; normal report compatibility.

### Phase 6 - Controlled validation and Astro bisect

1. Build Release and test targets after each phase boundary. Run focused unit tests first.
2. Reboot/recover GPU after previous driver reset. Confirm desktop/Vulkan health with a minimal emulator startup before Astro.
3. Run bounded Astro attempts with fixed command, timeout, and log path:
   - startup only;
   - short frame/time window after first gameplay workload;
   - progressively longer windows only if prior run has no loss or validation error.
4. Compare cold-cache and warm-cache runs separately. Record shader counts, first loss operation/tick, frame index, and whether run reaches bound.
5. If loss persists, bisect workload gates rather than extending runtime: presentation-only, guest submit without presentation, shader-heavy scene, depth-heavy scene, descriptor/image path, then candidate command class.
6. Use validation layers and targeted synchronization diagnostics for short runs only. Disable broad noisy diagnostics for longer bounded confirmation.
7. Exit at first device loss. Verify process reports root operation and terminates cleanly without black-screen hang or unsafe recovery.
8. Only after stable bounded runs, perform one longer Astro validation and document result. Performance comparison is separate from fault isolation.

**Artifacts:** build log, focused test result, bounded Astro logs, first-loss metadata, driver/GPU details, and validation summary.

## Test Strategy

### Unit and seam tests

- Device-loss state first-writer/idempotence test.
- Vulkan result classifier test: success, retryable surface result, ordinary failure, device loss.
- Scheduler submit result test with fake queue result.
- Master semaphore refresh/wait result propagation test.
- Failed tick never becomes `IsFree()` test.
- Loss guard prevents refresh/wait/submit/command-pool polling after latch.
- Flip cancellation test for each request state and condition-variable notification.
- Shutdown test proves timeline/queue/device idle calls are skipped after loss.
- Telemetry report test proves partial status and failed-operation counts.

### Existing regressions to retain

- Shader module cache tests and recompiler tests.
- Descriptor/depth-comparison tests and exact-format behavior.
- Swapchain acquire-semaphore retirement behavior on normal path.
- Performance telemetry tests.
- Relevant resource tracking tests.

### Build and runtime gates

- Compile Debug and Release configurations.
- Run focused CTest targets first; record unrelated existing Windows failures separately, especially `kernel_file_system` (`0xc0000409`).
- Run normal startup without a game to verify no regression in usage/exit behavior.
- Run bounded Astro only after focused tests and GPU recovery.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Failed-submit call sites interpret sentinel as valid tick | Use explicit status-bearing result; update every caller and add failed-tick test. |
| Loss latch races with guest/presentation submission | Shared atomic latch checked before and after queue-lock acquisition; first-failure metadata one-shot. |
| Shutdown deadlocks on flip or deferred-operation condition variables | Loss path signals all relevant condition variables and avoids callbacks requiring GPU completion. |
| Resource destruction calls hidden waits | Audit all shutdown/destructor paths; centralize loss-aware idle/wait policy; add fake-call assertions. |
| Device-loss logs obscure root operation | Capture first operation, domain, tick, debug fields, frame slot, and result at detection point. |
| Overbroad result handling hides non-device bugs | Classify only explicit device-loss results as terminal; retain existing behavior for ordinary failures. |
| CPU fallback touches invalid graphics objects | Use process/window logging state only; never allocate or present through Vulkan after latch. |
| Instrumentation changes timing and masks fault | Keep diagnostics bounded and opt-in where possible; compare with diagnostics disabled after behavior is stable. |
| Astro rerun triggers another driver reset | Reboot/recover first; use timeout and short bounded windows; stop at first loss. |

## Out of Scope

- Recreating Vulkan device or swapchain after `VK_ERROR_DEVICE_LOST`.
- Changing Windows TDR settings or GPU driver policy.
- Moving Vulkan work to asynchronous threads.
- Rewriting command submission, descriptor allocation, image barriers, or render graph.
- Broad shader optimization, cache policy redesign, or new title-specific replay infrastructure.
- Claiming black-frame presentation after device loss.

## Completion Checklist

- [ ] Shared terminal device-loss state implemented and tested.
- [ ] First-failure diagnostics include operation/result/tick/domain/debug context.
- [ ] Submit, timeline query/wait, acquire, and present propagate failure explicitly.
- [ ] No failed tick is reported as completed.
- [ ] Guest/presentation producers stop after loss.
- [ ] Flip queues and deferred work cancel without GPU waits.
- [ ] Shutdown skips timeline/queue/device waits after loss and remains bounded.
- [ ] Telemetry reports partial/device-loss status accurately.
- [ ] Focused tests pass; unrelated test failures recorded separately.
- [ ] Bounded Astro run either completes target window cleanly or exits with actionable first-loss report.
- [ ] Longer validation and performance comparison performed only after stable bounded run.

## Handoff to Implementation

Start with Phase 1 and Phase 2 in one small change set only if result seams can be tested without Vulkan. Do not begin Astro validation from an intermediate build. After scheduler tests pass, implement presentation cancellation and teardown before any real-title run. Keep the current depth-comparison and acquire-semaphore fixes intact. Commit each phase separately so a later Astro result can be bisected against a known-good baseline.
