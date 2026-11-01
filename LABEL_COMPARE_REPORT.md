# Label store GPU compare: working report

Branch `draft/label-compare` on `chitters2004/AnyPS5`, based on `boykopovar/AnyPS5` `main`
`dc9dc7eaaeaf99b1c491d426e68bb999def3da16`. This file is for local agents working on the branch.
It breaks the repository rule against report files (AGENTS.md, CONTRIBUTING.md;
`tools/check_conventions.py` reports it as `[notes-file]`), so drop every commit that touches it
before any pull request to upstream. The code commits pass the conventions check on their own.

Commits, oldest first:

1. `515a35953` feat(agc): check a GPU-compare rule for label stores against the flush's decision (stage 1, shadow check)
2. `efc46e616` feat(agc): decide a label store's blocks on the GPU instead of waiting for its writers (stage 2, watched imports)
3. `8dcb3789c` docs: first version of this report
4. `a24797fef` feat(agc): decide compared label stores on the GPU too (compared imports, needed on Windows)
5. `cc8593c56` docs: report update
6. `d3eadc01c` feat(agc): put a GPU-compared image back in sync when its batch completes
7. `08811bbd3` feat(agc): compare small block-unit label stores on the GPU and allow images up to 1 MiB
8. `43de569b4` docs: report update
9. `0beed21ce` feat(agc): read pending leases' written-page tables in the label store GPU compare
10. `fe1a7b9e9` docs: report update
11. `8a7ab13ae` feat(agc): split the label store fallback refusals by cause in shadow mode
12. this report update

Line references below are at commit 7 unless noted; search by function name if they moved. Paths are under `core/libs/prx/libSceAgcDriver/`.

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
  linear image (any size) or small (<= 64 KiB) image, with the default `PublishScope::Whole`.
- `FlushPending` waits when `Recorder::PendingWriterSerial` finds an unfinished batch that writes
  the range or holds it in an address-based lease (`Graphics/src/Recorder.cpp:3223`): `Sync` for
  the open batch, `FinishUpTo` otherwise. This is the wait.
- `writeBackLayers` decides per 64 KiB block (`Graphics/src/Texture.cpp:6479`):
  - Watched memory: `written = kept || (stamped && !unchangedSinceBaseline)`; an untracked unit
    (`layerGeneration == 0`) outside a drawn rectangle is stored whole.
  - Compared memory (not watched): `compareUntracked` runs the CPU flush hook
    (`GuestMemory::FlushGpuWrites`, which can sync writers again), then compares the guest bytes
    with the saved `original`; `written = kept || bytes differ`.
  Written blocks keep the guest bytes; the rest are retiled and copied into the import.
- Image kinds (`Texture.cpp:1508`): a tiled image at a 64 KiB-aligned base tracks one unit per
  64 KiB ("block units") and stores through `writeBackWindows`; above 64 KiB its pieces go through
  unit shadows. Linear, thick or unaligned images track one unit (or array layers) and store
  through the whole-layer path.

Why it waits: an address-based lease's stores reach the CPU write tracker only when its batch
completes, and both decisions read bytes the pending writers have not produced yet.

## Which imports are watched

`decideImportWatch` (`Graphics/src/GuestBufferMemory.cpp:1270`):

- Windows: direct host imports of private memory are compared by default; they stay watched only
  when the probe shows the watch survives import, settle reads, GPU reads and writes, and a CPU
  store is still seen. Without a settle queue they are always compared. Shared aliases keep the
  watch. Windows therefore needs the compared variant (commit 4).
- Linux: watched unless GPU writes resolve the userfaultfd write protection (lavapipe does), then
  compared. `APS5_WRITE_WATCH_IMPORTS=watch|unwatch` forces it.

Read the `[write-watch] host imports ...` line on the target machine.

## How address-based stores are tracked today

The recompiled shader instruments each BDA store (`core/shader/recompiler/SpirvBackend/src/SpirvBdaLookup.cpp:156-219`):
it hashes the 128 KiB chunk (`addr >> 17`) into a 2048-slot table in the dispatch's fault buffer
and sets the 4 KiB page bit with an atomic OR (layout `core/shader/recompiler/BdaAbi.hpp:56-70`).
At completion `BdaResources::markWrittenPages` (`Graphics/src/BdaResources.cpp:338-361`) reads the
table and calls `GuestMemory::MarkPossiblyWritten` per range, then clears it. The table is in GPU
memory as soon as the stores run; no code reads it on the GPU today. Overflow of the table is a
`Require` failure.

## Design decision

Upstream already narrows this wait by writer provenance in open pull requests: #8887 (writers over
the stored layers only), #12070 (the image's own stamped write-backs), #10382 (read-only leases),
#13920 (per-lane prefixes). What remains are true writers whose effect on the decision is unknown
until they run. The chosen design keeps the store on the GPU, in queue order behind the writers,
and moves only the decision there. Rejected: a completion-time CPU store (every GPU reader gate
would need a new deferred-writer kind); waiting less for "readers" (address-based reads are leases
that may store); page-protected lazy readback (upstream #9136).

## The rules (`Graphics/include/LabelCompare.hpp`)

Watched memory, per block, from its state at the wait (`kept` at unwatch, `unknown` unit,
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
explicit writer also changes, cannot attribute a change to the lease from bytes alone.

Since commit 9 that state reads the lease's own record instead (`OnGpuLease`): when every lease
over the image is in the open batch with a written-page table and the block is a whole 64 KiB
tracker block, the GPU keeps it iff a lease stored in its pages and its bytes changed, which is
`stampedAfter && changedAfter` exactly. Otherwise it still falls back (edge blocks, untabled
leases, leases in batches in flight).

Without a baseline `unchangedSinceBaseline` is always false, so the decision after the wait is the
stamp alone: a stamped block left to the GPU is kept on the CPU, and a lease-mode block has its
"changed" flag preset. Without the lease tables this case mostly could not run at all: the
fallback states are, in practice, images whose bytes were not settled when their baseline was
captured, so they have no baseline.

Compared memory (`ClassifyCompared`): kept at unwatch → keep; no writer or lease → keep iff
changed now; otherwise GPU against `original`. No stamps, so no fallback. Needs a complete
`original` and no stored save pending.

## Stage 1: shadow check

`APS5_LABEL_COMPARE_SHADOW=1` classifies the single image of a waiting label store before the
wait (`labelCompareClassify`, `Texture.cpp:4529`), lets the wait and writeBackLayers run, then
compares each block's prediction with the real decision and prints a `[label-compare]` line in the
storage profile (waits checked, eligible and refused with wait time, refusal reasons, blocks per
mode, mismatches). `any` checks every waiting flush and prints a line per check. Behaviour
unchanged.

## Stage 2: the GPU compare

Opt-in: `APS5_LABEL_GPU_COMPARE=1`.

- `FlushPending` (`Texture.cpp:4464`): for a label-reason flush of one image with an unfinished
  writer, classify; if every block is decidable, a GPU-compared block has a baseline, no unit
  shadow overlaps the range, and the pipeline exists, skip the wait and attach the modes.
  Otherwise wait as before and count the refusal.
- `writeBackLayers`: on compared memory it skips `compareUntracked` (`Texture.cpp:6438`); blocks
  follow the modes (`Texture.cpp:6481`): keep blocks are skipped, store and GPU blocks are stored
  ranges, GPU blocks are also conditional and mark the layer skipped.
- Whole-layer path: the import copy (`Texture.cpp:6708`) leaves the conditional ranges out.
  Small block-unit path: `writeBackWindows` (`Texture.cpp:3557`) takes the conditional pieces out
  of its scratch-to-import copies and passes them as sources.
- `recordConditionalStores` (`Texture.cpp:6286`) builds a host-visible staging buffer per store:
  the baseline (`original` or `generationBaseline`), and the source bytes (the retiled results;
  bytes no piece covers are the baseline, so a stored range rewrites them unchanged). It records a
  barrier from every earlier command and host write, then `ConditionalStore.comp` twice per range
  through buffer device addresses: pass 0 flags the range when any import word differs from the
  baseline; pass 1 copies the source words only if the flag is clear. A kept range is not written.
- Bookkeeping when recorded: as a partial skip (stored ranges stamped, layer stale, `original`
  dropped). At completion (`settleConditionalAtCompletion`, `Texture.cpp:6338`): if every GPU range
  was stored, no block was kept, and the image was neither written back (ticket) nor re-uploaded
  (generation) since, the layer takes the generation collected after its own stamps when recorded,
  as a full store's settle would; on compared memory the stored bytes copied into `savedBytes`
  become `original`. Otherwise it stays stale and the save is cancelled.
- Eligible: one tracked unit, at most 1 MiB, whole words, host import over the image; small block
  units included.
- Lease tables (commit 9): `ShaderResources::MarkGpuWrites` registers each lease's fault buffer
  with its read ranges (`Recorder::NoteLeaseTable`; a lease without fault checks marks the batch
  `untabledLease`). `Recorder::OpenLeaseTables` returns the tables over the image only when every
  lease there is in the open batch (a reap clears a table at completion, so an in-flight lease's
  table may be gone before the compare runs) and no table is shared with a batch in flight.
  `recordConditionalStores` copies each table (overflow word to the end of the list) into its
  staging buffer behind a barrier from every earlier command; pass 0 (invocation 0) looks up the
  block's chunk key and 16-page mask; an overflowed or invalid table counts as stored. Flags are
  three words per range: changed, lease stored, decision.

## Safety, condition by condition

| Condition | Argument | Evidence |
| --- | --- | --- |
| Identical keep or skip decisions | Rules above; modes are the decisions | `agc_label_compare`: watched rule 198 outcomes, 0 mismatches, six broken rules fail; compared rule 0 mismatches, three broken rules fail. Driver: linear and tiled cases in both watch modes keep the leased block and store the rest; shadow run on compared memory: 0 mismatches |
| CPU-write preservation | Kept and CPU-decided blocks are not written; a GPU-kept range is not touched | `agc_conditional_store`; per-word mutation fails; tiled control with pieces copied unconditionally fails |
| Ordering behind writers and alias stores | Recorded after the writers in the open batch, behind its own full barrier; queued label and key stores flushed first as today | `agc_conditional_store` before/after cases; driver tests with the writer in the open batch |
| Baseline lifetime and versioning | Baseline copied into staging when recorded, kept by the batch; no baseline means wait | code |
| Import lifetime | Same import, offset and lifetime as today's copy | code only |
| Conservative generation bookkeeping | Stale when recorded; back in sync at completion only if fully stored and untouched since | driver: baseline-bytes writer → in sync; changing writer → stale; never settling, and settling when kept, each fail in both modes. Not tested: the ticket and generation guards against a write-back or upload in between |
| Fallback intact | Every refusal waits; switch off by default | `--label-gpu-compare` fails with the switch off |
| Lease-table decisions | Table = the stamps the lease will make; only open-batch leases, so no reap clears a table first | `agc_label_compare` 810 outcomes, 0 mismatches; `agc_conditional_store` seven table cases; driver: fill, image and lease (inside or outside the image) run waiting and on the GPU give identical bytes with no wait; treating every lease as stored, or falling back, each fail |

Driver-level negative controls run and seen failing: compare off; GPU blocks stored
unconditionally; leases ignored; pending compared blocks stored; never settling; settling when a
range was kept; block units refused; tiled pieces copied unconditionally. Since commit 9 also: lease mode falling back; every lease counted as stored. One control did not
fail: keeping the `compareUntracked` call; its flush hook did not sync here, so skipping it is
defensive.

Still waiting (listed in `docs/dev/debt/libSceAgcDriver.md`): images over 1 MiB, block units of
images over 64 KiB, DCC keys, drawn targets, no host import, compared ranges without a complete
snapshot or with a stored save pending, GPU blocks without a baseline, unit shadows, pending
completion stores or labels, listed copied writers, watched fallback blocks, a missing pipeline.

## Tests run (Linux, lavapipe, LLVM 20.1.2, Vulkan validation layer 1.3.275)

`ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ctest -V -R`
`'agc_driver_(storage_flush_ordering|linear_readback_label|alias_copy_label|storage_write_back_overhang$|storage_write_back_no_window|storage_one_slice_alias|storage_read_during_idle_store|copied_writer_label_order)|agc_label_compare|agc_conditional_store'`:

- `main`: 10/10 passed.
- Head (commit 9): 15/15 passed, and 15/15 with `APS5_LABEL_GPU_COMPARE=1` forced on every test.
- No `was not found but was requested by env var VK_INSTANCE_LAYERS`; the layer loads. Only VUID:
  `VUID-VkDeviceCreateInfo-pNext-pNext` (old layer); per-test counts identical to `main`, plus
  one per new registration.

New registrations: `agc_label_compare`, `agc_conditional_store`,
`agc_driver_storage_flush_ordering_label_compare` (shadow `any`, fails on a mismatch),
`agc_driver_storage_flush_ordering_gpu_compare` (`APS5_WRITE_WATCH_IMPORTS=watch`),
`agc_driver_storage_flush_ordering_gpu_compare_compared` (`unwatch`). The `--label-gpu-compare`
mode runs a 128 KiB linear image (writer of other bytes, writer of the baseline bytes) and a
64 KiB S64KB tiled image (both writers).

## Test environment caveat: forced watch on lavapipe

`APS5_WRITE_WATCH_IMPORTS=watch` on lavapipe makes the driver tests use watched memory, but
lavapipe executes GPU writes as CPU stores, so a GPU write that runs while a flush waits leaves a
CPU-collected stamp (seen with a temporary diagnostic: `changed 1 cpu 1` on the filled block). That
violates the premise the import probe checks (watched imports need 0 pages "written after a GPU
write"), so on a correctly probed GPU it cannot happen. The lease-table test therefore lets its
GPU fill execute before the classification (submitted and waited, not reaped), so both paths see
the same stamp. Any new watched-mode test on lavapipe needs the same care.

The validation layer also caught a real bug during commit 9: `vkCmdCopyBuffer` from the fault
buffer without transfer-source usage (`VUID-vkCmdCopyBuffer-srcBuffer-00118`, while `ctest`
passed). Fixed in the same commit.

## The size limit

Why it exists: per GPU-compared store the label thread memcpys the baseline (and, on the
block-unit path, the source prefill) into a host-visible staging buffer of twice the compared
bytes, and the GPU reads both from host memory, over PCIe on a discrete GPU. A watched image also
needs a baseline, and `generationBaseline` is kept only up to 1 MiB (`Texture.cpp:2695`).
At 1 MiB the cost is a 2 MiB staging allocation (pooled), up to 2 MiB of CPU copies (about
0.1-0.3 ms) and the GPU reading 2 MiB from host memory, against waits measured in milliseconds.
Above that a watched image has no baseline, and the copies grow linearly. Lifting it further
needs a device-side baseline captured on the GPU when the image is uploaded or stored (one
image-sized VRAM copy per eligible image, plus a copy per upload) and the shader reading the
retile scratch in place (device-address usage on `tiledScratch`). Worth it only if the refusal
histogram shows `large` costing frame time.

## Would a GPU write tracker (improvement 5) replace this?

No, and it is not definitely better. The decision is `stamped and changed`; a GPU tracker gives
the stamp half on the GPU, but identical-byte rewrites still need the byte compare, so the
conditional store shader, the staging of results and baseline, the completion bookkeeping and the
block-unit plumbing all stay. What 5 replaces is the watched-memory classification: the lease
fallback states and the `leaseOver` reasoning. On compared memory (Windows by default) there are
no stamps at all, so 5 changes nothing there.

The useful part of 5 was cheap because the GPU already has the data: each address-based dispatch's
written-page table is in its fault buffer before completion. Commit 9 does this (see the rules
above); the fallback remains only for edge blocks and leases outside the open batch. A full GPU-resident generation table stamped by every
GPU writer (fills, copies, draws and dispatches through descriptors, labels) would also let other
flushes stop waiting, but it means instrumenting every write path, keeping two trackers coherent,
and it still falls back when a fault table overflows. Do the cheap part when the shadow
histogram shows `fallback-block` costing frame time; consider the full tracker only if non-label
flush waits dominate after this.

## Not verified

- Any title; no frame-time number. Hardware GPUs; Windows.
- Import lifetime at runtime; the completion guards against an intervening write-back or upload.
- Concurrent CPU stores during the window (same race class as today's decision after the wait),
  including the baseline-filled bytes a stored block-unit range rewrites.
- `captureGuestBytes` reads unsynced (`Texture.cpp:2628`): an image that becomes pending again
  without an upload while a GPU-compared store is in flight could capture pre-store bytes. Today's
  partial skip has the same exposure.
- DCC images: still refused; the key fill follows the texels as on a partial skip, but nothing
  on the label path was tested with keys.
- `tools/pr_overlap.py` not run; no pull request opened.

## What to measure on GTA

1. Note the `[write-watch] host imports ...` line.
2. Same machine and scene, `main` and head, `tools/trace_digest.py` summaries.
3. Head with `APS5_LABEL_COMPARE_SHADOW=1` and the storage profile: mismatches must be 0; eligible
   wait time bounds the saving; each refusal reason is printed as `count/ms waited`
   (`large`, `units`, `keys`, `no-baseline`, ...). Fallback blocks are split (commit 11):
   `fallback-edge`, `fallback-lease-in-flight`, `fallback-lease-untabled`,
   `fallback-lease-table-shared`, `fallback-lease-no-table`, and `fallback-edge+<lease reason>`
   when both apply. The in-flight and edge numbers decide the two extensions discussed below.
4. Head with `APS5_LABEL_GPU_COMPARE=1`: the `[label-compare] GPU compare` line gives waits
   skipped, blocks per mode, GPU ranges stored and kept, images in sync at completion and left
   stale, and why waits stayed; compare frame p50, the label wait and storage uploads per 10 s
   with step 2.
5. Watch label-polled data for visual or logic regressions.

## Next steps, ranked

1. Measure (above); the refusal histogram decides the rest.
2. If `fallback-block` still costs time, extend lease tables to in-flight leases (pin a table
   against its completion's clear until the compare's batch completes) and to edge blocks
   (verify `StoredOver`'s granularity against the 4 KiB page bits).
3. Block units above 64 KiB (gate the unit-shadow publish the same way) if `units` costs time.
4. Device-side baseline and in-place scratch reads if `large` costs time.
5. DCC images with a test if `keys` costs time.
6. Rebase on #8887, #12070, #10382 and #13920 once they merge.

## Rules for agents on this branch

Push only to `origin` (`chitters2004/AnyPS5`), branch `draft/label-compare`; `upstream`
(`boykopovar/AnyPS5`) is fetch-only. No pull requests, issues or posts. No code comments except
`#endif` and namespace ends. Conventional Commit subjects ending with the Co-Authored-By trailer.
