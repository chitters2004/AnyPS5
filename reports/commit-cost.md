# Committer per-draw cost: handover runbook (branch `draft/commit-cost`)

This file and `reports/` are here only so that local agents can continue the work without the original conversation. AGENTS.md, CONTRIBUTING.md and `tools/check_conventions.py` keep reports out of the repository (`[notes-file]` error). Drop every `docs:` commit that touches `reports/` before anything goes upstream.

## Start here

**Where things stand.** The branch is `upstream/main` `dc9dc7eaa` plus 13 `perf(agc)` commits and one `docs:` commit that adds an `ENVIRONMENT.md` row (`c42c773d8`, to squash into `eb2f5b50f`), all in section 6. The other `docs:` commits only keep `reports/`.
- **Validation:** all 1,156 `agc_` ctests pass on lavapipe with validation, with per-test VUID counts identical to main (section 3).
- **Waiting on the owner:** GTA runs (section 0), the calibration numbers (section 0.1) and ideally one profiled GTA log and a frame capture (section 0, step 5).
- **When the numbers arrive:** follow section 8.

**What is in `reports/`:**

| Path | What it is |
|---|---|
| `commit-cost.md` | This runbook |
| `ab_run.py` | Runs a title or a replay once per switch configuration and prints one comparison table (`--self-test` covers it) |
| `tools/run_all_agc.sh`, `tools/compare_runs.sh` | Full `agc_` suite with validation; results and per-test VUID counts; diff of two runs |
| `tools/mingw_objects.sh` | MinGW objects of libSceAgcDriver with the two header shims Ubuntu's MinGW needs |
| `bench/windows_audit.py` | Windows-expensive operations per draw (MinGW objects plus uprobe call counts) |
| `bench/calibration.cpp` | Per-machine costs of thread_local, locks, shared_ptr, clock, allocation, memcpy and Vulkan descriptor work |
| `bench/draw-bindings.cpp` | Per-draw CPU of the binding paths against the mock Vulkan device |
| `bench/snapshot.cpp`, `bench/descriptors.cpp` | Benchmarks of the snapshot copy cache and of vkUpdateDescriptorSets on lavapipe |
| `windows-audit-raw.md` | The audit's raw output (an upper bound; section 11) |

## 0. Owner run sheet (GTA, Windows)

One build of head covers every change: each draw-path change has an off-switch, so a single binary times them all. All commands run from the repository root.

**What it costs:** 6 configurations, 3 rounds, a timing pass and a profile pass, at 150 s per run plus a pause, is about 95 minutes unattended. The quick version below is about 20 minutes.

**Step 1. Build head** (`draft/commit-cost`) the way you normally build for GTA. Note the commit and your GPU, driver and OS.

**Step 2. Pick a repeatable scene.** Use the same save, place and camera every launch, and the same resolution and settings. Lock GPU clocks if you can (`nvidia-smi -lgc <clock>`). The script launches GTA once per run and stops it after `--seconds`. If GTA needs input after launch to reach the scene, the runs are not comparable; use step 5 instead.

**Step 3. Full run:**

```bash
python reports/ab_run.py run --out ab-gta --rounds 3 --seconds 150 --warmup 30 -- <the command that starts GTA>
```

**Quick run** (head, ring off, all switches off):

```bash
python reports/ab_run.py run --out ab-gta-quick --configs head,no-ring,all-off --rounds 2 --seconds 120 --warmup 30 -- <the command that starts GTA>
```

**Optional epoch-proof pass:** `APS5_DRAW_PIPELINE=1` makes Revalidate reuse proofs; the profile lines differ with it.

```bash
python reports/ab_run.py run --out ab-gta-pipeline --mode profile --pipeline --configs head --rounds 2 --seconds 120 --warmup 30 -- <the command that starts GTA>
```

**Step 4. Send back:**
- the markdown the script prints at the end (`python reports/ab_run.py report ab-gta` prints it again);
- the commit, GPU, driver and OS;
- for a speed claim in a PR, the `tools/trace_digest.py` summary of `ab-gta/timing/head/round-1/aps5-summary.jsonl` and the same on main.

**Reading the table:**
- A `no-X` row slower than `head` is what change X saves. `all-off` approximates main for the switchable changes; commit `331932b6f` and the once-per-batch keep (`4ae6f4bc8`) have no switch.
- The profile table gives the lookup split, the Revalidate split and the ring share that choose the next step (section 8).

**Step 5. If launches are not repeatable,** capture once and replay instead:
- Set `APS5_FRAME_CAPTURE=<dir>` and play to the scene. It records 10 frames at the next flip by default; `APS5_FRAME_CAPTURE_TRIGGER` lets you pick the moment (see `docs/dev/BUILD.md`, Frame capture and replay).
- Give the script `build/core/libs/prx/libSceAgcDriver/agc_frame_replay.exe <dir>` as the command, with `build/core/libs/libs/unpatched` prepended to `PATH`. A replay exits on its own, so `--seconds` only bounds it.
- Unverified: whether a replay writes `aps5-summary.jsonl` intervals as a title does. If the timing table is empty, use the replay's own JSON line in each run's `stdout.log`.
- A capture also lets agents reproduce GTA frames on Linux. It holds the game's memory, so share it privately only.

Switch reference:

| Configuration | Switches set | Turns off |
|---|---|---|
| `head` | none | nothing |
| `no-ring` | `APS5_NO_DYNAMIC_DRAW_DATA=1` | `eb2f5b50f` and its follow-ups `f8eedb639` and `4ae6f4bc8` |
| `no-set-reuse` | `APS5_NO_DRAW_SET_REUSE=1` | `30ef0bf7a` |
| `no-chunk` | `APS5_DRAW_UPLOAD_CHUNK_MIB=1` | `d2b0314fb` |
| `no-pending-gate` | `APS5_NO_PENDING_WRITE_GATE=1` | `78647781b` |
| `all-off` | all four | all of the above |

### 0.1 Also send: calibration numbers from the GTA machine (one minute)

Build the benchmark with the same MSYS2 toolchain as the driver, from the repository root, then run it:

```bash
g++ -std=c++20 -O2 -Icore/libs -Icore -I3rdparty/Vulkan-Headers/include reports/bench/calibration.cpp -o calibration.exe -static
```

```bash
./calibration.exe
```

Send the printed table. `CALIBRATION_GPU=<index>` picks another device from the list it prints. It measures, on that machine:
- `thread_local` (emutls), `ThreadOwned`, `HostThreadLocal`, `AgcDriver::Mutex`, `std::mutex`, `std::shared_mutex`, a `shared_ptr` copy, `steady_clock::now`, `new`/`delete` and `memcpy`;
- `vkUpdateDescriptorSets` by descriptor type, a whole set against buffers only, set allocation, a bind with dynamic offsets, and creating an exact-size buffer.

These turn the audit's counts (section 10) and the descriptor savings into microseconds on the owner's hardware.

## 1. Goal and rules

- **Goal:** GTA V (PPSA04263) at PS5 Pro frame rate or better, with no fidelity loss. Fixes must be general; never per-title logic.
- **Bottleneck:** on Windows the committer thread bounds the frame: about 100 of 130 ms, about 30 us per draw, about 3,000 draws a frame, GPU 32-44 ms.
- **Measurement:** agents cannot run GTA. The owner times drafts.
- **Push rule:** `origin` is the fork `chitters2004/AnyPS5`. Commit to `draft/commit-cost` and push to `origin`. `upstream` is `boykopovar/AnyPS5`, fetch only, and work is based on `upstream/main`. Never push upstream, open a PR or issue, or post anywhere.
- **Code rules:**
  - No code comments except `#endif` and namespace ends.
  - Commit subjects are Conventional Commits, with the `Co-Authored-By` trailer the session asks for.
  - Each independent gain gets its own commit so it can be timed alone.
  - Each change has a test that fails without it.
  - Avoid recompiler source edits; they invalidate the shader cache for every comparison.
  - On Windows, `std::shared_mutex` and `thread_local` are slow on hot paths. Use `AgcDriver::Mutex` and the `ShaderRecompiler::ThreadOwned` slots.
- **Not to duplicate:** parallel commit belongs to upstream #7895 and #14051; do not start a rival. Upstream #9508 (GPU descriptor heap) would supersede descriptor sets entirely; check its state before more descriptor-set work.

## 2. Environment setup (Linux, lavapipe)

```bash
git remote add upstream https://github.com/boykopovar/AnyPS5.git
```

```bash
git fetch upstream main
```

```bash
git submodule update --init --recursive --depth 1 --jobs 8
```

```bash
sudo apt-get install -y mesa-vulkan-drivers vulkan-validationlayers libvulkan1 libpipewire-0.3-dev libpulse-dev libasound2-dev libxrandr-dev libwayland-dev libxkbcommon-dev libegl-dev libdecor-0-dev ccache nasm
```

Configure only the execution tests that matter; there are 564, and building all of them takes hours on 4 cores:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "-DANYPS5_AGC_EXECUTION_TESTS=F16ToF32Unpack;DrawCacheReuse;DrawDataHitUserWords;LinearTargetDrawBindings;DrawWriteRanges;PartiallyResidentSnapshot;DispatchMoveDepth"
```

```bash
ninja -C build -j4 agc_driver_dispatch_move_depth_tests agc_driver_draw_cache_reuse_tests agc_driver_draw_data_hit_user_words_tests agc_driver_draw_write_ranges_tests agc_driver_linear_target_draw_bindings_tests agc_driver_partially_resident_snapshot_tests agc_draw_inputs_in_place_driver_tests agc_driver_recorder_tests agc_driver_graphics_tests agc_snapshot_copy_failure_tests
```

The library alone takes about 8 minutes on 4 cores, and the test executables about 1 minute more.

## 3. Validation procedure (run on main and on the change)

```bash
ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --test-dir build -R '^(agc_driver_dispatch_move_depth|agc_driver_draw_cache_reuse.*|agc_driver_draw_data_hit_user_words.*|agc_driver_draw_write_ranges.*|agc_driver_linear_target_draw_bindings|agc_driver_partially_resident.*|agc_draw_inputs_in_place_driver|agc_driver_recorder|agc_draw_descriptors|agc_driver_graphics|agc_snapshot_copy_failure|agc_descriptor_cache|agc_revalidate_split)$' -V -j2 --timeout 1500 > head.log 2>&1
```

```bash
grep -oE 'VUID-[A-Za-z0-9_-]+' head.log | sort | uniq -c
```

Then:
- Confirm that the log has no `was not found but was requested by env var VK_INSTANCE_LAYERS`.
- Run the same commands on `upstream/main` and diff the VUID counts. `ctest` can report Passed while validation errors are logged.
- The baseline on main `dc9dc7eaa` (23 tests) is 72 `VUID-VkDeviceCreateInfo-pNext-pNext`, 216 `VUID-vkCmdCopyImage-srcImage-01548` and 216 `VUID-vkCmdCopyImage-srcImage-01551`. Every head so far matches it exactly.

**Full suite.** Configure with `-DANYPS5_AGC_EXECUTION_TESTS=` (empty builds all 564 execution tests; about 9 minutes on 4 cores, and each binary is about 75 KB), then run every `agc_` test:

```bash
ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --test-dir build -R '^agc_' -E fuzz -V -j4 --timeout 900 > all.log 2>&1
```

It takes about 20 minutes. Compare results and VUID counts per test name, not per test number: head registers extra tests, which shifts the numbering.

Results on main `dc9dc7eaa`: 1,151 tests, 1,074 passed, 75 skipped (lavapipe features), 2 failed. The two failures:
- `agc_driver_guest_memory_lifetime_converted` needs `build/core/relinker/relinker`, which this configuration does not build.
- `agc_driver_image_table` has a failure regex that rejects any VUID, and it gets the baseline `VUID-VkDeviceCreateInfo-pNext-pNext`.

Heads `89c31547d` and `78647781b` (with the pre-pass gate) give identical results and identical per-test VUID counts, plus `agc_revalidate_split`, which passes. Watch for a false positive: a grep for "was not found but was requested" also matches ctest echoing a test's failure regex, so check the matching line.

**Windows check.** With `g++-mingw-w64-x86-64-posix`, compile the changed sources with `-fsyntax-only`, using the Linux build's commands (`ninja -C build -t commands libSceAgcDriver`) with the compiler swapped and `-fPIC` and the dependency flags removed. Do the same for main from a `git worktree`, then diff. Ubuntu's MinGW lacks `PROCESSOR_RELATIONSHIP::EfficiencyClass` and `_fflush_nolock` (both errors also appear on main); nothing else differs.

Extra cross-check: `APS5_VERIFY_SET_LAYOUTS=1` makes the pipelined draw path compare `DescribeSetLayouts` with the built layouts; it passes on head.

Before pushing:

```bash
python3 tools/check_conventions.py --base upstream/main
```

Expect exactly one error, the `[notes-file]` error for this file.

Scripted version of the full-suite comparison:

```bash
reports/tools/run_all_agc.sh main /tmp/runs
```

Run that with main checked out and built, then the same with `head` as the label on the branch; then:

```bash
reports/tools/compare_runs.sh /tmp/runs main head
```

Test-fixture notes:
- Recorder releases kept objects on a release thread. Tests call `recorder.Sync()` and `recorder.FlushDeferredReleases()`, then poll; see `settle` in `writtenSetReuseTests`.
- Mock contexts have zero limits by default; set the samplers and dynamic storage buffer limits a test needs.

## 4. Profile inputs from the owner (Windows, GTA)

- Lookup was 34.8 of 68 us per draw when instrumented: DrawResourceKey, ResourceCache::Find, Revalidate, MovedReadOnlyBuffers, CheckBufferAliases. Record 9.9, vertex 7.6, readTarget 6.0.
- Template (ResourceCache) hits: 116,253 of 117,874 draws. Only 15% of draws find a register-level draw cache entry (99,837 have none; 7,995 inserts per 10 s).
- Committer busy shares: ShaderResources constructor 23.8% (buildComplete 19%), UploadFinish 17.9%, copyRegion 13.8% (mostly memcpy), Revalidate 9.6% (all revalidateFull), CopyDrawInput 9.2%, Collect 6.6%, Recorder::Submit 6.4%.
- Samples: prepareDraw 7,863 of 11,746 busy, resolveDrawResources 3,965, cachedTexture 1,604, EqualsCommitted 1,228.

## 5. Code map: what each path does for correctness

Paths are in `core/libs/prx/libSceAgcDriver/`.

- **resolveDrawResources** (`Graphics/src/Draw.cpp`): DrawResourceKey (per-program memoized content keys; with movable buffers the data words and the target and index ranges stay out of the key), then `SharedResourceCache().Find`, then `ShaderResources::Revalidate`, then `MovedReadOnlyBuffers`, then `CheckBufferAliases`. A hit reuses a build's descriptor set only after Revalidate proves that every texture or storage image is still the cache's object for its memory.
- **Revalidate** (`Graphics/src/ShaderResources.cpp`):
  - Under `EpochProofs()` (that is `DrawPipelineEnabled()`, `APS5_DRAW_PIPELINE`), an earlier proof is reused while the global CollectEpoch, ProofSerial and PendingSerial all stay unchanged.
  - Otherwise `revalidateFull` runs:
    - the pre-pass: CompletePendingCpuWrites and FlushDepthPlanes over directRegions, describedRanges and storageTextures;
    - the image proof: `fastRevalidate`, then the T1 own-object refresh, then `fullWalk`, which repeats the cachedTexture and cachedStorageTexture lookups;
    - the import checks: ScanPending and FlushPending, then the HostImportSerial loop;
    - staging copies and depth copies.
- **Register draw cache** (`Execution/src/Driver/Draw/Draw.cpp` and `DrawRegisterKey.cpp`): the key hashes user-data SGPR words, which change nearly every draw, hence the 15% entry rate. Data-only hits (by shape key) cover part of it.
- **PrepareDrawBindings** (`ShaderResources.cpp`): builds per-draw descriptor bindings for a template hit whose buffers moved. Data words are uploaded to `Recorder::DrawUpload`. Read-only guest buffers read in place get per-draw snapshots, to protect against later CPU writes.
- **copyRegion and snapshotCopy** (`Graphics/src/GuestBufferMemory.cpp`): a snapshot-backed region uploads a snapshot taken before the device lock. The snapshot copy cache (`Graphics/include/SnapshotCopies.hpp`) shares one buffer between identical snapshots.
- **CopyDrawInput** (`Draw.cpp`): copies vertex and index bytes into a pooled Buffer that is bound exactly to size (BindExactBuffer). Exact size matters for robust vertex fetch bounds, so it was left alone.
- **ShaderData**: `RuntimeAbi::ShaderData` (`core/shader/recompiler/RuntimeAbi.hpp`) is the 16,064-byte maximum layout. A compiled program's data is compact: `IrBindingLayout::ShaderDataDwords()` counts the user-data registers it reads, the optional dispatch limit, 12 dwords per runtime image and its runtime state, and `program.shaderDataDwords` holds the result. Small data with no runtime images goes into push constants instead (`BindingAllocator.cpp`, `pushDataStartDword`). The worker builds it per draw in `ShaderDataDwordsFor` (`Optimization/src/DescriptorBindingBuilder.cpp`). Correction: the commit messages of `f8eedb639` and `d2b0314fb` describe 16 to 32 KiB per draw. That is the maximum, not the usual case; fix the wording when squashing for upstream.

## 6. Commits on the branch (base `dc9dc7eaa`)

Each code commit was timed alone and validated as in section 3.

| Commit | Change | Test that fails without it | Expected saving / evidence |
|---|---|---|---|
| `331932b6f` | A snapshot cache entry takes the region's vector instead of copying it | `agc_snapshot_copy_failure` (CheckMovedInsertion) | One memcpy and one allocation per snapshot cache miss; part of copyRegion's 13.8%. Bench: 4 KiB saves 172 ns, 1 MiB saves 145 us |
| `30ef0bf7a` | A finished draw returns its sets to its template (up to 16); reuse rewrites only buffer descriptors | `agc_driver_graphics` writtenSetReuseTests | Bench on lavapipe: 16 samplers save 25 ns, 64 save 84 ns per update; real drivers unknown |
| `eb2f5b50f` | ShaderData and FlattenedSrt become dynamic storage buffers for graphics builds; a draw changing only data words binds the template's ring set for the upload chunk with dynamic offsets | `agc_driver_graphics` dynamicDrawDataTests (fails with `APS5_NO_DYNAMIC_DRAW_DATA=1`) | Removes set allocation and vkUpdateDescriptorSets for those draws |
| `c42c773d8` | Adds the `ENVIRONMENT.md` row for `APS5_NO_DYNAMIC_DRAW_DATA` | the conventions check | Squash into `eb2f5b50f` before upstream |
| `f8eedb639` | Copy only the changed data bindings into the ring; unchanged ones stay on the template's buffer at offset 0; up to 4 ring sets per template keyed by (chunk, mask) | dynamicDrawDataTests partial case | Fixes `eb2f5b50f` copying every 16 KiB binding on every draw |
| `d2b0314fb` | `Recorder::DrawUploadChunk` goes from 1 MiB to 16 MiB | dynamicDrawDataTests: 200 full draws need at most one new ring set | Fewer chunk allocations and ring set writes; costs one live 16 MiB chunk per recorder |
| `4ae6f4bc8` | Fixed-size offsets; the batch keeps the ring set and chunk once (`Recorder::KeptOnce`) | dynamicDrawDataTests use_count check | Removes per-draw keeps and two per-draw allocations |
| `4df6d3109` | Profile-only `[revalidate-split]` and `[lookup-split]` lines (APS5_PROFILE_DRAW) | `agc_revalidate_split` (new ctest; does not compile without the change) | Measurement for the next step; no behaviour change |
| `78647781b` | The Revalidate pre-pass checks `AnyPendingCpuWrites` once and walks its ranges for pending CPU writes only when some are pending (re-checked after a depth-plane flush that did work) | `agc_revalidate_split`: a revalidation with nothing pending makes no checks (fails when the old always-check is forced); with copied writers pending it still checks | Per range and proof: drops a `DrawCopiedWriters()` shared_ptr copy, the per-thread `InCompletion` read (emutls on Windows) and an atomic load. The `[revalidate-split]` line prints checks per call |
| `46dc94fb0` | Profile-only `[draw-bindings]` line: ring draws and bytes copied (with their size spread), ring sets found, written and evicted live, draws per template and batch, rewritten draws with data bytes and guest snapshots, set pool hits and misses, templates drawing | the profiled `agc_revalidate_split` checks the counts on a real device | Says whether the chunk size, the 4 ring sets and the 16 kept sets per template fit GTA |
| `c5a6421b4` | `DrawPipeline::Busy()` reads the finisher's global state before the thread_local `finisherProducer` | none can fail: the result is unchanged; the draw-pipeline execution tests cover every state, and MinGW disassembly shows the idle path returns before `__emutls_get_address` | About 21 emutls calls per draw on Windows |
| `8bc6e456b` | `PerformanceContext::Current()` returns null without reading thread_local while no context carries a frame | `performanceContextTests` in `agc_driver_graphics` pins the semantics (nesting, frameless contexts, other threads); MinGW disassembly shows the constructor skips emutls when the count is zero | At least 22 emutls calls per draw on Windows |
| `fcac8f0d9` | `flushBetweenPackets` keeps its reap timestamp in a `HostThreadLocal` slot, fetched only where the reap is decided | none can fail: per-thread behaviour is unchanged; the object has no emutls call left | About 19 emutls calls per draw become one FLS lookup on the reap path |
| `434d12353` | Switches for one-build A/B: `APS5_NO_DRAW_SET_REUSE`, `APS5_DRAW_UPLOAD_CHUNK_MIB` (default 16) and `APS5_NO_PENDING_WRITE_GATE` | `agc_draw_bindings_no_set_reuse`, `agc_draw_bindings_small_chunk` and `agc_revalidate_split_ungated` check that each switch restores the old behaviour | Lets the owner time each change in one build with `reports/ab_run.py` (section 0) |

Section 0 lists the A/B switches; `reports/ab_run.py` runs them. Standalone benchmarks are in `reports/bench/` (build commands in section 9).

**Per-draw CPU benchmark** (`reports/bench/draw-bindings.cpp`, standalone; mock Vulkan with descriptor updates stubbed, so the Vulkan driver's own cost is excluded). It measures MovedReadOnlyBuffers plus PrepareDrawBindings for a template whose ShaderData and SRT both change, in us per draw, ring on versus off:

| Shader data | Small template | Template with 16 samplers |
|---|---|---|
| 32 dwords | 0.97 vs 2.65 | 1.08 vs 2.82 |
| 256 dwords | 2.24 vs 3.29 | 1.78 vs 2.74 |
| 4,016 dwords (the ABI maximum) | 4.22 vs 4.29 | 3.80 vs 4.38 |

At realistic sizes the ring saves about 1 to 1.7 us per draw before the driver's descriptor writes, which it also avoids. perf at the maximum size shows memmove at 37%. The kernel page-fault time in that profile is a mock artefact: the mock maps each new chunk fresh.

## 7. What the owner should measure on GTA

Take `tools/trace_digest.py` summaries on main and on each code commit above, on the same machine and scene, with `APS5_PROFILE_DRAW=1`, and collect these lines:

- **`[draw-bindings] (10 s): ...`.** Constants bytes per ring draw and their size spread, ring set hits, writes and live evictions, draws per template and batch, the set pool hit rate and templates drawing.
- **`[rescache] draws: ... draw bindings (cumulative): R ring with dynamic offsets (S ring sets written), W rewritten sets`.** The share of draws that take the ring. If W dominates, the next step in this area is putting small guest-buffer snapshots in the same ring.
- **`[lookup-split] (10 s): N lookups, H hits; us per lookup: key, find, revalidate, moved buffers, alias checks`.** Says where the 34.8 us lookup goes.
- **`[revalidate-split] (cumulative): ... epoch proofs reused, ... proved (no epoch, first proof; moved: collect epoch, proof serial, pending serial); per call: pre-pass us over N ranges, images us (textures, storage images), imports us, rest us`.** Says why proofs are not reused and which phase of `revalidateFull` costs.
- **The existing `[rescache] revalidate (10 s)` line.** Fast, full and failed counts, fast-fail reasons, full-walk ms by reason.

Run once with and once without `APS5_DRAW_PIPELINE=1`: epoch proofs exist only with it.

## 8. Next steps, ranked

1. **Revalidate.** Read `[lookup-split]` and `[revalidate-split]` from GTA first, then pick by the numbers:
   - **The pre-pass still dominates** after `78647781b` (check the pending-write checks per call): the remaining per-range work is FlushDepthPlanes' `overlapsStalePlane` and the multisample-target Require. Gate those per template on a depth-plane generation, like the epoch gate on imports.
   - **Images dominate with fast-fail reasons, or full walks:** fix the fast-fail reasons the existing revalidate line names. The other option is a per-template proof keyed on the write generations of its own ranges, instead of the global epoch, proof and pending serials.
   - **Epoch proofs are rarely reused because a global serial moves:** scope the reuse test to the template's ranges.
2. **The ring path's coverage.** If `rewritten sets` dominate, make small read-only guest snapshots (at most `DrawUploadLimit`, 4 KiB, already taken from `DrawUpload`) dynamic bindings in the same ring.
3. **The ShaderData copy.** Program data is compact (see section 5), so this is unlikely to matter. Only if a profile shows the per-draw copy in PrepareDrawBindings or ringDrawBindings costing would the fix be moving userData and bufferOffsets out of ShaderData (push constants or a small binding). That is a recompiler change: it invalidates every shader cache (a one-time recompile for all players, and A/B timing needs separate warm caches) and touches every title's shaders. It needs the owner's decision.
4. **The ShaderResources constructor (23.8%).** With 98.6% draw template hits this is probably dispatch builds; split draw and dispatch in the profile before acting.
5. **CopyDrawInput (9.2%).** Exact-size buffers per input; any change must keep robust vertex fetch bounds (vkCmdBindVertexBuffers2 sizes are an option).
6. **Before upstream:** squash `c42c773d8` into `eb2f5b50f`, drop the `docs:` commits that touch `reports/`, run `tools/pr_overlap.py`, and run the section 3 validation on real NVIDIA and AMD GPUs. The dynamic offsets depend on `maxDescriptorSetStorageBuffersDynamic` and `minStorageBufferOffsetAlignment`.

## 9. Standalone benchmarks (`reports/bench/`)

`draw-bindings.cpp` includes `tests/Graphics.cpp` for its mock Vulkan device, so it builds and links like `agc_driver_graphics_tests`, with Graphics.cpp's object swapped:
- Take the compile command of `tests/Graphics.cpp` from `ninja -C build -t commands agc_driver_graphics_tests`.
- In it, compile `reports/bench/draw-bindings.cpp` instead, adding `-Icore/libs/prx/libSceAgcDriver`.
- Link with that target's link command (its last line), with `tests/Graphics.cpp.o` replaced by the new object.
- Run `./draw-bindings small|many only|both <dwords>`. Prefix it with `APS5_NO_DYNAMIC_DRAW_DATA=1` for the rewrite path.


```bash
g++ -std=c++20 -O2 -Icore/libs -I3rdparty/Vulkan-Headers/include -Icore/libs/prx/libSceAgcDriver -Icore reports/bench/snapshot.cpp -o snapshot-bench
```

```bash
g++ -std=c++20 -O2 -I3rdparty/Vulkan-Headers/include reports/bench/descriptors.cpp -o descriptors-bench -ldl
```

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./descriptors-bench
```

Results on 4-core x86-64 lavapipe:

- **`snapshot`** (copy versus moved insert):

  | Size | Saving per insert |
  |---|---|
  | 0 B | 19 ns |
  | 256 B | 40 ns |
  | 4 KiB | 172 ns |
  | 64 KiB | even (noise) |
  | 1 MiB | 145 us |

- **`descriptors`** (vkUpdateDescriptorSets with 3 buffers):

  | Other descriptors | Full rewrite | Buffers only |
  |---|---|---|
  | 0 | 15.3 ns | 15.0 ns |
  | 16 samplers | 39.8 ns | 15.3 ns |
  | 64 samplers | 98.8 ns | 15.1 ns |

## 10. Not verified

- The three thread_local fixes have no test that fails without them, since the behaviour is unchanged by design; the evidence is the MinGW disassembly and the semantics tests.
- GTA, Windows, and real GPU drivers.
- Descriptor update cost on real drivers.
- On lavapipe the ring path was observed in `agc_driver_recorder` (7 draws) and `agc_draw_descriptors` (1 draw), using a temporary probe that was not committed. The selected execution tests' draws use non-dynamic templates.
- The first `[revalidate-split]` sample on lavapipe (`agc_revalidate_split`) showed no collect epoch and a pre-pass of about 0.8 ms over one range. A toy case, not representative.

## 11. Windows hot-path audit (static sites times calls per draw)

**Method** (`reports/bench/windows_audit.py`):
- Compile every libSceAgcDriver source with MinGW GCC 13 at -O3. 6 shim sources with Windows SEH assembler stubs and the Linux-only libc and libkernel files fail; none is on the draw path.
- Count per function the calls to `__emutls_get_address` (thread_local), winpthreads rwlock and mutex calls, FLS/TLS slot calls and lock-prefixed atomics.
- Count calls per draw of the same functions (same mangled names) with uprobes on the Linux build, over the draw execution tests (64 draws).

The raw table is `reports/windows-audit-raw.md`. Its totals are an upper bound: they count call sites, and most sites sit in profiling, tracing or failure branches. The top entries were checked by hand.

**Real on the default path:**

| Site | Per draw | Cost |
|---|---|---|
| `PerformanceTimer` constructor (`Execution/include/PerformanceTimer.hpp`) | at least 22 (more where inlined) | `PerformanceContext::Current()`, a thread_local read |
| `DrawPipeline::Busy()` (`DrawPipeline.cpp`) | about 21 | reads thread_local `finisherProducer` whenever the pipeline is idle |
| `Driver::flushBetweenPackets` (`PacketFlush.cpp`) | about 19, once per packet | thread_local `lastReapTry`, plus `deferredLabels()` through `HostThreadLocal` (FLS) |
| `std::_Sp_counted_base::_M_release` | 36 | shared_ptr releases, each one or two atomic RMW |
| `ShaderRecompiler` `materializeResult` and `getSource` | about 0.5 each | std::shared_mutex (winpthreads rwlock) sites; whether these run on the committer is not established |

**Fixed** (`c5a6421b4`, `8bc6e456b` and `fcac8f0d9`; section 6). None of the three has an off-switch: time them as head against `434d12353` or main. Remaining candidates:
- **shared_ptr releases (36 per draw):** find the owners released per draw before changing anything.
- **The recompiler's rwlock sites:** first establish which thread runs `materializeResult` and `getSource`.

Under Wine, emutls costs 18.9 ns against 2.2 ns natively, so about 62 thread_local reads per draw would be about 1.2 us of a 30 us draw. The owner's calibration run (section 0.1) gives the real Windows cost.

**Not on GTA's recorded path:** `vkGetDeviceProcAddr` runs about 14 times per draw in the tests, all from `CommandBatch` and `RenderTarget`, which the synchronous test readbacks use; recorded draws open batches through `Recorder`. The `[vk]` profile line counts GTA's real lookups.

**Upper bound from the raw table:** 1,220 emutls, 2,717 mutex, 7.6 rwlock and 6,770 atomic sites times calls per draw, mostly branches that do not run (for example `PerformanceTimer::Finish` returns before its frame and trace paths when neither is active).

