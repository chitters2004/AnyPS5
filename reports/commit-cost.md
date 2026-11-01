# Committer per-draw cost: draft report (branch draft/commit-cost)

Drop this file and the commit that adds it before anything goes upstream: AGENTS.md and CONTRIBUTING.md keep reports out of the repository. It is here only so that local agents can read the findings.

Base: upstream/main `dc9dc7eaaeaf99b1c491d426e68bb999def3da16`.

## Profile inputs (owner's Windows GTA V PPSA04263 numbers)

- Committer bounds the frame: about 100 of 130 ms, about 30 us per draw, about 3,000 draws a frame.
- Lookup is 34.8 of 68 us per draw: DrawResourceKey, ResourceCache::Find, Revalidate, MovedReadOnlyBuffers, CheckBufferAliases.
- Template (ResourceCache) hits: 116,253 of 117,874 draws. Register-level draw cache entries are found for only 15% of draws.
- Busy shares: ShaderResources constructor 23.8% (buildComplete 19%), UploadFinish 17.9%, copyRegion 13.8% (mostly memcpy), Revalidate 9.6% (all revalidateFull), CopyDrawInput 9.2%.

## What each path does for correctness

- **DrawResourceKey and ResourceCache::Find** (Graphics/src/Draw.cpp). Each stage's content key is memoized per program. Data words are left out of the key when movable buffers are on, and so are the target and index ranges (trim). A hit reuses a build's descriptor set only after Revalidate proves that every texture or storage image is still the cache's object for its memory. MovedReadOnlyBuffers lists the data words and read-only guest buffers that differ from the build. CheckBufferAliases repeats the build's alias checks against this draw's target and index buffer.
- **Why few draws find a draw-cache entry.** The register key (Execution/src/Driver/Draw/DrawRegisterKey.cpp) hashes the user-data SGPR words, and those change nearly every draw. Data-only hits (by shape key) cover part of this. The ResourceCache leaves data words out of its key, hence the 98.6% hit rate.
- **PrepareDrawBindings** (ShaderResources.cpp). On a hit with moved buffers, it uploads the new words into the recorder's DrawUpload chunk (1 MiB). It then allocates a recycled set, rewrites every descriptor of the template into it, and binds that set for the draw. This runs on most draws.
- **copyRegion / snapshotCopy** (GuestBufferMemory.cpp). A snapshot-backed region uploads a snapshot taken before the device lock. The snapshot copy cache shares one buffer between identical snapshots (by address, size and usage, then memcmp).
- **CopyDrawInput.** Vertex and index bytes are copied into a pooled Buffer unless a reusable recorder snapshot exists. The Buffer is created exactly to size (BindExactBuffer) because robust vertex fetch bounds use the buffer size.

## Commits on the branch (drop every docs: commit before going upstream)

1. `331932b6f` perf(agc): move a snapshot into the copy cache instead of copying it. On a cache miss, the entry takes the region's vector instead of copying it, which removes one memcpy and one allocation per miss. Test: agc_snapshot_copy_failure (CheckMovedInsertion; fails without the change). Standalone benchmark (insert only): 0 B saves 19 ns, 4 KiB saves 172 ns, 1 MiB saves 145 us; 64 KiB was even.
2. `30ef0bf7a` perf(agc): reuse a template's draw sets and rewrite only their buffers. A finished draw returns its sets to the build that wrote them (up to 16 per build). The next draw of that build rewrites only its storage-buffer descriptors. Test: agc_driver_graphics writtenSetReuseTests (fails without the change). Standalone benchmark on lavapipe, vkUpdateDescriptorSets with 3 buffers: 0 other descriptors saves 0 ns, 16 samplers save 25 ns, 64 save 84 ns.

Validation: 23 focused tests on lavapipe with ANYPS5_REQUIRE_VULKAN=1, ANYPS5_NO_SHADER_CACHE=1 and VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation pass on main and head. The VUID sets are identical: 72 VkDeviceCreateInfo-pNext-pNext, 216 vkCmdCopyImage-srcImage-01548 and 216 vkCmdCopyImage-srcImage-01551, all already on main. The tests were the draw descriptors, recorder, draw cache reuse/data hits/write ranges, linear target bindings, partially resident snapshot, dispatch move depth, in-place inputs, graphics and snapshot copy tests. check_conventions passes.

Not verified: GTA, Windows, descriptor write cost on real drivers, and whether the lavapipe GPU tests hit the set-reuse path (the mock test does).

## Not changed, ranked by the profile

1. Revalidate (9.6%, always revalidateFull): needs the write-watch epoch rules understood first.
2. ShaderResources constructor (23.8%): with 98.6% template hits this is probably dispatch builds. Measure the split first.
3. Per-draw constants through a ring and dynamic offsets: in progress, see below. Upstream #9508 (a GPU descriptor heap) would supersede it.
4. CopyDrawInput's exactly sized buffers: exact size matters for robust vertex fetch, so it stays.
5. DrawResourceKey vector build and hash: under 1 us of 34.8, dropped.

## Commit 3: ring and dynamic-offset constants (`eb2f5b50f`)

perf(agc): bind draw shader data through dynamic offsets into the upload ring.

- **Layout.** Graphics builds without update-after-bind give their ShaderData and FlattenedSrt bindings VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC. This applies only when the dynamic bindings fit maxDescriptorSetStorageBuffersDynamic and 64. buildPrepare and DescribeSetLayouts share one rule (DynamicDataLayout), so the pipelined and journal paths still get identical layouts; APS5_VERIFY_SET_LAYOUTS=1 passes on lavapipe. Dispatches keep STORAGE_BUFFER. The chain pools hold 2048 dynamic descriptors, and SetDescriptorLimit knows the type.
- **Per draw.** In dynamic mode, MovedReadOnlyBuffers reports every data binding with its words, with no compare. When nothing else needs a snapshot, PrepareDrawBindings reserves one DrawUpload slice for all data bindings, with sub-offsets aligned to minStorageBufferOffsetAlignment, and copies the words with dataPatches applied. It then returns a small DrawBindings that shares the template's ring set for that upload chunk and carries the dynamic offsets. A ring set is written once per (template, chunk) and never updated while in use. When the chunk changes, it goes back to WrittenSets once its batches finish.
- **Other paths.** A draw that also snapshots a read-only guest buffer keeps the rewritten-set path and binds with zero offsets. ShaderResources::Bind binds the template's own set with zero offsets.
- **Switches and counters.** APS5_NO_DYNAMIC_DRAW_DATA=1 restores the previous layouts. The [rescache] draw line (APS5_PROFILE_DRAW) now prints the cumulative counts of ring draws, ring sets written and rewritten sets.
- **Tests.** agc_driver_graphics dynamicDrawDataTests is a mock test, and it fails with APS5_NO_DYNAMIC_DRAW_DATA=1. It covers layout types, agreement with DescribeSetLayouts, zero offsets on Bind, compute staying non-dynamic, one ring set write per chunk, no writes on the second draw, aligned and distinct offsets, and earlier draws' bytes left intact. The Recorder.cpp capture hook now reads dynamic writes as buffers.
- **Validation.** On lavapipe all 23 focused tests pass with the validation layer, and the VUID set is identical to main. A temporary, uncommitted probe confirmed that real-device draws took the ring path: 7 in agc_driver_recorder and 1 in agc_draw_descriptors. The execution tests' draws used non-dynamic templates.
- **Limit.** The ring path applies only when no read-only guest buffer is snapshotted for the draw. The profile line's ring/rewritten split on GTA decides whether the next step below is needed.

## Further options, ranked

1. **Revalidate (lookup is 34.8 of 68 us; 9.6% busy, always revalidateFull).** This is the largest remaining item. Epoch proofs are reused only while the global collect epoch, proof serial and pending serial all stay unchanged, and a busy frame moves them constantly. A per-template proof keyed on the write generations of the template's own ranges would make the proof O(ranges) compares instead of repeating cachedTexture lookups. First collect the [rescache] revalidate FastFail reasons on GTA.
2. **Guest buffer snapshots into the same ring.** Small read-only guest snapshots (up to DrawUploadLimit, 4 KiB) already come from DrawUpload. Making those bindings dynamic as well would let draws with snapshots take the ring path. Do this only if the new counter shows that rewritten sets dominate.
3. **ShaderResources constructor (23.8%) with 98.6% draw template hits.** This is probably dispatch builds. Measure the draw/dispatch split before acting.
4. **Register draw cache (15% entries).** The key includes user-data words. Check the [draw-cache] data-only hit count; widening data hits would cut decode work, not the committer's lookup.
5. **Descriptor heap / descriptor buffer (upstream #9508).** It supersedes sets entirely and would subsume commits 2 and 3 if it lands.
6. **Push descriptors.** Not better: they rewrite every descriptor per draw (what commit 2 removes) and cap at maxPushDescriptors.

## What the owner should measure on GTA

- tools/trace_digest.py on main, on each commit and on head, same machine and scene.
- With APS5_PROFILE_DRAW: the [buffers] snapshot lines and recordDraw samples, plus committer time in vkUpdateDescriptorSets, AllocateRewritten and copyRegion memcpy.
