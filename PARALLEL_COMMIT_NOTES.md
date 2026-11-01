# Parallel commit report (draft branch only, not for a PR)

This is the single report for `draft/parallel-commit-notes` on chitters2004/AnyPS5. It covers the research on upstream #7895 and #14051, what this branch holds, and the build and test results.

AGENTS.md keeps notes out of the repository. Drop this file before any branch built on this one is proposed upstream; the conventions check flags it as `[notes-file]`.

Rules this work followed:

- Upstream boykopovar/AnyPS5 is fetch-only. Its push URL is set to `DISABLED`.
- Nothing was pushed upstream, and no comment, review or PR was posted anywhere.
- #14051 and the preparer work are other authors'. Nothing here is meant to compete with them.

## 1. State at a glance

| What | SHA |
|---|---|
| `upstream/main` | `dc9dc7eaae` |
| #14051 head (vingelvatten) | `aa0880e4eb`, based on `73f4f7e06e`, 556 commits behind main |
| johandavidcm `feat/agc-draw-prepare-ahead` | `4c1af4f484`, based on `18ef6be0cd`; contains #10678 (`4d4b48da5`, `7b0b45a52`) |
| this branch | main + #14051 + prepare-ahead + exit-crash fix |

`pull/7895/head` does not exist: #7895 is an issue, not a PR.

**Verdict.** #7895 is an issue that holds the plan for parallel draw preparation. #14051 is step 2 of that plan: it adds a seam and parallelises nothing. It does not merge cleanly on current main and has a change request against it. Even if the whole plan works, it can't reach ~67 ms a frame on GTA V; the realistic gain is about 10-15% (section 4).

**This branch** builds on Linux, and its preparer test no longer crashes at exit. It matches main on the shared draw-pipeline tests with no new validation errors (section 8). It has not been built on Windows or measured in a game.

**Next step:** a Windows GTA V A/B, main against this branch (section 7).

## 2. Issue #7895: the plan for parallel draw preparation

- Title: "the queue-0 draw committer bounds draw-heavy titles on the CPU — is parallel draw recording planned?"
- Opened by SP4C3B4R-8 on 19 Oct; about 45 comments.

The opening measurement (Astro Bot Crash Site, RTX 5070 Ti, 7800X3D):

- The committer is busy about 88 of 102 ms a frame.
- Worker 0 is blocked about 72% of the time, mostly in `DrawPipeline::Enqueue`.
- 87.5% of draws read nothing the previous 16 draws wrote.

### The series

oneandonlydean (maintainer) endorsed it on 20-21 Oct. MichonGoddijn231849 was @-mentioned but never commented.

1. **Split `Graphics::Draw` into prepare and record.** Merged as #8992.
2. **Make prepare's read-only work explicit.**
   - #14051 (vingelvatten) does this as `planDraw`.
   - johandavidcm's "stage A" on his branch does the same job.
3. **Helper threads prepare draws N+1..N+k in guest order.**
   - At record or adopt time the committer checks for CPU writes and queued GPU writes over the draw's read ranges. If the check fails, it prepares the draw inline.
   - Only johandavidcm's branch implements this, with one `DrawPreparer` thread (bounded queue of 64) behind `APS5_DRAW_PREPARE_AHEAD=1`.
   - Not a PR yet: he's waiting for #10678.
4. **Per-pass secondary command buffers**, only if recording becomes the limit. Not started.

Recording, cache inserts, snapshots and submit stay serial, on one recorder, in guest order.

### Maintainer requirements for every step (oneandonlydean)

- The step is behind a switch, off by default.
- Before/after frame timings from the same scene.
- The adopt-time check covers CPU writes and queued GPU writes, with an inline fallback.
- A window test that writes between prepare and record and fails without the check. One test with two cases (CPU store, and a queued GPU write via `DrawWriteRanges`) was accepted.
- Per-title survival rates in each PR description.
- In-place buffers and GPU-sampled images need no check.
- BDA and image tables resolved on the CPU must fail the check on a queued write; unbounded read sets record in place.
- For #8992: "identical output and timings within noise on the same scene."

### johandavidcm's adopt-time check contract

In order:

1. `GuestAllocationsGeneration_nid_postfix`
2. `GuestMemory::ForgetSerial`
3. `DrawPipeline::WrittenSince` over the read ranges, against `WatchWrites` taken before stage A

A mismatch prepares inline under the mutex. Survival counts print in the `[draw] pipelined` line under `APS5_PROFILE_DRAW`.

### Measured results for the preparer (reported in the thread)

| Who | Title, hardware | Result |
|---|---|---|
| johandavidcm | Yakuza 0 | committer 168.4 → 160.3 ms (-4.8%); frame p50 -1.0%, within noise |
| BunnyFl | Heavenly Bodies, RX 9070 XT | +4.3% presents; 169-177k adopted, 0-27 refused |
| Tehshortbus | GTA V cutscene 1, RTX 4070 SUPER, Linux | +3-5%; 71% prepared, 79% of those adopted; cutscene 2 unchanged |
| chitters2004 (you) | GTA V, RTX 3080 | "probably +3-5%, can't settle it"; run order biased the result |

Earlier, johandavidcm measured that moving prepare work to the draw finisher no longer pays: the finisher gets slower by about as much as the committer gains. That is why the preparer has its own thread.

### Related threads

- **Dispatch side (m335671):**
  - D1 is #8278.
  - D4a is #9888 (an adoption check; 0 rejections on Sword of the Sea).
  - D4b, preparing queue-0 dispatches ahead on a helper, is a design only. Its ceiling is about 9-13%.
- **Worker drains:** johandavidcm found worker 0 spends 62.6% of its time in `DrawFinisher::Drain`, mostly from `drawRelocationPending`. `APS5_NO_DRAW_RELOCATION=1` gave -4% p50 on Yakuza.
- **Spin windows:** the GPU-mutex hand-off and the committer's wait for work spin up to 150 µs (`HandoffSpin`, on machines with more than 8 CPUs). `DrawPipeline::Drain` spins up to 300 µs.

### Not mentioned anywhere in the thread

`std::shared_mutex`, SRWLOCK, emutls, the guard registries, and label compares as a cost.

## 3. PR #14051: `refactor(agc): split prepareDraw's read-only plan from its record-time work`

Author vingelvatten, opened 31 Oct, one commit `aa0880e4eb`, 3 files changed (+116/-45). Labels: build, docs, agc, `libSceAgc*`, and "needs rebase" (seen only in a summary of the page).

### What it does

| Piece | What it does |
|---|---|
| `planDraw` → `DrawPlan` | Validation and the early nothing-to-draw exits; fragment outputs, pipeline stages, mesh group count, viewport, depth-bounds and sample-mask checks, vertex input layout, and the resource-cache key. Still runs on the committer. |
| `prepareDrawInputs` | The record-time work: index and vertex captures, the multisample-target check, the vertex fetch plan. |
| `resolveDrawResources` | Takes the key from the plan. |

`APS5_VERIFY_DRAW_PLAN=1` rebuilds the plan after the record-time work and requires it to match. It adds 22 `_plan_verify` test variants.

It makes no speed claim. Its own A/B (Crash Site, RTX 4070 Ti SUPER) measured 74.5 vs 74.3 presents per 10 s, which is noise.

### Review state

- **MarcZX10, changes requested:**
  - Blocking: "Please agree with the #10678 author on landing order and update the dependency checklist on the later PR." The checklist says "Depends on: none", but the PR overlaps #10678.
  - Non-blocking: `planDraw` calls `VertexInputCache::TryGet`, which changes the cache (LRU, insert/evict, bypass bit). Yet the PR says the plan touches "no shared cache". The ask is to "describe this synchronized cache interaction in the plan contract and verifier rationale, or move the cache operation outside the read-only stage."
- **TechLevelZero's automated GPU report:** passed on an RTX 2000 Ada, with no new validation errors.
- **janzezno:** #12862 split the test registrations; put the `_plan_verify` lines inside `agc_add_draw_pipeline_test`. Seen only as a summary of the page.
- **Overlap bot:** could not compare some files because the PR conflicts with main.

### Does it apply on current main?

No. `git merge-tree` shows conflicts in two places:

- `core/libs/prx/libSceAgcDriver/CMakeLists.txt`, from #12862.
- One hunk of `Graphics/src/Draw.cpp`. Main renamed `color.address` to `MemoryAddress()`, and added an occlusion partial-write check that reads `Recorder::Active()`, which is record-time state.

## 4. Amdahl on the GTA V committer

The committer takes about 100 of the 130 ms frame on Windows.

| Share | Work |
|---|---|
| p = 42.6% | could leave the committer: ShaderResources constructor 23.8, Revalidate 9.6, CopyDrawInput 9.2 |
| 57.4% | stays serial: UploadFinish 17.9, copyRegion 13.8, Collect 6.6, Submit 6.4, other 2.7 |

| Workers | Committer time | Speedup |
|---|---|---|
| unlimited | ~57 ms | 1.74x |
| 4 | ~68 ms | 1.47x (matches the prior ~67 ms estimate) |

Step 2 keeps cache inserts and snapshots on the committer, so only the read-only part moves. johandavidcm measures that at 10-15% of the committer: about 87 ms at best. That fits the measured -4.8% committer time and -1% frame.

UploadFinish plus copyRegion (31.7%) is the real cap. Only step 4 or batched uploads touch it.

Queue depth 64 made drains double, so a deeper queue is not a lever.

## 5. Shared state on the draw path (main `dc9dc7eaae`)

### Guard registries

- `Graphics/src/MultisampleTarget.cpp:785` `guardedMutex` and `Graphics/src/DepthSurface.cpp:84` `depthGuardMutex` are `std::shared_mutex`, which is a winpthreads rwlock on MinGW.
- Commit `2659ec333` moved the other draw-path locks to the futex `AgcDriver::Mutex` (`Execution/include/Mutex.hpp`). There is no shared version of it.
- Guard checks return early through `anyGuarded()` / `anyDepthGuard()` when no guard exists.
- Once any guard is live, every texture, storage, index and vertex check takes a shared lock:
  - `RequireNoMultisampleTarget`, called from `Draw.cpp` and `ShaderResources.cpp`
  - `RoutesToNativeDepthOwner`
  - `HostOnlySampleRanges`
- `MultisampleTargetRangeGeneration` (`MultisampleTarget.cpp:1515`) takes the lock just to read the `guardedGeneration` atomic, on every call. It is called from `BdaResources.cpp:121`.
- The `guardedGeneration` atomic already exists, so a lock-free check is possible.

### emutls

- About 20 plain `thread_local`s in `Graphics/src/Draw.cpp` and `Graphics/src/ShaderResources.cpp`.
- On MinGW each new preparer thread pays emutls first-touch and per-access cost.
- `HostThreadSlot` (`libc/include/HostThreadSlot.hpp`) exists to avoid this but isn't used there.

### Label compare

`DrawFinisher::PendingLabel` (`Execution/src/Driver/Draw/DrawPipeline.cpp:83`) scans the pending draws under the pipeline mutex.

### Not verified

That a winpthreads rwlock goes through a global spinlock. There was no winpthreads source in the container.

## 6. Suggested ordering

1. **On the preparer, with no lock:** read the guard generation, then build the plan. Never take a guard `shared_mutex` on a preparer thread.
2. **On the committer at adopt time, in this order:**
   - Compare the generation (one atomic load).
   - Run the write checks: allocation generation, `ForgetSerial`, `WrittenSince`.
   - If either fails, cut: prepare inline.
3. **The label compare goes last**, and only on wait packets, because it alone needs the pipeline mutex. Commit cuts fall at those label waits and at failed checks.

## 7. Plan

1. **Windows GTA V A/B:** main against this branch.
   - At least three alternated pairs on the same scene, so run order can't bias the result.
   - `APS5_PROFILE_DRAW=1` on both arms, `APS5_DRAW_PREPARE_AHEAD=1` on the branch arm.
   - `tools/trace_digest.py` summaries of both arms.
   - Survival counts: prepared, adopted, not ready, refused.
2. **Read the preparer thread's profile** from that run for winpthreads rwlock time (the guard mutexes) and `__emutls_get_address`.
3. **Post the results on #7895.** That is the evidence the maintainer asks for, and there is no Windows GTA V preparer data yet.
4. **Only if step 2 shows the locks are hot:** make the lock-free guard check its own small PR, with its own `trace_digest` before and after and Vulkan validation runs.

## 8. What this branch holds, and test results

### Commits on top of main

| Commit | What |
|---|---|
| `60eadd98b` | merges #14051 |
| `98e9ff33a` | merges `feat/agc-draw-prepare-ahead` |
| `d73ca62fb` | fixes the preparer test's exit crash |
| docs commits | this report |

### Resolving the #14051 merge (`60eadd98b`)

- `Graphics/src/Draw.cpp`: the plan's alias check uses `state.color.MemoryAddress()`. Main's occlusion partial-write check stays in `prepareDrawInputs`, because it reads record-time state.
- `CMakeLists.txt`: dropped the PR's copy of the old test block, which #12862 moved. The `_plan_verify` registration now sits inside `agc_add_draw_pipeline_test`, as janzezno suggested.

### Resolving the prepare-ahead merge (`98e9ff33a`)

The merge had 21 conflicting hunks in 8 files:

- `Driver.hpp`
- `VulkanDevice.hpp`
- `Execution/.../Draw.cpp`
- `DrawPipeline.cpp`
- `VulkanDevice.cpp`
- `Graphics/include/Draw.hpp`
- `Graphics/src/Draw.cpp`
- `ENVIRONMENT.md`

How they were resolved:

- **Into the plan:** the prepared-ahead fragment outputs, stages and vertex layout, and the early resource key (`KeyForDevice`, `APS5_VERIFY_EARLY_DRAW_KEY`), feed #14051's `planDraw`. So `APS5_VERIFY_DRAW_PLAN` also checks prepared values against a fresh plan.
- **Pipeline choice:** an adopted prepared pipeline takes precedence. Otherwise main's shape-memo path runs as before.
- **Signatures:** main's `shape` parameter sits before `contentKey` and `ahead` in every `Draw` and `commitDraw` signature. All callers were checked.
- **Summary line:** the pipelined summary keeps main's separate read-range field and adds the preparer counters, with no duplicate output.
- **Test registration:** main registers the preparer test (`agc_driver_draw_prepare_ahead`) automatically from `tests/execution`.

### Exit-crash fix (`d73ca62fb`)

- **Symptom:** reported by Tehshortbus on #7895. The test segfaults at exit with `VUID-vkDestroyDevice-device-05137`.
- **Cause:**
  - `AheadDraw` is shared by the commit lambda and the preparer job.
  - Either can outlive the other in a thread-local reclaim list (`DrawPipeline::Enqueue`, `DrawPreparer::Enqueue`) until the thread exits.
  - The order between the device and the prepared pipeline was whatever order the closures died in. So `vkDestroyDevice` could run with the pipeline alive, and `vkDestroyPipeline` then faulted.
- **Fix:** `AheadDraw` holds `std::shared_ptr<VulkanDevice> device`, declared before `prepared`. The prepared pipeline is therefore always released before that device reference. The preparer job uses `ahead->device`.

### Build

Linux, GCC 13, Release, `BUILD_TESTING=ON`, both arms built in separate trees from the same submodules:

| Arm | Tree | Targets | Compile errors |
|---|---|---|---|
| branch | `build/` | `libSceAgcDriver`, the preparer test, the 22 draw-pipeline tests | none |
| main `dc9dc7eaae` | `/home/user/AnyPS5-main/build` | `libSceAgcDriver`, the 22 draw-pipeline tests | none |

### Tests

All runs used lavapipe (llvmpipe, LLVM 20.1.2, Mesa) with `ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`.

| Run | Result |
|---|---|
| `agc_driver_draw_prepare_ahead`, branch without the fix | 6 of 6 segfault at exit, `05137` logged |
| `agc_driver_draw_prepare_ahead`, branch with the fix | 6 of 6 pass, no `05137` |
| main vs branch: the 22 draw-pipeline tests, default and `_draw_pipeline` (44 each) | main 44/44, branch 44/44, none skipped |
| branch: the same 22 plus `_plan_verify` and the preparer test (67) | 67/67, none skipped |
| branch: the same 67 with `APS5_DRAW_PREPARE_AHEAD=1` | 67/67, none skipped |

Validation results:

- Both arms log the same error: `VUID-VkDeviceCreateInfo-pNext-pNext`, 186 lines in each 44-test log.
- It is environmental: the installed validation layer (1.3.275) doesn't know a structure the driver passes at device creation.
- The branch adds no new `VUID-` errors.
- No log has `was not found but was requested by env var VK_INSTANCE_LAYERS`.

## 9. What could not be verified

- **Thread coverage:** about 3 comments in #7895's hidden block (26 Oct: BunnyFl, johandavidcm, dantdmdarealguy) and the end of panicDa3m0n's 26 Oct comment were read only as a summary.
- **#14051 page:** inline review threads, per-job CI results and the "needs rebase" label as shown on the page could not be loaded. janzezno's comment was read only as a summary.
- **winpthreads:** that its rwlock uses a global spinlock.
- **Not run:**
  - a Windows build
  - a real GPU
  - any game measurement
  - the full test suite (only the focused draw tests above)

## 10. Reproduce

```bash
git remote add upstream https://github.com/boykopovar/AnyPS5.git
git remote set-url --push upstream DISABLED
git fetch upstream main pull/14051/head:refs/remotes/upstream/pr14051
git fetch https://github.com/johandavidcm/AnyPS5.git feat/agc-draw-prepare-ahead:refs/remotes/johandavidcm/feat/agc-draw-prepare-ahead
```

```bash
git submodule update --init --recursive --depth 1 --jobs 8
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

```bash
cmake --build build --parallel --target libSceAgcDriver agc_driver_draw_prepare_ahead_tests
```

```bash
ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ctest --test-dir build -R '^agc_driver_draw_prepare_ahead$' -V
```

On Debian or Ubuntu, configuring also needs `libxrandr-dev`, `libx11-dev`, `libxext-dev`, `libpulse-dev` and `libasound2-dev`. Running the tests without a GPU needs `mesa-vulkan-drivers` (lavapipe) and `vulkan-validationlayers`.
