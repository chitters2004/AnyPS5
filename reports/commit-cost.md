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

## Commits on the branch

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

## Ring and dynamic-offset constants: design (in progress)

Goal: a template hit whose data words changed binds one long-lived set with dynamic offsets into the recorder's DrawUpload chunk, instead of allocating and writing a set per draw.

- **Scope.** Graphics builds only (drawBuild), not BDA, not update-after-bind (a dynamic type is invalid in an UPDATE_AFTER_BIND_POOL layout), and dynamic bindings per set within maxDescriptorSetStorageBuffersDynamic. A switch disables it. Compute keeps STORAGE_BUFFER and RefreshData.
- **Layout.** ShaderData and FlattenedSrt bindings (DataRole, count 1) become VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC. The layout key carries the type, so pipelines and layouts separate by themselves. Places that assume STORAGE_BUFFER:
  - the image count in buildComplete;
  - the write switch;
  - the non-split pool sizes;
  - DescriptorCache chain pool sizes (they need a dynamic entry);
  - the descriptor limit switch (maxDescriptorSetStorageBuffersDynamic);
  - the update-after-bind flags;
  - commit 2's buffers-only filter.
- **Bind sites.** ShaderResources::Bind (zero offsets for the template's own set), Draw.cpp recordDraw (the DrawBindings offsets) and recordPreparedDraw (via Bind). VulkanDevice compute binds stay non-dynamic. Dynamic offsets are ordered by binding number within each set.
- **Per draw.** In dynamic mode, MovedReadOnlyBuffers reports every data binding with its words (no compare). PrepareDrawBindings takes the ring path when only data words are selected:
  - reserve one DrawUpload slice for all data bindings, with sub-offsets aligned to minStorageBufferOffsetAlignment, and copy the words with dataPatches applied;
  - take the template's ring set for that chunk, or write one (from WrittenSets with buffer writes only, else AllocateRewritten with every write);
  - return a small per-draw DrawBindings that shares the ring set (no recycling of its own), carries the dynamic offsets, and keeps the slice for capture.
  When guest buffers are selected, the existing rewrite path runs and binds with zero offsets.
- **Lifetime.** The ring set is a DrawBindings held by the template and by each draw that uses it (the recorder keeps the draw). When the chunk changes, the template drops it, and it returns to WrittenSets once its batches finish. It holds the chunk only weakly for identity.
- **Expected saving.** Most template hits stop allocating a set, stop issuing vkUpdateDescriptorSets and stop taking DescriptorCache's mutex. The left-over per-draw cost is one small memcpy, one make_shared and the bind.

## What the owner should measure on GTA

- tools/trace_digest.py on main, on each commit and on head, same machine and scene.
- With APS5_PROFILE_DRAW: the [buffers] snapshot lines and recordDraw samples, plus committer time in vkUpdateDescriptorSets, AllocateRewritten and copyRegion memcpy.
