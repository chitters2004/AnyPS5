# Parallel commit notes (draft branch only, not for a PR)

Draft notes for the fork. AGENTS.md keeps notes out of the repository, so drop this file before any branch built on this one is proposed upstream.

## SHAs

- `upstream/main` (boykopovar/AnyPS5): `dc9dc7eaae`
- #14051 head: `aa0880e4eb`, based on `73f4f7e06e` (556 commits behind main)
- johandavidcm `feat/agc-draw-prepare-ahead`: `4c1af4f484`, based on `18ef6be0cd`; it already contains #10678 (`4d4b48da5`, `7b0b45a52`)
- `pull/7895/head` does not exist: #7895 is an issue, not a PR

## Verdict

#7895 is an issue that holds the plan for parallel draw preparation. #14051 is step 2 of that plan: it adds a seam and parallelises nothing. It does not merge cleanly on current main and has a change request against it. Even if the whole plan works, it can't reach ~67 ms a frame on GTA V; the realistic gain is about 10-15%.

## The plan in #7895

oneandonlydean (maintainer) agreed this shape:

1. Split draws into prepare and record. Merged as #8992.
2. Make prepare's read-only work explicit. This is #14051 (vingelvatten). johandavidcm also has a stage A for it on his branch.
3. Helper threads prepare draws N+1..N+k in guest order. The committer checks each draw before using it (CPU writes and queued GPU writes over its read ranges) and prepares it inline if the check fails. This exists only as johandavidcm's `feat/agc-draw-prepare-ahead`, with one `DrawPreparer` thread. It isn't a PR yet: he's waiting for #10678.
4. Per-pass secondary command buffers, only if recording becomes the limit. Not started.

Recording, cache inserts, snapshots and submit stay serial, on one recorder, in guest order.

The maintainer's bar for every step:

- behind a switch, off by default
- before/after frame timings from the same scene
- a window test that writes between prepare and record, per write path
- per-title survival rates in the PR description

Known defect on the preparer branch: `agc_driver_draw_prepare_ahead` segfaults at exit 6 of 6 times, with `VUID-vkDestroyDevice-device-05137` (Tehshortbus, 28 Oct). The thread-local reclaim list in `DrawPipeline::Enqueue` outlives the device.

## #14051 asks

- MarcZX10 requested changes.
  - Blocking: "agree with the #10678 author on landing order and update the dependency checklist on the later PR."
  - Non-blocking: `planDraw` calls `VertexInputCache::TryGet`, which changes the cache. The reviewer asks to document that in the plan contract or move it out of the read-only stage.
- janzezno: #12862 split the test registrations. Put the `_plan_verify` registration inside `agc_add_draw_pipeline_test`.
- On current main it conflicts in `CMakeLists.txt` and in one hunk of `Graphics/src/Draw.cpp`. Both are resolved on this branch (see below).

## Amdahl on the GTA V committer (about 100 of 130 ms a frame)

| Share | Work |
|---|---|
| p = 42.6% | could leave the committer: ShaderResources constructor 23.8, Revalidate 9.6, CopyDrawInput 9.2 |
| 57.4% | stays serial: UploadFinish 17.9, copyRegion 13.8, Collect 6.6, Submit 6.4, other 2.7 |

| Workers | Committer time | Speedup |
|---|---|---|
| unlimited | ~57 ms | 1.74x |
| 4 | ~68 ms | 1.47x (matches the ~67 ms estimate) |

Step 2 keeps cache inserts and snapshots on the committer, so only the read-only part moves. johandavidcm measures that at 10-15% of the committer: about 87 ms at best. His measurements were -4.8% committer time and -1% frame on Yakuza.

UploadFinish plus copyRegion (31.7%) is the real cap. Only step 4 or batched uploads touch it.

## Shared state on the draw path (main `dc9dc7eaae`)

- `Graphics/src/MultisampleTarget.cpp:785` `guardedMutex` and `Graphics/src/DepthSurface.cpp:84` `depthGuardMutex` are `std::shared_mutex`, so winpthreads rwlocks on MinGW.
  - `2659ec333` moved other draw-path locks to the futex `AgcDriver::Mutex` (`Execution/include/Mutex.hpp`). There is no shared version of it.
- Guard checks return early on `anyGuarded()`/`anyDepthGuard()` when no guard exists. Once any guard is live, every texture, storage, index and vertex check takes a shared lock:
  - `RequireNoMultisampleTarget` from `Draw.cpp` and `ShaderResources.cpp`
  - `RoutesToNativeDepthOwner`
  - `HostOnlySampleRanges`
- `MultisampleTargetRangeGeneration` (`MultisampleTarget.cpp:1515`) locks just to read the `guardedGeneration` atomic, on every call. It is called from `BdaResources.cpp:121`.
- emutls: about 20 plain `thread_local`s in `Graphics/src/Draw.cpp` and `Graphics/src/ShaderResources.cpp`. `HostThreadSlot` (`libc/include/HostThreadSlot.hpp`) exists to avoid emutls but isn't used there.
- Label compare: `DrawFinisher::PendingLabel` (`Execution/src/Driver/Draw/DrawPipeline.cpp:83`) scans the pending draws under the pipeline mutex.
- Not verified here: that winpthreads rwlocks go through a global spinlock (no winpthreads source in the container).

## Suggested ordering

1. On the preparer, with no lock: read the guard generation, then build the plan. Never take the guard `shared_mutex` there.
2. On the committer at adopt time, in this order:
   - Compare the generation (one atomic load).
   - Run the write checks (allocation generation, `GuestMemory::ForgetSerial`, `DrawPipeline::WrittenSince` over read ranges).
   - If either fails, cut: prepare inline.
3. Do the label compare last, and only on wait packets, because it alone needs the pipeline mutex. Commit cuts fall at those label waits and at failed checks.

## Plan

1. Run a Windows GTA V A/B: main against prepare-ahead plus #14051.
   - At least three alternated pairs on the same scene.
   - `APS5_PROFILE_DRAW=1`, plus `tools/trace_digest.py` summaries of both arms.
   - Record survival counts: prepared, adopted, not ready, refused.
2. Read the preparer thread's profile from that run for winpthreads rwlock time and `__emutls_get_address`.
3. Post the results on #7895.
4. Only if step 2 shows the locks are hot: make the lock-free guard check its own small PR, with its own `trace_digest` before and after.

## What this branch holds

`draft/parallel-commit-notes` = `upstream/main` `dc9dc7eaae` + #14051 (`aa0880e4eb`) merged, with these resolutions:

- `Graphics/src/Draw.cpp`: the plan's alias check uses main's `state.color.MemoryAddress()`. Main's occlusion partial-write check stays in `prepareDrawInputs`, because it reads `Recorder::Active()` (record-time state).
- `CMakeLists.txt`: dropped the PR's copy of the old test block, which main moved in #12862. The `_plan_verify` registration now sits inside `agc_add_draw_pipeline_test`.


## Next: merge the preparer branch (open)

Merging `johandavidcm/feat/agc-draw-prepare-ahead` on top conflicts in:

- `Execution/include/Driver/Driver.hpp`
- `Execution/include/VulkanDevice.hpp`
- `Execution/src/Driver/Draw/Draw.cpp`
- `Execution/src/Driver/Draw/DrawPipeline.cpp`
- `Execution/src/VulkanDevice.cpp`
- `Graphics/include/Draw.hpp`
- `Graphics/src/Draw.cpp`
- `docs/dev/ENVIRONMENT.md`

Both his stage A and #14051 restructure `prepareDraw`. Resolved in `98e9ff33a`; see above.

## Preparer branch merged, exit crash fixed, built and tested

- `98e9ff33a` merges johandavidcm `feat/agc-draw-prepare-ahead` (`4c1af4f48`, with #10678) onto the #14051 merge.
  - The prepared-ahead fragment outputs, stages and vertex layout, and the early resource key, feed `planDraw`.
  - An adopted pipeline takes precedence over main's shape memo.
  - Main's `shape` parameter sits before `contentKey` and `ahead` in every `Draw` and `commitDraw` signature.
  - The pipelined summary keeps main's separate read-range field.
- `d73ca62fb` fixes the `agc_driver_draw_prepare_ahead` exit crash.
  - Cause: `AheadDraw` is shared by the commit and the preparer job. Either can outlive the other in a thread's reclaim list until the thread exits, so the device could be destroyed before the prepared pipeline.
  - Fix: `AheadDraw` now holds the device ahead of `prepared`, so the pipeline is always released first.
- Built on Linux: GCC 13, Release, `BUILD_TESTING=ON`, targets `libSceAgcDriver`, the preparer test and the 22 draw-pipeline tests. No compile errors.
- Tests on lavapipe (llvmpipe, LLVM 20.1.2) with `ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`:
  - `agc_driver_draw_prepare_ahead` before the fix: 6 of 6 runs segfault at exit, with `VUID-vkDestroyDevice-device-05137`.
  - After the fix: 6 of 6 pass, with no `05137`.
  - The 22 draw-pipeline tests in their default, `_draw_pipeline` and `_plan_verify` variants, plus the preparer test: 67 of 67 pass, none skipped. They also pass 67 of 67 with `APS5_DRAW_PREPARE_AHEAD=1` added.
  - The only validation error is `VUID-VkDeviceCreateInfo-pNext-pNext`. It comes from the installed validation layer (1.3.275) being older than a structure the driver passes, and is environmental.
  - No log has the layer-missing line.
  - `main` was not run as a comparison arm, and no Windows build was made.

## What could not be verified

- About 3 hidden comments on #7895 (26 Oct) and the end of panicDa3m0n's 26 Oct comment: read only as a summary.
- #14051: inline review threads, per-job CI results, the "needs rebase" label as shown on the page, and janzezno's comment (summary only).

## Commands

```bash
git remote add upstream https://github.com/boykopovar/AnyPS5.git
git remote set-url --push upstream DISABLED
git fetch upstream main pull/14051/head:refs/remotes/upstream/pr14051
git fetch https://github.com/johandavidcm/AnyPS5.git feat/agc-draw-prepare-ahead:refs/remotes/johandavidcm/feat/agc-draw-prepare-ahead
```
