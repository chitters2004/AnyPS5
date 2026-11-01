# Label store GPU compare: working report

Branch `draft/label-compare` on `chitters2004/AnyPS5`, based on `boykopovar/AnyPS5` `main`
`dc9dc7eaaeaf99b1c491d426e68bb999def3da16`. This file is for local agents working on the branch.
It breaks the repository rule against report files (AGENTS.md, CONTRIBUTING.md;
`tools/check_conventions.py` reports it as `[notes-file]`), so drop every commit that touches it
before any pull request to upstream. The code commits pass the conventions check on their own.

Commits, oldest first:

1. `515a35953` feat(agc): check a GPU-compare rule for label stores against the flush's decision (stage 1, shadow check)
2. `efc46e616` feat(agc): decide a label store's blocks on the GPU instead of waiting for its writers (stage 2, write-watched imports)
3. `8dcb3789c` docs: first version of this report
4. `a24797fef` feat(agc): decide compared label stores on the GPU too (stage 2, compared imports, needed on Windows)
5. this report update

Line references below are at commit 4. Paths are under `core/libs/prx/libSceAgcDriver/`.

## Goal and starting point

GTA V spends about 20 ms of a 130 ms frame on label handling. A sizing run with the label store
off (`APS5_NO_LABEL_READBACK_STORE=1`, unsafe) went from 131.7 to 111.5 ms p50; the label wait
alone is about 11 ms. An earlier own-note exemption cut waits by 90% but not wait time (8.8%), so
the remaining waits are few and long.

## The path, end to end

- `VulkanDevice::WriteLabelOnGpu` calls `StorageTexture::StoreReadbackBeforeLabel("readback before label")`
  (`Execution/src/VulkanDevice.cpp:2575`; `Execution/src/Driver/Packets/PacketMemory.cpp:57` does
  the same with "readback before interrupt").
- `StoreReadbackBeforeLabel` calls `FlushPending(base, bytes, nullptr, reason)` for every pending
  linear or small (<= 64 KiB) image, with the default `PublishScope::Whole`.
- `FlushPending` waits when `Recorder::PendingWriterSerial` finds an unfinished batch that writes
  the range or holds it in an address-based lease (`Graphics/src/Recorder.cpp:3223`): `Sync` for
  the open batch, `FinishUpTo` otherwise. This is the wait.
- `writeBackLayers` decides per 64 KiB block (`Graphics/src/Texture.cpp:6413`). Whether the range
  is write-watched comes from `GuestMemory::ChangedBlocks` (`Texture.cpp:6370`):
  - Watched: `written = kept || (stamped && !unchangedSinceBaseline)`; an untracked unit
    (`layerGeneration == 0`) outside a drawn rectangle is stored whole.
  - Compared (not watched): `compareUntracked` runs the CPU flush hook (`GuestMemory::FlushGpuWrites`,
    which can sync writers again), then compares the guest bytes with the saved `original`;
    `written = kept || bytes differ`.
  Written blocks keep the guest bytes; the rest are retiled on the GPU and copied into the import.

Why it waits: an address-based lease stamps the pages it stored only when its batch completes
(commit 9fffceaa9), and both decisions read bytes the pending writers have not produced yet.

## Which imports are watched

`decideImportWatch` (`Graphics/src/GuestBufferMemory.cpp:1270`):

- Windows: direct host imports of private memory are compared by default; they stay watched only
  when the probe shows the watch survives import, settle reads, GPU reads and writes, and a CPU
  store is still seen. Without a settle queue they are always compared. Shared aliases keep the
  watch. So Windows needs the compared variant (commit 4).
- Linux: watched unless GPU writes resolve the userfaultfd write protection (lavapipe does: its
  GPU writes are CPU stores), then compared. `APS5_WRITE_WATCH_IMPORTS=watch|unwatch` forces it.

Read the `[write-watch] host imports ...` line on the target machine to know which path runs.

## Design decision

Upstream already narrows this wait by writer provenance in open pull requests: #8887 (writers over
the stored layers only), #12070 (the image's own stamped write-backs), #10382 (read-only leases),
#13920 (per-lane prefixes). What remains are true writers whose effect on the decision is unknown
until they run, so provenance cannot remove the comparison. The chosen design keeps the store on
the GPU, in queue order behind the writers, and moves only the decision there. Rejected: a
completion-time CPU store (every GPU reader gate would need a new deferred-writer kind, as copied
writers did); waiting less for "readers" (address-based reads are leases that may store);
page-protected lazy readback (upstream #9136).

## The rules (`Graphics/include/LabelCompare.hpp`)

Watched ranges, per block, from its state at the wait (`kept` at unwatch, `unknown` unit,
`stampedNow`, `changedNow` against the baseline, explicit pending `writerOver`, pending `leaseOver`):

| State | Mode |
| --- | --- |
| unknown unit | store (whole unit) |
| kept at unwatch | keep |
| no writer, no lease | keep if stamped and changed, else store |
| lease, unstamped, and (changed or explicit writer) | fall back to the wait |
| stamped, or lease | GPU: keep iff bytes differ from the baseline at that queue point |
| explicit writer only, unstamped | store |

Explicit writers stamp when recorded, so their stamps are in `stampedNow`; a lease's change
implies its completion stamp; an unstamped block under a lease that already differs, or that an
explicit writer also changes, cannot attribute a change to the lease, so it falls back.

Compared ranges (`ClassifyCompared`): kept at unwatch → keep; no writer or lease → keep iff
changed now; otherwise GPU against `original`. No stamps are involved, so there is no fallback.
The image needs a complete `original` and no stored save pending (`awaitStoredSave` would sync).

Bug found during stage 2: stage 1 classified an untracked unit as keep; writeBackLayers stores it
whole. Fixed in commit 2.

## Stage 1: shadow check (commit 1)

`APS5_LABEL_COMPARE_SHADOW=1` classifies the single image of a waiting label store before the
wait (`labelCompareClassify`, `Texture.cpp:4511`), lets the wait and writeBackLayers run, then
compares each block's prediction (the post-wait byte compare standing in for the GPU) with the
real decision, and prints a `[label-compare]` line in the storage profile: waits checked,
eligible and refused with their wait time, refusal reasons, blocks per mode, mismatches. `any`
checks every waiting flush and prints a line per check. Behaviour unchanged.

## Stage 2: the GPU compare (commits 2 and 4)

Opt-in: `APS5_LABEL_GPU_COMPARE=1`.

- `FlushPending` (`Texture.cpp:4446`): for a label-reason flush of exactly one image with an
  unfinished writer, classify; if every block is decidable, no unit shadow overlaps the flush
  range, and the pipeline exists (`Recorder::ConditionalStoreReady`), skip the wait and attach the
  modes to the write-back. Otherwise wait as before and count the refusal.
- `writeBackLayers`: on compared memory it skips `compareUntracked` (`Texture.cpp:6372`); blocks
  follow the classified modes (`Texture.cpp:6415`): keep blocks are skipped, store and GPU blocks
  are stored ranges, GPU blocks are also conditional and mark the layer skipped. `Require`s pin the
  path: whole-layer import path, one import segment, no block units, no drawn rectangle, no stored
  save.
- The import copy (`Texture.cpp:6638`) leaves the conditional ranges out; `recordConditionalStores`
  (`Texture.cpp:6266`) copies the retiled bytes and the baseline into a host-visible staging buffer
  the batch keeps, then `Recorder::RecordConditionalStore` runs `Graphics/shaders/ConditionalStore.comp`
  twice per range through buffer device addresses: pass 0 flags the range when any import word
  differs from the baseline; after a compute barrier, pass 1 copies the result words only if the
  flag is clear. A kept range is never written. A completion action counts stored and kept ranges.
- Bookkeeping: identical to today's partial skip. Stored ranges are stamped; the layer keeps its
  old generation (stale) and re-uploads at its next use; `original` is dropped; no stored save.
  The pending-write note is today's, so CPU readers still sync on the batch.
- Images up to `LabelCompareMaxBytes` (256 KiB); base and size whole words.

## Safety, condition by condition

| Condition | Argument | Evidence |
| --- | --- | --- |
| Identical keep or skip decisions | Rules above; the modes are the decisions | `agc_label_compare`: watched rule 198 outcomes, 0 mismatches, six broken rules fail; compared rule 0 mismatches, three broken rules fail. Driver: `..._gpu_compare` (watched) and `..._gpu_compare_compared` keep the leased block and store the other; shadow run on compared memory: 2 blocks, 0 mismatches |
| CPU-write preservation | Kept and CPU-decided blocks are not written; a GPU-kept range is not touched | `agc_conditional_store` "a word changed ... keeps its whole range"; per-word mutation fails |
| Ordering behind writers and alias stores | Recorded after the writers in the open batch; the import-ready barrier (ALL_COMMANDS and HOST to COMPUTE) precedes the compare; queued label and key stores flushed first as today | `agc_conditional_store` before/after cases; driver test with the writer in the open batch |
| Baseline lifetime and versioning | Baseline copied into staging at record time, kept by the batch | code |
| Import lifetime | Same import, offset and lifetime as today's copy | code only |
| Conservative generation bookkeeping | Same state as a partial skip | code only |
| Fallback intact | Every refusal waits; switch off by default | `--label-gpu-compare` fails with the switch off |

Driver-level negative controls, each run and seen failing: compare off; GPU blocks stored
unconditionally; leases ignored by the watched classifier; pending compared blocks stored.
One control did not fail: keeping the `compareUntracked` call in stage 2. Its flush hook did not
sync the open batch here, so skipping it is defensive, not shown necessary.

Still waiting (listed in `docs/dev/debt/libSceAgcDriver.md`): images over 256 KiB, block units,
DCC keys, drawn targets, no host import, compared ranges without a complete snapshot or with a
stored save pending, unit shadows over the image or the range, pending completion stores or
labels, listed copied writers, watched fallback blocks, a missing pipeline.

## Tests run (Linux, lavapipe, LLVM 20.1.2, Vulkan validation layer 1.3.275)

`ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ctest -V -R`
`'agc_driver_(storage_flush_ordering|linear_readback_label|alias_copy_label|storage_write_back_overhang$|storage_write_back_no_window|storage_one_slice_alias|storage_read_during_idle_store|copied_writer_label_order)|agc_label_compare|agc_conditional_store'`:

- `main`: 10/10 passed.
- Head: 15/15 passed, and 15/15 with `APS5_LABEL_GPU_COMPARE=1` forced on every test.
- No `was not found but was requested by env var VK_INSTANCE_LAYERS`; layer loads
  (`VK_LOADER_DEBUG=layer`). Only VUID: `VUID-VkDeviceCreateInfo-pNext-pNext` (old layer);
  per-test counts identical to `main`, plus one per new registration.

New registrations: `agc_label_compare`, `agc_conditional_store`,
`agc_driver_storage_flush_ordering_label_compare` (shadow `any`, fails on a mismatch),
`agc_driver_storage_flush_ordering_gpu_compare` (`APS5_WRITE_WATCH_IMPORTS=watch`),
`agc_driver_storage_flush_ordering_gpu_compare_compared` (`APS5_WRITE_WATCH_IMPORTS=unwatch`).

## Not verified

- Any title; no frame-time number. Hardware GPUs; Windows.
- Generation bookkeeping and import lifetime at runtime.
- Concurrent CPU stores during the window (same race class as today's decision after the wait).
- `captureGuestBytes` reads unsynced (`Texture.cpp:2628`): an image that becomes pending again
  without an upload while a GPU-compared store is in flight could capture pre-store bytes as its
  baseline. Today's partial skip has the same exposure; not made worse by design, not tested.
- `tools/pr_overlap.py` not run; no pull request opened.

## What to measure on GTA

1. Note the `[write-watch] host imports ...` line.
2. Same machine and scene, `main` and head, `tools/trace_digest.py` summaries.
3. Head with `APS5_LABEL_COMPARE_SHADOW=1` and the storage profile: mismatches must be 0; eligible
   wait time bounds the saving; refusal reasons rank the next work.
4. Head with `APS5_LABEL_GPU_COMPARE=1`: the `[label-compare] GPU compare` line (waits skipped,
   blocks per mode, GPU ranges stored and kept, reasons waits stayed); frame p50 and the label wait
   against step 2; storage uploads per 10 s (the re-uploads below).
5. Watch label-polled data for visual or logic regressions.

## Improvements and a better method

Ranked by expected value. The refusal histogram from step 3 decides the order on GTA.

1. Refine bookkeeping at completion. Every GPU-compared block leaves its unit stale, so the next
   use re-uploads the image even when the GPU stored it. For a label store every frame that is an
   upload every frame. Fix: take the collected generation when recording; at completion, if every
   GPU range was stored, the image is unchanged since (a ticket like `savedTicket`), and no stamp
   newer than that generation hit the unit, set the layer to that generation. On compared memory,
   commit the stored save as a full store does. Later stamps stay newer, so it remains safe.
2. Lift the size limit. The label path stores every pending linear image, whatever its size.
   Large ones refuse now because staging copies the results and the baseline through host-visible
   memory. Give `tiledScratch` device-address usage and keep a device-side baseline, captured on
   the GPU when the image is uploaded or stored. Then the shader reads both in place and no
   CPU memcpy happens at label time.
3. Cover block units and DCC images. Block units store through `writeBackWindows` and unit
   shadows. The same two-pass shader can gate the window copies per unit. DCC images store whole
   and need the key fill gated by the same flags.
4. Combine with upstream #8887, #12070, #10382 and #13920. They remove waits before
   classification and shrink the refused set; rebase once they merge.
5. Better method: a GPU-side write tracker. The fallbacks and most of the remaining complexity
   exist because leases report their stores only at completion. If GPU writers stamped a
   per-64 KiB-block generation table on the GPU, in queue order, then the whole keep or skip
   decision could run on the GPU with the same formula as the CPU tracker. That covers leases,
   explicit writers and CPU stamps (uploaded at record time), with no fallback. It would also let
   flushes other than the label path stop waiting: refresh, copy and fill flushes run the same
   decision. Address-based dispatches would write the table from their stores (how they find
   their stored pages for completion marking today was not checked), and the CPU tracker would read it back at completion
   instead of marking pages. This is the larger, general version of the current design; build it
   if step 3 shows that non-label flushes or lease fallbacks still cost frame time.
6. Avoid the label-time store entirely when nobody reads the image before the next GPU use. The
   eager store exists for CPU readers polling after the label. A per-image history of whether the
   CPU ever read the range after a label could defer it. Riskier, because a first read then pays
   a wait. Only worth trying if steps 1 to 3 leave the stores themselves expensive.

## Rules for agents on this branch

Push only to `origin` (`chitters2004/AnyPS5`), branch `draft/label-compare`; `upstream`
(`boykopovar/AnyPS5`) is fetch-only. No pull requests, issues or posts. No code comments except
`#endif` and namespace ends. Conventional Commit subjects ending with the Co-Authored-By trailer.
