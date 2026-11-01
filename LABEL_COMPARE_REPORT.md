# Label store GPU compare: working report

Branch `draft/label-compare` on `chitters2004/AnyPS5`, based on `boykopovar/AnyPS5` `main`
`dc9dc7eaaeaf99b1c491d426e68bb999def3da16`. This file is for local agents working on the branch.
It breaks the repository rule against report files (AGENTS.md, CONTRIBUTING.md), so drop this commit
before any pull request to upstream.

Commits, oldest first:

1. `515a35953` feat(agc): check a GPU-compare rule for label stores against the flush's decision (stage 1)
2. `efc46e616` feat(agc): decide a label store's blocks on the GPU instead of waiting for its writers (stage 2)
3. this report

## Goal and starting point

GTA V spends about 20 ms of a 130 ms frame on label handling. A sizing run with the label store
off (`APS5_NO_LABEL_READBACK_STORE=1`, unsafe) went from 131.7 to 111.5 ms p50; the label wait
alone is about 11 ms. An earlier own-note exemption cut waits by 90% but not wait time (8.8%), so
the remaining waits are few and long.

## The path, end to end

- `VulkanDevice::WriteLabelOnGpu` calls `StorageTexture::StoreReadbackBeforeLabel("readback before label")`
  (`Execution/src/VulkanDevice.cpp:2575`; `Execution/src/Driver/Packets/PacketMemory.cpp:57` does
  the same with "readback before interrupt").
- `StoreReadbackBeforeLabel` (`Graphics/src/Texture.cpp:4926`) calls `FlushPending(base, bytes, nullptr, reason)`
  for every pending linear or small (<= 64 KiB) image, with the default `PublishScope::Whole`.
- `FlushPending` (`Texture.cpp:4346`) waits when `Recorder::PendingWriterSerial` finds an
  unfinished batch that writes the range or holds it in an address-based lease
  (`Graphics/src/Recorder.cpp:3223`): `Sync` for the open batch, `FinishUpTo` otherwise. This is
  the wait.
- `writeBackLayers` then decides per 64 KiB block (`Texture.cpp:6404`):
  `written = kept || unknown || (stamped && !unchangedSinceBaseline)`; an untracked unit
  (`layerGeneration == 0`) outside a drawn rectangle is stored whole before that line. Written
  blocks keep the guest bytes; the rest are retiled on the GPU and copied into the host import.

Why it waits: an address-based lease stamps the pages it stored only when its batch completes
(commit 9fffceaa9), and `unchangedSinceBaseline` reads bytes the pending writers have not
produced yet. Submitting without waiting is not enough.

## Design decision

Upstream already narrows this wait by writer provenance in open pull requests: #8887 (writers over
the stored layers only), #12070 (the image's own stamped write-backs), #10382 (read-only leases),
#13920 (per-lane prefixes). What remains are true writers whose effect on the decision is unknown
until they run, so provenance cannot remove the comparison. The chosen design keeps the store on
the GPU, in queue order behind the writers, and moves only the decision there. A completion-time
CPU store was rejected: every GPU reader gate (draws, fills, copies, indirect) would have to learn
a new deferred-writer kind, as copied writers needed. Waiting less for "readers" was rejected:
address-based reads are leases that may store.

## The rule (`Graphics/include/LabelCompare.hpp`)

Per block, from its state at the wait (`kept` at unwatch, `unknown` unit, `stampedNow`,
`changedNow` against the baseline, an explicit pending `writerOver`, a pending `leaseOver`):

| State | Mode |
| --- | --- |
| unknown unit | store (whole unit, as writeBackLayers) |
| kept at unwatch | keep |
| no writer, no lease | keep if stamped and changed, else store |
| lease, unstamped, and (changed or explicit writer) | fall back to the wait |
| stamped, or lease | GPU: keep iff bytes differ from the baseline at that queue point |
| explicit writer only, unstamped | store |

Explicit writers stamp when recorded, so their stamps are already in `stampedNow`; a lease's
change implies its completion stamp; an unstamped block under a lease that already differs, or
that an explicit writer also changes, cannot attribute a change to the lease, so it falls back.

Bug found during stage 2: stage 1 classified an untracked unit as keep. writeBackLayers stores it
whole. Fixed in commit 2; a stage-1 shadow run on GTA would have shown it as mismatches.

## Stage 1: shadow check (commit 1)

`APS5_LABEL_COMPARE_SHADOW=1` classifies the single image of a waiting label store just before
the wait (`labelCompareClassify`, `Texture.cpp:4511`), lets the wait and writeBackLayers run,
then compares each block's prediction (using the post-wait byte compare as the GPU's answer) with
the real decision, and prints a `[label-compare]` line in the storage profile: waits checked,
eligible and refused with their wait time, refusal reasons, blocks per mode, mismatches. `any`
checks every waiting flush and prints a line per check. Behaviour unchanged.

## Stage 2: the GPU compare (commit 2)

Opt-in: `APS5_LABEL_GPU_COMPARE=1`.

- `FlushPending` (`Texture.cpp:4446`): for a label-reason flush of exactly one image with an
  unfinished writer, classify; if every block is decidable, no unit shadow overlaps the flush
  range, and the pipeline exists (`Recorder::ConditionalStoreReady`, `Recorder.cpp:2148`), skip
  the `Sync`/`FinishUpTo` and attach the modes to the write-back. Otherwise wait as before and
  count the refusal.
- `writeBackLayers` (`Texture.cpp:6406`): blocks follow the classified modes; keep blocks are
  skipped, store and GPU blocks go into the stored ranges; GPU blocks are also listed as
  conditional and mark the layer as skipped. `Require`s pin the path: only the whole-layer import
  path with one import segment, no block units, no drawn rectangle, no stored save.
- The import copy (`Texture.cpp:6629`) leaves the conditional ranges out;
  `recordConditionalStores` (`Texture.cpp:6260`) copies the retiled bytes and the baseline
  (`original` or `generationBaseline`, as `unchangedSinceBaseline`) into a host-visible staging
  buffer the batch keeps, then `Recorder::RecordConditionalStore` (`Recorder.cpp:2175`) runs
  `Graphics/shaders/ConditionalStore.comp` twice per range through buffer device addresses:
  pass 0 sets the range's flag when any import word differs from the baseline; after a compute
  barrier, pass 1 copies the result words only if the flag is clear. A kept range is never
  written, so a CPU store into it is not overwritten with stale bytes. A completion action counts
  stored and kept ranges.
- Bookkeeping: identical to today's partial skip. The stored ranges (GPU ones included) are
  stamped with `MarkWritten`; the layer keeps its old generation (stale), so the next refresh
  re-uploads it from guest memory, which then holds whichever outcome the GPU chose;
  `forgetStored` drops `original`; no stored save is committed. The pending-write note is the
  one today's path makes, so CPU readers still sync on the batch.
- Images up to `LabelCompareMaxBytes` (256 KiB) qualify; base and size must be whole words.

## Safety, condition by condition

| Condition | Argument | Evidence |
| --- | --- | --- |
| Identical keep or skip decisions | Rule above; modes are the decisions | `agc_label_compare`: 198 modelled outcomes, 0 mismatches; six broken rules fail (3, 1, 5, 2, 22, 108 mismatches). Driver: flush ordering `--label-gpu-compare` keeps the leased block and stores the other; fails with leases ignored |
| CPU-write preservation | Kept and CPU-decided blocks are not written; a GPU-kept range is not touched word by word | `agc_conditional_store` "a word changed ... keeps its whole range"; per-word decision mutation fails |
| Ordering behind writers and alias stores | Recorded into the open batch after the writers; the write-back's import-ready barrier (ALL_COMMANDS and HOST to COMPUTE) precedes the compare; queued label and key stores flushed first as today | `agc_conditional_store` "GPU write recorded before the store is compared" vs "after ... not compared"; driver test with the writer in the open batch |
| Baseline lifetime and versioning | Baseline copied into the staging buffer when recorded, kept by the batch | code (`recordConditionalStores`) |
| Import lifetime | Same import, offset and lifetime as today's copy into it | code; not tested separately |
| Conservative generation bookkeeping | Same state as a partial skip, which today's code produces and refreshes | code; not tested at runtime |
| Fallback intact | Every refusal waits as before; switch off by default | flush ordering `--label-gpu-compare` fails with the switch off ("waited") |

Refused, so still waiting: images over 256 KiB, block units, DCC keys, drawn targets, no host
import, imports that are compared instead of write-watched, unit shadows over the image or the
range, pending completion stores or labels, listed copied writers, fallback blocks, a missing
pipeline. Listed in `docs/dev/debt/libSceAgcDriver.md`.

## Tests run (Linux, lavapipe, LLVM 20.1.2, Vulkan validation layer 1.3.275)

Focused set with `ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ctest -V`:

- `main`: 10/10 passed.
- Head: 14/14 passed, and 14/14 again with `APS5_LABEL_GPU_COMPARE=1` forced on every test.
- No `was not found but was requested by env var VK_INSTANCE_LAYERS`; the layer loads
  (`VK_LOADER_DEBUG=layer`). The only VUID is `VUID-VkDeviceCreateInfo-pNext-pNext` (old layer
  against newer structures); per-test counts identical to `main`, plus one per new registration.
- `python3 tools/check_conventions.py --base upstream/main`: passed.

Pattern: `agc_driver_(storage_flush_ordering|linear_readback_label|alias_copy_label|storage_write_back_overhang$|storage_write_back_no_window|storage_one_slice_alias|storage_read_during_idle_store|copied_writer_label_order)|agc_label_compare|agc_conditional_store`.

New registrations: `agc_label_compare` (host), `agc_conditional_store` (recorder tests
`--conditional-store-only`), `agc_driver_storage_flush_ordering_label_compare` (shadow `any`,
fails on a mismatch), `agc_driver_storage_flush_ordering_gpu_compare` (`--label-gpu-compare` with
`APS5_LABEL_GPU_COMPARE=1;APS5_WRITE_WATCH_IMPORTS=watch`).

## Environment findings

- On Linux with lavapipe the import probe sees lavapipe's CPU-side GPU writes resolve write
  protection, so imports become "compared", not watched, and the classifier refuses them
  ("unwatched"). The driver test forces `APS5_WRITE_WATCH_IMPORTS=watch`. On the target machine,
  check the `[write-watch] host imports ...` line: if imports are compared there, stage 2 never
  fires and needs an untracked variant (decision = pure byte compare against `original`; the
  rule is simpler but saved-byte tickets and `awaitStoredSave` need handling).
- The real label path uses `PublishScope::Whole`; stage 2 first required `None` and would never
  have fired. It now requires no unit shadow over the range instead.
- The in-flight writer case cannot show a skipped wait in a test: the test waits for the GPU, so
  `Reap` retires the batch without a wait.

## Not verified

- Any title. No GTA run, no frame-time number.
- Hardware GPUs; only lavapipe.
- Generation bookkeeping and import lifetime at runtime (argued from code only).
- Concurrent CPU stores during the window: same race class as today's decision after the wait,
  not tested.
- `tools/pr_overlap.py` was not run; no pull request was opened.

## What to measure on GTA

1. Same machine and scene, `main` and head, `tools/trace_digest.py` summaries.
2. Head with `APS5_LABEL_COMPARE_SHADOW=1` and the storage profile: mismatches must be 0; the
   eligible wait time bounds what stage 2 can save; refusal reasons show what to cover next.
3. Head with `APS5_LABEL_GPU_COMPARE=1`: the `[label-compare] GPU compare` line gives waits
   skipped, blocks per mode, GPU-compared ranges stored and kept, and the reasons waits stayed.
   Compare frame p50 and the label wait with step 1. Check the `[write-watch]` import line first.
4. Watch for visual or logic regressions in label-polled data (stale or torn small images).

## Rules for agents on this branch

Push only to `origin` (`chitters2004/AnyPS5`), branch `draft/label-compare`; `upstream`
(`boykopovar/AnyPS5`) is fetch-only. No pull requests, issues or posts. No code comments except
`#endif` and namespace ends. Conventional Commit subjects ending with the Co-Authored-By trailer.
