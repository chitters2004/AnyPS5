# Stage overhead draft: report for the local session

This file is a handoff note for the local session that measures and validates this draft. It breaks the AGENTS.md rule against report files in the repo: drop the commit that adds it (`git rebase --onto HEAD~1 HEAD` style, or drop it in a rebase) before any PR. Its content belongs in the PR body.

## Branch

- `draft/stage-overhead` on `chitters2004/AnyPS5`, based on upstream `boykopovar/AnyPS5` main `0d5e03be8` (fetched read-only; `gh repo sync` was refused by the cloud proxy with 403).
- Commit `79ee45a6e2` from the brief is in neither repo. Paths moved: `Graphics/src/DrawCapture.cpp` is now `core/libs/prx/libSceAgcDriver/Execution/src/Driver/Draw/DrawCapture.cpp`; `InvocationFor` is in `Execution/src/Driver/Shaders/ShaderRegistry.cpp`; `materializePreparedMemoized` is in `core/shader/recompiler/Recompiler.cpp`.

## Part 1: commits

| Commit | What it does |
|---|---|
| `fbb44692d` test(agc) | New CPU-only test `tests/execution/StageRecapture.cpp` (+ `tests/cmake/agc_driver_stage_recapture_tests.cmake`). Drives a fragment stage through `InvocationFor` → `CaptureInvocation` → `Materialize` as `materializeDrawStage` does. Checks the invocation refers to the prepared code and the draw's own state, that a new interface prepares another artifact, and that every memo hit or patched result equals a build without the memo (same user data and push offset only). Registered three ways: default, `APS5_VERIFY_MASKED_RESULT_MEMO=1`, `APS5_NO_MASKED_RESULT_MEMO=1`. `--bench` prints timings; not registered with CTest. |
| `3ee41810c` perf(shader) | `CacheKey.hpp`: key words are staged in a 64-word buffer (`KeyWriter`) and appended in blocks, instead of one out-of-line `vector::emplace_back` per word (top symbol in the perf profile; the key build was ~70% of InvocationFor). Key words and order unchanged; `CheckInterfaceKey` pins the layout across two flush boundaries. |
| `fa344e699` perf(agc) | `InvocationFor` copies the request once instead of three times, restores the source code before preparing at use, and moves the matched request into the invocation via a new `PreparedShaderInvocation::TryCreate(RecompileRequest&&, ...)` that only consumes it on a static-ABI match. |
| `4260236c6` perf(shader) | `BufferSubstituteTable` holds its slots as an immutable `shared_ptr<const vector>`; `Slots()` returns the pointer, `Learn()` swaps in an updated copy. Removes a vector copy under a mutex per materialization. |
| `f4ec74eaf` perf(shader) | `snapshotHash` split into `snapshotCommonHash` (images, samplers, uniform fill, tables, partial threads, push offset, pixel and vertex state) plus a per-memo continuation over buffers, flattened SRT and user data. A miss hashes the shared part once instead of twice. Inputs of each hash unchanged. |

## Related upstream PRs

- #9899 (callee-link reuse): only compute and GCN vertex stages take `LinkUserDataCallees`, so no functional overlap with graphics stages. It edits the same lines of `InvocationFor`; expect a trivial textual conflict.
- #10757 (reuse an evicted result's storage): owns the donor copy at the brief's line 1876 (`make_shared<RecompileResult>(*donor)`) and eviction cost. Not touched here. Victim scan (1901–1915) is 4 ways; not worth changing, the real eviction cost is freeing the victim, which #10757 recycles.
- #14266 (digest SPIR-V features once): found in the overlap search. I had a content-verified thread-local digest memo; it was dropped as a duplicate. #14266 changes `appendFeatureDigest`'s signature, which conflicts textually with `KeyWriter`; resolution is mechanical (push its two words through `KeyWriter`).
- #5609 (relocate cached draw stages when only buffer bases move): same idea family as Part 2.
- #6223 (image-table element reuse): orthogonal (ShaderResources).
- #14852: already merged in main.
- #13517 (disk-cache key pair hash): no overlap.

## Benchmark (CPU only, for ranking; never a PR claim)

Method: `build/tests/agc_driver_stage_recapture_tests --bench`, Release, GCC 13.3, Intel Xeon @ 2.10 GHz VM (4 vCPU), `taskset -c 2`, `ANYPS5_NO_SHADER_CACHE=1`. Fixture: 5-word fragment shader, one user word, no buffers/images; target with 24 capabilities and 12 extensions. Each measure: 4000-iteration warm-up, then 31 batches × 4000 iterations; a run reports median/p10/p90/min/max of batch means. Six interleaved before/after runs (before = `fbb44692d`, after = `f4ec74eaf`).

Important: the test binary loads `libSceAgcDriver.prx` from `build/core/libs/libs/unpatched` at run time (RUNPATH). For an A/B, copy that directory per version and run each binary with `LD_LIBRARY_PATH` pointing at its own copy; otherwise both binaries use whichever library was built last.

Median of six run medians (range of run medians; median p10/p90):

| Measure | Before | After | Change |
|---|---|---|---|
| InvocationFor | 740 ns (731–815; 714/765) | 545 ns (528–556; 530/572) | −26% |
| BuildPreparedShaderKey | 518 (512–529; 504/549) | 362 (354–417; 353/389) | −30% |
| RecompileRequest copy | 30 | 30 | — |
| TryCreate | 84 | 87 | noise |
| Stage, memo hit | 1209 (1200–1251; 1184/1273) | 1002 (977–1302; 968/1098) | −17% |
| Stage, new user data | 1477 (1451–1495; 1425/1605) | 1206 (1178–1778; 1169/1263) | −18% |
| Materialize, memo hit | 55 | 56 | — |

Remaining cost in this fixture: capture ≈ 440 ns; the feature digest ≈ 150–200 ns of the key (the dropped memo prototype measured InvocationFor ≈ 355 ns, key ≈ 150 ns), which #14266 removes for device-profile targets. The substitute and hash-split commits are not exercised by this fixture (no buffers/images), so their effect is unmeasured.

## Tests and mutation check

- 41 CPU tests pass (`ctest -R "^agc_shader_|^agc_driver_stage_recapture|^agc_driver_prepared_(shaders|graphics|unregistered)"`); 29 Vulkan tests skip (return 77) but build. Each commit was built and tested on its own.
- Mutations, all killed by the new test: key writer keeps staged words after a flush; interface key loses the final flush; learning a substitute changes earlier slots; common hash drops the push offset; masked hash ignores the common part (killed in the default and verify modes; survives with the masked memo off, as expected); full hash drops user data words; invocation prepared at use keeps the source code; rvalue `TryCreate` skips the static-ABI match.
- `python3 tools/check_conventions.py --base <upstream main>` passed (before this file was added).

## Not run or not verified (local session to do)

- AGENTS.md GPU runs: the same focused tests on main and this branch with `ANYPS5_REQUIRE_VULKAN=1`, `ANYPS5_NO_SHADER_CACHE=1`, `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`; confirm the layer loads and no new `VUID-` errors. Suggested: `ctest --test-dir build -R "agc_driver_prepared_|agc_driver_stage_recapture|agc_shader_" -V`.
- GTA V: no capture, no `tools/trace_digest.py` summaries, no frame-time numbers.
- Vertex, mesh and tessellation stages not benchmarked (ValidateRequestInputs cost on the vertex path unmeasured).
- `tools/pr_overlap.py --branch draft/stage-overhead` could not run (no GitHub API access to upstream from the cloud session); manual search only.
- Full test suite not built (~1900 tests); no Windows/MinGW build; no ASan/TSan; no comparison against an on-disk shader cache written by main.

## Part 2: guarded replay recipe for descriptor-only changes (design only)

### Where it fits

`validateVariant` (`Execution/src/Driver/Dispatch/VariantValidation.cpp`) already allows `dataPositions` (words that only feed the flattened SRT) to differ and returns `EqualData`; `patchDataWords` then patches them. The recipe extends this to descriptor words. A recipe hit rebuilds the `ResourceSnapshot` by patching the stored words read at the recorded addresses, skipping the SRT walk and `ShaderMemory` capture, and reuses a cached invocation. It then goes through the normal memoized `Materialize`: buffer words are patched by the masked memo, but images are hashed in full by the masked hash, so a new T# still pays a full `materializeResult`.

### Data a recipe keeps

- Identity: shader snapshot pointer, code offset, prepared `SourceHandle`, variant ID, interface-key hash, push offset and layout, `convertedSamples`, linked callee and user-data-tail code (addresses and words, as #9899 keeps them), and the substitute-slots pointer (comparable thanks to commit `4260236c6`).
- Read set: the variant's runs and words, and `userDataMask`.
- A role per word:
  - Pointer: its value is the base of a later read.
  - Selector: an index, count, branch value or table bound.
  - Descriptor: a leaf V#/T#/S# word, with a mask of the bits the variant specializes on.
  - Data: existing `dataPositions`.
  - Fixed: everything else (must be equal).
- Mapping: each Descriptor word's snapshot slot (buffer/image/sampler index and dword) and its binding word in the compiled result; capture outputs to reproduce (`regions`, `supplied`, `userDataUse`).

### Invalidations (each forces a full recapture)

- Shader or callee code: snapshot replaced, raw-range check fails, callee or tail words differ.
- Specialization: variant ID; interface-key inputs (pixel/vertex state, float mode, wave size, target); buffer word1/word3, misalignment, dword count and available-dword mask (`BufferSpecializationFields`); a whole image descriptor; a newly learned substitute (slots pointer changed).
- Static ABI: descriptor set, first binding, push offset/size, push-data use.
- A user word that changes read selection: any `asBase` word, or any word not proven value-only.
- Addresses, counts, table bounds: any Pointer or Selector word; table base, size, records, keys, fault, outside; table ranges; uniform-fill word count.
- Pending GPU writes: same `syncPendingRuns` and draw-pipeline drain as `validateVariant`; otherwise Differing.
- Label overlaps: `QueuedLabel`.
- Storage overlaps: pending or shadowed storage textures (`FlushingImage`).
- Generation moves: `PublishGeneration`, `PendingSerial`, `ForgetSerial` before and after the read; odd forget serial; the variant's generation.
- Unmapped reads: `Inaccessible`.
- Stamp validation mode: take legacy, vouched and shared-word paths exactly as `validateVariant` does; under `APS5_VERIFY_DRAW_DATA_HITS` always shadow-compare.

### Riskiest failure: old addresses after a pointer changes

An SRT pointer moves from A to B, the table at B has the same layout, variant and bindings still match, and the recipe re-reads stale words at A. Ruled out by:

1. Roles come from the capture's read trace (leaves vs other reads, plus `asBase`), not from values, at field granularity, so a V# base that is dereferenced is a Pointer.
2. Default deny: anything not proven Descriptor or Data is Fixed.
3. Equal Pointer and Selector words imply equal runs: the walk is deterministic given code plus guarded words, and a pointer read from memory is itself a word in a run, so it is guarded too.
4. Shadow compare in verify mode: run the full recapture beside the recipe and compare with `verifyStageData` (`Draw.cpp:892`), which already compares runs, words, variant, push constants and bindings and so catches this case. Extend it to regions and supplied blocks, `userDataUse`, poisoned SRT reads, specialization constants, runtime image resources and image-table ranges.
5. A test: two same-layout tables at A and B, swap the pointer, the recipe must refuse; mutation (classify the pointer as Descriptor) must fail the test.

### Measure on a standing GTA capture before building it

- Eligible fraction: of the ~4,100 recaptures per frame, how many had a draw-cache entry at the same push offset that came back `Differing` (not Layout, Absent or a gate outcome), differ only at Descriptor or Data words, and recompile to the same variant ID. The existing `differingAddress` / `differingData` / `differingWalk` / `differingMixed` counters give a first cut. The key unknown: whether per-draw ring allocations move the table pointers, which would make most stages ineligible.
- Vertex-path share: the candidate search at `DrawLookup.cpp:242` runs only for `ShaderPath::Vertex`. If most recaptured stages are on Geometry, Tessellation or Mesh paths, the recipe must hook the draw-key lookup instead.
- Split of `Shader.DrawStage` by its timing marks (`prepared_invocation`, `capture_resources`, `materialize`) via `trace_digest`: bounds the gain, since a recipe saves the capture walk but not image-driven materialize misses.
- Result-memo hit / patched / miss rates from `APS5_PROFILE_DRAW`.
