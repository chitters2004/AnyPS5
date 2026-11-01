# AGC guard check: certificate vs snapshot

Branch `draft/guard-snapshot`, based on `draft/agc-guard`. This folder is for agents and reviewers reading the branch. `tools/check_conventions.py` allows Markdown and logs under `docs/dev/`, so it passes, but `AGENTS.md` says reports belong in the pull request and no other files may be added. Before opening a pull request, drop every `docs(agc)` commit (`88079274b`, `af5a4f28e`, `aabfd6e43`, `8adf95f64`, `126fff126`, `e4d71ebe2`, `850f87fe2`, `da5182b03`, `26c7b2105`, the three docs commits after `5293a5cc7`, the docs commit after `88142952c` and the one that adds this sentence) and move the text into the PR body. They are interleaved with code commits, so rebuild the branch from the `fix`/`perf` commits in order.

## Commits

| SHA | Change |
|---|---|
| `2e098c60f` | fix: advance the guard growth before the first insertion. A throw between `push_back`s no longer leaves a range out of every certificate. |
| `1241edd3c` | fix: the certificate lives on the heap and is owned by the TEB/thread slot. A `thread_local` release object deletes it and points the slot at a retired sentinel (growth 0, never matches), so reads during Windows emutls teardown take the exact path instead of touching freed memory. |
| `2a4339321` | perf: classify a miss once (`ResidentGuardOverlaps_nid_no_patch` returns color/depth bits) and run only the overlapping registries' exact paths. An overlapping read drops from 6 scans and 2 exact passes to 2 scans and 1 exact pass. **Pick this commit for the certificate design.** |
| `7133b7d37` | perf: replace the certificate with a process-wide snapshot. |
| `edaacd785` | perf: `ShaderResources::requireGuestBuffers` makes one combined check (`RequireNoMultisampleTarget` gained an optional `htileClear`), not separate depth and color checks. |
| `88142952c` | perf: CPU index expansion reads the index range once instead of once per index (see "Index expansion batching"). |
| `5293a5cc7` | test: race the full color target and native depth registrations against checks, and reads spanning a color and a depth guard (see "Registration races"). |
| `b7a439237` | fix: a snapshot lookup ORs the tags of every interval the read covers (see "Independent review"). |
| `7d96fe73e` | fix: `GuardOneFragmentFmask` marks the color guard resident before its registration finishes (see "Bug found by the range proofs"). |
| `ac35a7e35` | test: the mapping proof test aligns its block at run time (MinGW rejects static objects aligned above 8192 bytes; the convention checker flagged it). |
| `cff455dfe` | perf: `GuestMemory::CheckRangeProved` and mapping proofs on draw recipes, so a draw-cache hit skips re-querying its index and vertex mappings until guest memory changes (see "Mapping proofs"). |
| `fb7ca46f3` | perf: a 64-entry log of recent registrations, so a stale guard proof is revalidated against the registrations since it rather than fully re-checked (see "Registration log"). The range proof moves to `GuestMemory::RangeProof`. |
| `876e52aca` | perf: exact range proofs on draw recipes, so a draw-cache hit skips its index and vertex range checks while nothing has registered (see "Draw proofs"). |
| `9a7128ddb` | perf: growth-stamped proofs (see "Skipping repeated checks"). `requireGuestBuffers` skips its whole loop while nothing has registered, and every registration now advances the growth when it starts and again when it finishes. |

## Snapshot design

- **Layout:** `constinit` arrays of 256 `std::atomic<uint64_t>` begin/end pairs, sorted and merged. The low 2 bits of `begin` carry the registry tags (color 1, depth 2). Guard ranges are widened to at least 1 byte, `begin` is rounded down to 4, and `end` saturates.
- **Reader:** load the sequence (acquire), reject an odd sequence, binary search on `end`, test one interval, acquire fence, recheck the sequence. Up to 4 attempts, then report both tags. A read writes nothing and keeps no per-thread state.
- **Registration:** inside the registry's unique lock: bump growth, then add the ranges to the snapshot under a leaf writer mutex, then `push_back`. A throw leaves a superset, which is safe.
- **Removal:** does not touch the snapshot, so the snapshot stays a superset of the live guards.
- **Miss:** scan only the tagged registries under their shared locks and pass the result to the exact paths. If the scan clears a tag, that read counts as stale.
- **Rebuild trigger:** the 64th stale read. The rebuild reads color, then depth, each under its own shared lock with no other registry lock held, so it adds no lock-order edges. It publishes only if growth did not move; otherwise it abandons. The TSan model (mutant 3) shows that without this check a concurrent registration is lost.
- **Capacity:** 256 merged intervals (4 KiB). An insertion past capacity marks the snapshot overflowed, so every read takes the exact path, until a rebuild fits.
- **Growth serial:** kept, because `ShaderResources::Revalidate` uses it.
- **Cleanup:** per device; it removes guards and never resets the snapshot.

## Benchmark

This is a standalone model of both registries (`std::shared_mutex` and a vector each; three in four guards color, the rest depth) plus each design. Guards are 64 KiB at a 1 MiB stride. A writer thread inserts churn guards and keeps 4 live. Machine: Linux, 4-vCPU Xeon @ 2.10 GHz, g++ 13 `-O2`, 200 ms per cell, one run per cell.

Values are total ns per check (reader time plus writer CPU, divided by reads), minus the null-design harness overhead (31–56 ns). Each cell reads main / cert-draft / cert-fixed / snapshot.

Patterns:
- **disjoint-local:** one gap per thread.
- **disjoint-spread:** random gap among all G+1.
- **near-neighbour:** 64 B immediately before and after one guard.
- **cyclic-N:** round-robin over N separated gaps. This defeats the 4-entry round-robin certificate.
- **overlap:** inside a guard, which is kept.
- **retired-stale:** 64 guards were registered and removed before the run.

| Threads | Guards | Reg/s | disjoint-local | disjoint-spread | near-neighbour | cyclic-5gaps | cyclic-8gaps | overlap | retired-stale |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 4 | 0 | 39/6/0/0 | 41/26/24/18 | 48/0/0/5 | 34/54/46/1 | 38/39/27/0 | 39/98/55/30 | 46/0/0/0 |
| 1 | 4 | 1000 | 48/0/0/0 | 58/33/28/23 | 42/0/2/1 | 42/54/53/3 | 38/33/26/4 | 43/110/71/33 | 39/0/0/2 |
| 1 | 12 | 0 | 41/3/8/10 | 53/73/60/40 | 46/0/8/7 | 42/67/66/8 | 53/66/47/4 | 45/101/69/32 | 41/0/0/8 |
| 1 | 12 | 100 | 44/1/0/4 | 51/66/70/28 | 55/1/8/4 | 53/75/56/7 | 39/58/52/2 | 38/106/90/41 | 51/4/5/6 |
| 1 | 12 | 1000 | 49/7/4/7 | 71/88/75/48 | 48/5/8/6 | 56/67/67/15 | 55/69/75/11 | 48/106/72/42 | 45/1/3/5 |
| 1 | 12 | 5000 | 52/29/6/12 | 79/68/94/38 | 53/6/2/12 | 66/79/73/13 | 50/72/66/12 | 46/128/86/68 | 55/4/10/11 |
| 1 | 64 | 0 | 64/2/2/4 | 90/136/152/48 | 90/1/0/9 | 74/120/117/5 | 64/143/122/5 | 59/108/133/28 | 47/0/0/15 |
| 1 | 64 | 1000 | 65/0/0/2 | 124/140/181/38 | 66/0/0/0 | 76/129/129/22 | 73/95/95/1 | 47/129/120/34 | 70/0/0/3 |
| 1 | 256 | 0 | 182/0/0/2 | 189/275/367/48 | 168/2/0/5 | 176/282/313/9 | 171/298/396/1 | 86/134/392/34 | 127/0/2/3 |
| 1 | 256 | 1000 | 179/2/1/8 | 184/301/378/57 | 175/0/0/4 | 195/345/357/13 | 185/248/333/11 | 81/138/360/47 | 147/1/0/15 |
| 2 | 4 | 0 | 393/1/2/6 | 374/92/90/26 | 374/3/0/4 | 433/420/463/15 | 391/303/300/7 | 408/1350/491/141 | 344/4/4/7 |
| 2 | 4 | 1000 | 402/5/2/10 | 411/105/101/30 | 358/4/3/8 | 338/383/485/9 | 383/306/255/73 | 323/1264/687/214 | 377/8/8/7 |
| 2 | 12 | 0 | 392/2/0/5 | 404/339/314/32 | 359/2/1/9 | 375/452/450/12 | 382/334/403/5 | 376/1003/571/177 | 341/3/4/4 |
| 2 | 12 | 100 | 356/2/6/6 | 397/423/388/36 | 386/2/6/10 | 429/441/388/12 | 349/553/460/7 | 294/1082/632/172 | 344/3/4/5 |
| 2 | 12 | 1000 | 360/2/4/16 | 412/375/365/37 | 363/2/3/8 | 409/408/256/8 | 348/542/475/10 | 323/979/588/272 | 391/5/12/8 |
| 2 | 12 | 5000 | 377/1/1/6 | 394/408/440/31 | 368/1/0/3 | 461/543/504/11 | 447/553/404/6 | 349/1002/656/264 | 435/0/6/12 |
| 2 | 64 | 0 | 583/2/3/8 | 533/624/562/45 | 543/6/1/12 | 528/653/681/6 | 448/696/517/3 | 457/1132/818/243 | 424/0/0/4 |
| 2 | 64 | 1000 | 456/3/0/3 | 586/634/596/41 | 496/5/0/15 | 606/712/753/7 | 550/585/576/2 | 425/999/797/140 | 448/0/0/4 |
| 2 | 256 | 0 | 781/0/0/4 | 730/864/898/51 | 661/0/0/9 | 700/629/738/8 | 648/856/822/5 | 498/1137/972/186 | 632/0/0/3 |
| 2 | 256 | 1000 | 864/0/1/8 | 767/782/961/58 | 772/4/6/3 | 640/854/696/6 | 731/749/851/8 | 572/1200/994/176 | 646/0/0/4 |
| 4 | 4 | 0 | 913/18/10/11 | 883/202/170/27 | 1083/12/8/10 | 851/993/1037/14 | 1074/647/652/11 | 977/3291/1845/446 | 945/10/5/6 |
| 4 | 4 | 1000 | 902/9/8/9 | 1055/197/213/24 | 1007/9/7/6 | 1136/1228/1178/8 | 1097/721/598/9 | 990/2614/1578/485 | 1043/21/13/6 |
| 4 | 12 | 0 | 1010/18/20/12 | 1039/663/695/27 | 1086/10/6/6 | 1036/1166/1023/12 | 1012/1170/1004/11 | 980/3156/1789/441 | 855/12/7/5 |
| 4 | 12 | 100 | 927/4/11/0 | 1001/696/740/18 | 1056/6/15/0 | 1046/1142/1012/0 | 984/1226/1159/4 | 1123/3514/1909/448 | 1066/0/21/0 |
| 4 | 12 | 1000 | 1065/17/10/17 | 1018/829/805/46 | 924/26/14/11 | 973/1108/852/19 | 950/917/1238/34 | 944/3392/1756/427 | 1069/24/14/22 |
| 4 | 12 | 5000 | 1030/21/12/7 | 989/819/865/34 | 898/24/17/11 | 981/1275/1122/18 | 975/962/1337/14 | 1034/3858/1943/494 | 1109/15/2/9 |
| 4 | 64 | 0 | 1190/10/0/0 | 1164/1327/1331/31 | 1152/9/4/0 | 1144/1374/1381/2 | 1196/1370/1410/3 | 1165/3637/1970/517 | 1150/11/12/7 |
| 4 | 64 | 1000 | 1282/10/14/6 | 1206/1275/1302/31 | 1275/7/0/6 | 1121/1266/857/8 | 1147/1307/1378/19 | 1079/3112/1969/474 | 1140/6/16/11 |
| 4 | 256 | 0 | 1255/16/7/9 | 1268/1520/1531/47 | 1352/19/10/18 | 1266/1508/1501/13 | 1413/1434/1386/12 | 1201/3485/2055/545 | 1300/11/5/9 |
| 4 | 256 | 1000 | 1265/17/14/9 | 1332/1653/1545/58 | 1599/21/8/14 | 1196/1558/1479/24 | 1312/1497/1440/12 | 1160/3229/1909/541 | 1280/27/18/16 |

| Threads | Guards | Reg/s | Design | Writer CPU µs/reg | Seq retries/read | Fallbacks/read | p99.9 local ns | p99.9 cyclic-5 ns | Rebuilds |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 12 | 5000 | main | 7.34 | 0.000000 | 0.0000 | 369 | 774 | 0 |
| 1 | 12 | 5000 | cert-draft | 1.89 | 0.000000 | 0.0003 | 178 | 909 | 0 |
| 1 | 12 | 5000 | cert-fixed | 1.61 | 0.000000 | 0.0002 | 33 | 492 | 0 |
| 1 | 12 | 5000 | snapshot | 8.19 | 0.000717 | 0.0002 | 87 | 63 | 11 |
| 4 | 12 | 5000 | main | 74.38 | 0.000000 | 0.0000 | 13937 | 24059 | 0 |
| 4 | 12 | 5000 | cert-draft | 8.67 | 0.000000 | 0.0003 | 841 | 33861 | 0 |
| 4 | 12 | 5000 | cert-fixed | 8.53 | 0.000000 | 0.0003 | 770 | 29115 | 0 |
| 4 | 12 | 5000 | snapshot | 21.81 | 0.000777 | 0.0002 | 615 | 758 | 36 |
| 4 | 256 | 1000 | main | 71.21 | 0.000000 | 0.0000 | 19436 | 24446 | 0 |
| 4 | 256 | 1000 | cert-draft | 7.09 | 0.000000 | 0.0001 | 729 | 25982 | 0 |
| 4 | 256 | 1000 | cert-fixed | 6.36 | 0.000000 | 0.0001 | 819 | 45832 | 0 |
| 4 | 256 | 1000 | snapshot | 34.64 | 0.000172 | 0.0000 | 627 | 606 | 8 |

Worst-case latencies are 0.5–8 ms in every design: that is preemption on a 4-vCPU guest, not the code, so p99.9 is reported instead. Snapshot sequence retries stay at or under 0.12% of reads.

### Reproducing the published numbers

- **Main:** the first run (sharing CPU with a build) gave 542–670 ns gross at 12 guards and 4 threads, which matches the published 557–694 ns. On an idle machine it is about 1,000 ns at 4 threads and 360–400 ns at 2; more reader overlap means more lock contention. The Windows global spinlock behind `shared_mutex` is not modelled.
- **Certificate hit:** 0–20 ns net, matching the published 4–8 ns within harness noise.
- **Certificate miss:** 1.0–1.2 µs at 4 threads, matching the published 1.2 µs.

## Correctness model

`guard_model.cpp` (in this folder) is built with `g++ -fsanitize=thread`. Three readers check the most recently published guard while a writer registers guards (every fifth registration throws between two inserts), removes old ones and triggers rebuilds. The property: a registration that happens-before a check is always seen, unless that guard was later removed.

| Variant | Result (3000 rounds) |
|---|---|
| certificate, clean | 0 violations, TSan clean |
| certificate, no growth bump | violations on almost every check |
| certificate, growth bumped after the `push_back`s with a throw between | 38,401 violations (the fix 1 bug) |
| snapshot, clean | 0 violations, 730 rebuilds, TSan clean |
| snapshot, insertion not added | violations on almost every check |
| snapshot, rebuild without the growth check | 19,466 violations |
| snapshot, plain stores instead of atomics | TSan data race in `publish` |
| snapshot, proofs, no finishing growth advance (mutant 6) | 160,033 violations |
| certificate, proofs, no finishing growth advance (mutant 6) | 0 violations: the certificate's slow path waits on the registry lock, so it cannot run mid-registration |

After `9a7128ddb` the model also caches verdicts the way the driver does: each reader keeps 8 per-thread proofs keyed by guard slot and growth, and a registration yields between its snapshot insertion and its `push_back` to widen the race window. Mutant 1 now removes both growth advances, and mutant 5 skips the finishing advance on the throw path; both still model the original bugs. The numbers in `data/model_*.log` come from this final model, so some differ from the earlier table values (for example mutant 5 on the certificate now shows 31,143 violations).

Limits: on x86 (TSO), TSan does not model fence reordering, so a missing or wrong fence would not be caught. Weak-memory hosts are unverified.

## Driver tests (lavapipe, llvmpipe LLVM 20.1.2)

Run with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 ctest -R <guard tests> -V` over `guard_snapshot` (or `guard_certificate`), `alpha_to_coverage`, `depth_plane_guest_layers`, `native_multisample_depth`, `multisample_retirement`, `depth_storage`, `depth_surface_last_writer`, `sample_multisample_target`, `native_depth_bda_clear` (skips: no udmabuf), `depth_fast_clear_submit` and `graphics`.

- **Results:** all pass on main (`cf79276fe`), on `2a4339321` (certificate with fixes), on `7133b7d37` and on `edaacd785`. The layer loaded in every run.
- **Validation:** the only validation error, in every build, is `VUID-VkDeviceCreateInfo-pNext-pNext` at device creation (the layer is older than the headers), 3 per test. No new VUIDs.
- **New guard tests:** the snapshot test adds the stale-rebuild check, the overflow check (300 guards) and rebuilds running concurrently in the stress test. The hit paths are checked with the driver image's writable segments frozen read-only.
- **Conventions:** `tools/check_conventions.py --base upstream/main` passes on every commit, including these docs commits (the checker allows `.md` and `.log` under `docs/dev/`). An earlier version of this report wrongly said they fail `notes-file`.
- **`9a7128ddb`:** all 11 tests pass under validation, with the same 30 device-creation VUIDs and nothing new (`data/ctest_proof_validation.log`).

## Verdict

Ship the snapshot.
- **Where the designs differ:** with one area per thread they tie (both under 20 ns). When a thread touches more than 4 gaps between registrations (spread reads, 5 or more gaps, or more guards), the certificate misses on every read and costs as much as main or more (about 1 µs at 4 threads), while the snapshot stays at 8–58 ns.
- **Overlapping reads:** the snapshot is cheaper there too.
- **Writer upkeep:** the snapshot costs 8–35 µs of CPU per registration, against 6–9 µs for the certificate. That only matters at thousands of registrations per second, and it is already counted in the totals.

**Deciding GTA measurement:** per-thread, per-frame counts of certificate hits, misses and snapshot stale reads, plus registrations per second, captured with `tools/trace_digest.py` on main and on each commit in the same scene on the same machine. If GTA threads touch at most 4 gaps between registrations, the two designs are equal and the certificate's smaller writer cost wins narrowly; otherwise the snapshot wins.

## Not verified

- Windows: MinGW `constinit`, the emutls teardown ordering (fix 2 is only exercised on Linux, where the bug cannot occur), and winpthreads `shared_mutex` cost.
- Weak memory ordering.
- No GTA trace.
- `tools/pr_overlap.py` was not run (no PR).

## Reproducing

The sources are in this folder: `guard_bench.cpp` (the benchmark), `guard_model.cpp` (the concurrency model) `probe_bench.cpp` (the proof-owner benchmark), `log_bench.cpp` (the registration-log benchmark), `driver_guard_cost.cpp` (in-driver costs; build it by copying it into `core/libs/prx/libSceAgcDriver/tests/execution/DriverGuardCost.cpp` and adding `DriverGuardCost` to `ANYPS5_AGC_EXECUTION_TESTS`) and `windows_teardown.cpp` (build with `x86_64-w64-mingw32-g++-posix -O2 -std=c++20 -static [-DFIXED] -Icore/libs`). The last two include `guard_bench.cpp`; build them from this folder with `g++ -O2 -std=c++20 -pthread probe_bench.cpp -o probe_bench` (likewise `log_bench`). Neither is part of the CMake build. The benchmark's `cert-fixed` design models commit `2a4339321`, and its `snapshot` design models `7133b7d37`, including the growth-checked rebuild.

Benchmark. The first argument is milliseconds per cell. The second is `table` (the grid above), `full` (every rate for every guard count) or `quick` (4 threads, 12 guards). Run it on an idle machine; a concurrent build roughly halves main's measured contention. Output is CSV, one row per design × threads × guards × pattern × rate. Subtract the `null` design's `total_ns_per_op` to remove harness overhead.

```bash
g++ -O2 -std=c++20 -pthread docs/dev/reports/guard-snapshot/guard_bench.cpp -o guard_bench
```

```bash
./guard_bench 200 table > bench_table.csv
```

Model. `MUTANT` selects the planted bug: 0 clean, 1 no growth bump, 2 snapshot insertion skipped, 3 rebuild without the growth check, 4 plain stores (TSan race), 5 growth bumped after the inserts with a throw between, 6 no finishing growth advance (matters only with proofs on the snapshot). The first argument picks the design (`c` certificate, `s` snapshot), the second the number of rounds. Exit code 0 means no violations, 1 means violations, and 66 is a TSan report. Clang on this image lacks the TSan runtime, so use g++.

```bash
for m in 0 1 2 3 4 5 6; do g++ -std=c++20 -O1 -g -fsanitize=thread -Wno-tsan -DMUTANT=$m docs/dev/reports/guard-snapshot/guard_model.cpp -o model_$m; done
```

```bash
for m in 0 1 5 6; do ./model_$m c 3000; echo "certificate mutant $m rc=$?"; done; for m in 0 2 3 4 5 6; do ./model_$m s 3000; echo "snapshot mutant $m rc=$?"; done
```

Expected: certificate mutants 1 and 5 and snapshot mutants 2, 3, 4, 5 and 6 fail; both clean runs and certificate mutant 6 pass.

## Skipping repeated checks (`9a7128ddb`)

Idea: only a registration can make a clear range overlap a guard, so a caller that re-reads the same ranges can remember "proven clear at growth g" and skip the check while the growth is still g. `ShaderResources::Revalidate` already did this for its own checks (`provedGuards`).

- **`ResidentGuardProof`** (in `MultisampleTarget.hpp`) packs growth and a 16-bit key into one atomic, so concurrent writers can never pair a growth with the wrong key. `Holds(growth, key)` is one acquire load and a compare. A growth or key that does not fit packs to 0, which never holds, so those callers just keep checking.
- **`ShaderResources::requireGuestBuffers`**, run on every `Bind` and `PrepareDrawBindings` of a cached object, loads the growth first. If the proof holds for that growth and the allocation count it returns at once. Otherwise it checks every buffer and records the proof only if all were proven clear: no moved buffers, and no buffer accepted only through an HTILE clear (that acceptance depends on surface state the growth does not track). Allocations only grow during the build and are never edited, which is why the count is enough as a key.
- **Two growth advances per registration.** `ResidentGuardRegistration` (RAII) advances the growth and updates the snapshot before inserting, and advances it again in its destructor after inserting, also when an insert throws. With only the first advance, a lock-free snapshot lookup between that advance and the insertion saw no guard and recorded a proof at the new growth that kept holding afterwards. The same window existed in `Revalidate`'s epoch proof once fix 1 moved the advance before the insertion; the second advance closes both. The certificate design does not have the window, because its slow path waits on the registry lock.
- **Not covered:** draw-time vertex and index checks (`Draw.cpp`) and DMA checks (`PacketMemory.cpp`) rebuild their ranges on every draw or packet and have no object that persists between them, so they still do a snapshot lookup each. Giving them proofs needs a per-pipeline or per-draw-state cache, which is a larger change. The generic `GuestMemory::Read` → `GuardRead` path has no owner either.
- **Tests:** `CheckProof` covers recording, key mismatch, oversize growth and key, invalidation by a registration, survival across removals, and a read of a held proof writing nothing in the driver image. The stress test's readers also keep proofs, and fail if one holds for a guard that was published before the check. Removing the finishing advance makes this test fail in 3 of 3 runs (855, 1264 and 1598 stale acceptances); the code was restored before committing.
- **Not measured:** a proof check is one load and a compare against N snapshot lookups (8–58 ns each in the benchmark) per bind. The in-driver saving per frame needs the GTA trace.

## Draw proofs (`876e52aca`)

**Measurement first** (`probe_bench.cpp`, results in `data/probe.csv`): each simulated draw checks 3 ranges spread over the guard gaps, with no registrations, 1 or 4 threads, and 100 to 5000 distinct draws.

| ns per draw | 12 guards, 1 thread | 12 guards, 4 threads | 256 guards, 4 threads |
|---|---|---|---|
| 3 snapshot lookups (before) | 26–34 | 51–83 | 93–111 |
| proof held by the draw's owner | 0.8–1.6 | 0.8–1.2 | 1.1–2.3 |
| per-thread 512-entry exact-range cache | 30–34 | 116–202 | 167–190 |

A proof only pays when an object that persists between draws holds it; a per-thread cache is no faster than the snapshot, and slower with 4 threads. So proofs go where an owner exists.

- **Owner:** `DrawRecipe`, the immutable record a draw-cache hit replays. Draws with no recipe (the first draw, draw-cache misses) keep the per-range check. The recipe's header comment still says "immutable"; the new member is `mutable`, and the comment was left alone because the project allows no new comments.
- **What it holds:** `mutable std::array<ResidentGuardRangeProof, 8> guardProofs`: slot 0 for the index range, slots 1–7 for the first seven vertex copy ranges. `prepareDrawInputs` calls `RequireNoMultisampleTargetProved(proof, address, bytes, access)`, which loads the growth, returns if the proof holds for exactly that range, otherwise runs the full check and records the proof only when the range was proven clear.
- **`ResidentGuardRangeProof`:** a seqlock of growth, address and size with its own sequence. A writer claims the sequence with a compare-exchange and skips recording if another writer holds it. A reader takes the sequence, reads the three fields, fences, rechecks, and needs an exact match. Recipes have no lock the proof could rely on (only a comment says recipe draws run under the GPU mutex), so the proof is safe on its own.
- **A race the tests caught:** the first version used the growth itself as the sequence. Two writers recording at the same growth restore the same value (ABA), and a reader accepted a range assembled from both writers (seen 2 times in one run of the real code). The fix is the dedicated sequence. With a strengthened test (2 writers × 2,000,000 records, 2 readers): the ABA variant fails 5/5 runs (404 mixed ranges in one), the fixed code passes 20/20 and 10/10, and a variant that skips the compare-exchange claim fails only 1/5 (narrower window). Neither range-proof variant has been run under the TSan model.
- **DMA:** not done. The DMA source check in `PacketMemory.cpp` runs on packets parsed from guest command buffers on each submission, and nothing persists per packet, so it has no owner; the per-thread cache measured above would make it slower. It stays at one snapshot lookup per DMA packet.
- **Tests:** `CheckRangeProof` covers exact-match, other ranges, invalidation by a registration over the range, refused ranges never being proven, removal, held proofs writing nothing in the driver image, and the concurrent-writer check. 16 tests pass under validation, including `recorder` and `sample_order`, which replay recipes; `sample_order_grid_reference` and `sample_order_recipe_depth_locations` skip on lavapipe. The only validation error is still the device-creation `VUID-VkDeviceCreateInfo-pNext-pNext`, 3 per test (`data/ctest_drawproof_validation.log`).
- **Not measured:** the saving in a real frame, which depends on the draw-cache hit rate (the GTA trace).

## Registration log (`fb7ca46f3`)

- **What:** each registration's ranges go into a 64-entry ring, stamped with the growth value its finish produces. Both growth advances of a registration are now taken under the snapshot writer mutex, so growth changes are serialized and the stamp is exact. The ring is written under its own sequence counter and read lock-free.
- **Revalidation (`ResidentGuardsUnchangedSince_nid_no_patch`):** a proof stamped `p` is still valid at growth `g` if every log entry stamped in `(p, g]` misses the proven range, and the ring still reaches back to `p` (an entry stamped `<= p` is retained, or nothing was ever evicted). Entries stamped after `g` belong to registrations finishing concurrently; the proof is re-stamped at `g`, so they are caught next time. A proof older than the ring, a log write that failed (lock exception, which marks the log lost for good) or 4 torn reads fall back to the full check.
- **Users:** `RequireNoMultisampleTargetProved` (draw recipes) and `ShaderResources::requireGuestBuffers` (every guest buffer is revalidated against the log; any overlap means the full loop). `ResidentGuardProof` and `GuestMemory::RangeProof` gained `Stamp()` for this.
- **Measured** (`log_bench.cpp`, `data/log_bench.csv`; 12 guards, 1000 draws of 3 ranges, a busy-waiting writer registering at the given rate; ns per draw, then full checks per draw):

| Rate/s | Threads | 3 lookups | proof, full recheck | proof + log |
|---|---|---|---|---|
| 0 | 1 | 41.1 | 0.9 | 1.1 |
| 10,000 | 1 | 35.2 | 1.6 (0.016) | 0.9 (0.0000) |
| 50,000 | 1 | 129.4 | 42.0 (0.288) | 6.8 (0.0002) |
| 10,000 | 4 | 116.2 | 1.8 (0.008) | 1.4 (0.0001) |
| 50,000 | 4 | 112.8 | 1.7 (0.012) | 1.8 (0.0001) |

  The log only matters at tens of thousands of registrations per second: at 50,000/s on one thread it cuts a draw from 42 to 7 ns. At realistic rates both proof variants are about 1 ns. With 4 readers the busy-waiting writer shares 4 CPUs, so its real rate is lower than nominal.
- **Tests:** `CheckGuardLog` covers a registration elsewhere keeping a proof, one over the range invalidating it, a proof newer than the growth, revalidation advancing the proof, removals, and a log flooded with 80 registrations falling back to the full check. The stress readers also keep a range proof on each round's guard and fail if it is accepted after the guard is published. Mutants: not logging the ranges, and stamping entries one growth too early, each fail 3/3 runs (about 4,500 stale acceptances).

## Mapping proofs (`cff455dfe`)

- **What:** `GuestMemory::CheckRangeProved(proof, pointer, bytes, alignment)` always checks the null, alignment and overflow rules, then skips the mapping query if the proof holds for that exact range at the mapping epoch. Draw recipes keep `mappingProofs` (8 slots, same layout as `guardProofs`) for the index range and vertex copy ranges, used on draw-cache hits only.
- **Epoch:** `MappingEpoch()` = allocation generation + page-cache forget serial + page access serial. All three only grow, so equal sums mean all three are unchanged. This is the same set of counters the page-state cache already trusts: a registry mutation advances the generation when it ends and then forgets the changed pages (two forget-serial advances around the forget), and the guest arena's access observer advances the access serial.
- **Fix to that cache:** `ForgetInaccessiblePages` advanced the access serial only before forgetting. A check reading the serial in between could keep a proof of the stale pages, so it now advances it again afterwards (as `ForgetPages` already did).
- **When nothing is recorded** (the range is checked every time):
  - writable checks (the API is read-only);
  - ranges not inside one page-cached span (the guest arena or the main image);
  - processes where the cache's invalidation observers were never installed;
  - the caller's own stack, which `verify` accepts per thread.
- **Tests:** `CheckMappingProof` uses a 128 KiB block in the test executable's image, which is page-cached on Linux (the check is compiled out on Windows, and reports a skip if the image is not cached). It covers: a proof at the epoch; misalignment still refused; held proofs writing nothing in the driver image; a protection change to no access invalidating the proof and the next check refusing the range; a readable range proven again; a stack range never proven; and removal invalidating the proof. Mutants: a constant epoch and recording outside the cached spans each fail.
- **Other tests:** 16 tests pass under validation, with the same 39 device-creation VUIDs (3 per test; `data/ctest_log_mapping_validation.log`). `agc_write_tracking` and `agc_write_watch_coverage` pass (`data/ctest_guestmemory.log`). The page-state tests are Windows-only and were not run.
- **Not measured:** the cost of the skipped query in the driver. On Linux a small uncached range is read with `process_vm_readv` (a system call); a cached range is a page-table scan.

## Bug found by the range proofs (`7d96fe73e`)

The stress readers' new range-proof check (added with the registration log) accepted a stale proof after a registration in 4 of 25 runs. Splitting the stale counter showed it was always that check. The cause was older than the log:
- `GuardOneFragmentFmask` set `anyGuarded` only after its `ResidentGuardRegistration` was destroyed, which is the finishing growth advance.
- With no other guard resident (the stress test clears every 7 rounds), a check could load the final growth, still read `anyGuarded == false`, take `RequireNoMultisampleTarget`'s early return for an unguarded process, and record a proof stamped at the final growth that outlived the registration.
- This affected `requireGuestBuffers` proofs since `9a7128ddb` and recipe range proofs since `876e52aca`.

The fix sets the flag before the registration finishes, as `guard()` and `DepthSurfaceView` already did. The general rule for the proofs: every state a check reads must be published before the registration's finishing growth advance.

After the fix the guard test passed 0-fail in 40, 20 and 30 consecutive runs. The 18-test validation suite (the 16 above plus `agc_write_tracking` and `agc_write_watch_coverage`) passes on `7d96fe73e` with the same 39 device-creation VUIDs (`data/ctest_final_validation.log`). The earlier test logs for `9a7128ddb`, `876e52aca` and `cff455dfe` passed because their stress tests did not include the check that exposes this.

## Further improvements (not done)

1. Done in `fb7ca46f3` (registration log).
2. **Owners for the remaining sites.** Non-recipe draws, DMA and the generic `GuestMemory::Read` → `GuardRead` path have none. If decoded command buffers or descriptor fetches are cached anywhere, proofs could live there.
3. Done in `cff455dfe` (mapping proofs), for draw-cache hits.
4. **Formal check of the seqlocks** (snapshot and range proof) with a C11 model checker such as GenMC: x86 and TSan cannot catch a misplaced fence.
5. **Range proof in the TSan model,** so the no-claim variant fails every run, not 1 in 5.
6. **Windows and the GTA trace:** still the deciding gaps (see "Not verified").

## Impact of the remaining improvements

These are estimates, not measurements. Per-check costs come from this machine's benchmarks: a snapshot lookup is about 10–60 ns depending on threads and guards, and an owner-held proof about 1 ns. The per-frame counts are assumptions for scale, not GTA numbers.

| Improvement | Saves | Likely impact |
|---|---|---|
| Proofs for draws without a recipe (first draws, cache misses) | about 30–110 ns per draw (3 lookups) | Depends on the draw-cache hit rate: near zero if it is high; about 0.15–0.5 ms per frame at an extreme 5,000 uncached draws. |
| Proofs for DMA checks | 10–60 ns per DMA packet | Negligible (a few hundred packets is about 10 µs per frame), and it needs a new cache because nothing outlives a packet. |
| Proofs on the generic `GuestMemory::Read` → `GuardRead` path | 10–60 ns per driver read of guest memory | The biggest unknown: about 0.1–0.6 ms per frame at 10,000 reads (up to about 4% of a 60 fps frame). There is no single owner, so it means per-caller work on the hottest callers; `GuestMemory::Read` already counts callers (`CountReadCaller`), so a GTA run shows which ones matter. |
| Paged bitmap instead of the sorted array | a few ns per lookup at 12 guards | Negligible at GTA-like guard counts; only matters with hundreds of guards. |
| Merging the miss scan into the exact path | one lock per overlapping read | Negligible: overlapping reads throw or retire the guard, so they are rare. |
| Formal check of the seqlocks (GenMC) | none | Confidence only. Releases are x86-64 (TSO), so the remaining risk is mainly compiler reordering. |
| Range proof in the TSan model | none | Test quality: the no-claim variant would fail every run instead of 1 in 5. |
| Windows run | — | High value: nothing here has run on Windows, and its `shared_mutex` goes through a global spinlock, so main's cost and the gains are probably larger there. |
| GTA trace | — | Highest value: it replaces every estimate in this table with a measurement. |

Recommended next steps, before more code:
1. Run the guard and recipe tests and the benchmark on Windows.
2. Capture a GTA trace on main and on this branch, from the same machine and scene, summarised with `tools/trace_digest.py`. Per frame it should count:
   - draws with and without a recipe;
   - guest-memory reads per caller;
   - DMA packets;
   - registrations per second.

If guest-memory reads turn out to be large, proofs on the top few callers are the one remaining change likely to matter.

## Proofs for the top `GuestMemory::Read` callers: for and against

**For:**
- **Volume.** `GuestMemory::Read` runs the guard check on every driver read of guest memory; at thousands of reads per frame and 10–60 ns each, that could be about 0.1–0.6 ms per frame (estimate).
- **Proven pattern.** An owner-held proof costs about 1 ns, writes no shared memory, and already works for shader buffers and recipe draws, with exact ranges invalidated by the growth counter and the registration log.
- **Safe by construction.** A proof is recorded only after a range was proven clear, so the refusing and retiring paths always run in full.
- **Cheap to target.** `Read` already counts callers (`CountReadCaller`), so a trace names the hot ones.

**Against:**
- **Unknown benefit.** Without a trace, the read count and how often callers repeat an exact range are guesses; if ranges are fresh each time, proofs never hold and add a little cost.
- **Only part of each read.** `Read` also calls the GPU-write flush hook, the timing counter, the caller counter and the copy; for larger reads the copy dominates.
- **No single place for it.** `Read` is generic: every hot caller needs its own long-lived owner, an exact key, and its own concurrency and invalidation reasoning.
- **Correctness risk per site.** The FMASK residency bug was an ordering subtlety that survived several test runs before a new stress check caught it; each new proof site needs its own stress test and planted-bug check.
- **Untested on Windows.**
- **Simpler alternatives.** At some callers, one check for a batch of contiguous reads, or moving the check out of a loop, needs no proof state at all.

**Recommendation:** take the trace first. Add a proof only at a caller that is hot, repeats exact ranges, and already has a long-lived owner; prefer batching where reads are contiguous; otherwise leave `Read` as it is.

## Registration races (`5293a5cc7`)

The FMASK bug showed that a check could read state published after a registration finished. The stress test covered only FMASK registrations, so the other two sites now have race tests that start with nothing resident.

- **Full color target** (`GuardSnapshot.cpp`, inside `CheckTargets`, lavapipe):
  - 8000 rounds. Each round clears the device's targets, then 3 readers check the target's range with a range proof and a key proof while the main thread acquires the target. A proof or snapshot accepted after publication fails.
  - Overlapping reads of a host-only target refuse without side effects, so the readers drive the real `RequireNoMultisampleTargetProved` path, early-return flags included.
  - Planted bug (flag set after the registration finishes): with 60 rounds, 0/5 runs failed; with 1000, 0/5; with 8000, 3/4 (about 5 s per run). The window is a single atomic operation wide, so 8000 rounds is the setting kept.
- **Native depth** (`NativeMultisampleDepth.cpp`, `CheckRegistrationRace`, D32 with 4 samples):
  - 400 rounds. Overlapping reads of an unwritten depth surface retire the guard, which would mask a stale proof, so the readers use side-effect-free classification with a key proof.
  - They also check the invariant directly: a check that loaded the registration's final growth must never see the resident-depth flag false.
  - Planted bug (flag set after the registration finishes): 3/4 runs fail, with "saw no resident depth guard in 1 (or 2) rounds".
- **Spanning reads:** at the end of the depth race, two FMASK color guards are registered 2 MiB before and after the depth surface; reads from each into the depth guard across the gap must report both registries. This fails without `b7a439237`.

## Independent review

A separate agent reviewed the code diff without the author's conclusions (full summary in `data/independent_review.md`).
- **Its main finding was a real, high-severity bug that every earlier test missed.** The snapshot lookup returned only the first covered interval's tags, so a read spanning a color guard and a depth guard skipped one registry, and a proof could then hide it. It is fixed in `b7a439237` and covered by the spanning-read test above, which fails without the fix.
- **Everything else it checked was clear:** registration ordering at all three sites, the seqlocks, rebuild and log ABA, proof keys, mapping-epoch invalidation, exceptions and lock order.
- **Low-severity notes:**
  - Finish's exception path advances growth without the writer mutex, which is harmless because it disables the log.
  - A Windows-only mapping path was not audited.
  - Two includes were dropped from `MultisampleTarget.hpp`; both builds pass.
- **The bench and model share the simplified lookup.** `guard_bench.cpp` and `probe_bench.cpp` select the exact path from the first interval's tags only. Their reads never span intervals, so the published numbers are unaffected. `guard_model.cpp` uses a yes/no overlap test, which is correct.

## Windows (MinGW and Wine)

A full Windows cross-build of the libraries and tests was not attempted here (the project's Windows build expects a native MinGW environment, prebuilt FFmpeg and SDL). What was done instead:

- **MinGW compile check** (`data/mingw_syntax_check.log`): Ubuntu's `x86_64-w64-mingw32-g++-posix` (GCC 13), with each file's Linux compile flags. All seven changed sources compile: `MultisampleTarget.cpp`, `DepthSurface.cpp`, `GuestMemory.cpp`, `ShaderResources.cpp`, `Draw.cpp`, `GuardSnapshot.cpp` and `NativeMultisampleDepth.cpp`.
  - Covered: `constinit` atomics, `alignas(64)`, and the TEB thread-slot code.
  - One unrelated header, `prx/libc/include/CpuTopology.hpp` (unchanged from upstream main), uses `PROCESSOR_RELATIONSHIP::EfficiencyClass`, which Ubuntu's older MinGW headers lack; it was shimmed for the check only.
- **Thread-teardown fix under Wine** (`windows_teardown.cpp`, `data/windows_teardown_wine.log`):
  - A static MinGW program reproduces the certificate's teardown with the real `HostThreadSlot` TEB code: a `thread_local` object constructed first reads the slot in its destructor, after the certificate's owner has been destroyed.
  - The original design read a dead certificate in 64 of 64 threads; the fixed design (`1241edd3c`) read the retired sentinel in 64 of 64 threads.
  - A first version used a plain `bool` "alive" marker, which the compiler removed as a dead store in the destructor; it showed no bug, and the atomic marker shows the real one.
- **Benchmark under Wine** (`data/bench_wine_quick.csv`, 4 threads, 12 guards; ns per check net of harness overhead; rate 0, then rate 1000):

| Pattern | main | cert draft | cert fixed | snapshot |
|---|---|---|---|---|
| disjoint-local | 3292 / 4996 | 158 / 125 | 134 / 127 | 22 / 27 |
| disjoint-spread | 3017 / 5864 | 3430 / 2976 | 3174 / 4583 | 34 / 39 |
| cyclic-5gaps | 5758 / 4928 | 4685 / 4223 | 4652 / 5076 | 25 / 22 |
| overlap | 2858 / 4610 | 9134 / 12024 | 6140 / 9059 | 1915 / 1784 |

  Under winpthreads, main is 3–5× its Linux cost; the snapshot stays at 18–39 ns on disjoint reads. The certificate's hits (110–170 ns) are inflated here, because the benchmark's certificate uses plain `thread_local` (emutls on MinGW) while the driver uses a direct TEB slot. Wine is not Windows: the winpthreads lock code is the same, kernel waits differ.
- **Not done:** the Windows-only page-state tests, the Vulkan guard tests on Windows, and a real Windows machine.

## Costs inside the driver

`driver_guard_cost.cpp` links against the real driver (it was built as a temporary execution test and moved here; it is not part of the CMake build). Medians of 3 runs on this machine, in ns per operation (`data/driver_cost_summary.txt`):

| Operation | 1 thread | 4 threads |
|---|---|---|
| `RequireNoMultisampleTarget`, no guard resident | 6.7 | — |
| `RequireNoMultisampleTarget`, 12 guards, snapshot hit | 11.4 | 13.6 |
| `GuardRead`, 12 guards, snapshot hit | 13.0 | 15.1 |
| `RequireNoMultisampleTargetProved`, held proof | 7.4 | 6.1 |
| `ResidentGuardOverlaps`, overlapping a guard (registry scans) | 31 | 755 |
| `ResidentGuardsUnchangedSince`, 8 registrations behind | 10.4 | — |
| `CheckRange`, page-cached, 64 B / 4 KiB | 33.6 / 32.9 | — |
| `CheckRange`, uncached mapping, 64 B / 4 KiB | 284 / 369 | — |
| `CheckRangeProved`, held proof, 64 B / 4 KiB | 11.2 / 12.0 | — |
| `FlushGpuWrites` (no hook work) | 2.5 | — |
| `GuestMemory::Read`, page-cached, 64 B / 4 KiB | 81 / 176 | — |
| `GuestMemory::Read`, uncached mapping, 64 B / 4 KiB | 340 / 456 | — |
| `memcpy`, 64 B / 4 KiB | 5.3 / 123 | — |

- **Guard proofs save little.** A snapshot hit is already 11–15 ns, and a held guard proof is 6–7 ns, so a guard proof saves about 5–7 ns per range. The standalone `probe_bench` estimate (about 1 ns against 10–37 ns) overstated it; the driver adds call overhead and the seqlock read.
- **Mapping proofs save more.** About 22 ns per range on page-cached memory.
- **Within a small `GuestMemory::Read`,** the guard check is about 13 of 81 ns and the mapping check about 34 ns. On uncached memory the mapping check (a `process_vm_readv` system call) dominates at about 280 ns.
- **Overlapping reads under contention are the expensive path** (755 ns with 4 threads, from the registry locks). They are rare by design.
- **The first 4-thread run was wrong.** Every thread wrote to one shared `volatile` sink, which bounced its cache line; the harness now uses per-thread sinks.

## `GuestMemory::Read` caller census

From the source, by grepping direct calls (`data/read_callers.txt`): 76 direct calls of `Read`, `ReadCommitted`, `ReadCommittedUnsynced` and `GuardRead` outside tests, in 18 files. Calls through other wrappers are not covered.

| Class | Calls | Examples | Guard share | Action |
|---|---|---|---|---|
| Large copies (surfaces, textures, DCC, buffer mirrors, shader code) | 38 | `Texture.cpp`, `DccMetadata.cpp`, `GuestBufferMemory.cpp`, `ColorTargetTransfer.cpp`, `Draw.cpp` color and index copies, `ShaderRegistry.cpp` | small: the copy dominates (4 KiB: 176 ns, 123 of it `memcpy`) | none |
| One small read per packet, draw or dispatch | 23 | `Pm4.cpp` indirect arguments, predication, index attributes; `BufferFill.cpp`; `PacketMemory.cpp`; `VulkanDevice.cpp`; `DepthSurface.cpp` HTILE and clear words | about 13 of 60–80 ns | none: no long-lived owner, about 13 ns each |
| Polling while waiting on the GPU | 3 | `Pm4.cpp` WAIT_REG_MEM, semaphore, conditional write | negligible next to the wait | none |
| Per-element loops | 9 | `Draw.cpp` index expansion; `ShaderInputState.cpp` per-entry fallback; `ShaderMemory.cpp` per-word reads | about 13 of 60–80 ns, per element | see below |
| The read APIs themselves | 3 | `GuestMemory.cpp` | — | — |

- **The strongest finding is batching, not proofs.** `Draw.cpp` (about line 1590) expands quad lists and strip patches on the CPU by calling `GuestMemory::Read` once per index (2 or 4 bytes). The index range was already flushed and `CheckRange`d just above.
  - At about 60–80 ns per call, a 10,000-quad draw (40,000 index reads) costs about 3 ms in read overhead (estimate from the measured per-read cost).
  - One guarded read of the whole index range into a local buffer would cut this to roughly the cost of a copy.
  - Implemented in `88142952c` (see "Index expansion batching").
- **Others to look at:**
  - `ShaderMemory.cpp` reads shader memory one word at a time in its pending-write and verification policies, a path that mainly runs during captures.
  - `ShaderInputState.cpp` already reads in one batch and only falls back per entry when the whole range is not accessible.
- **Conclusion:** proofs at `GuestMemory::Read` callers are not worth it. The guard is about 13 ns of a read, and the large-read and per-packet callers have no owner. Batching the per-element loops, starting with index expansion, is the useful change. A GTA trace (`CountReadCaller`) would confirm which of these paths run per frame.

## Index expansion batching (`88142952c`)

- **What changed:** quad lists and triangle strips drawn through emulated barycentric patches expand their indices on the CPU in `prepareDrawInputs` (`Draw.cpp`). Each index was fetched with its own `GuestMemory::Read` of 2 or 4 bytes, which repeats the guard check, the GPU-write flush, the mapping check and the read counters each time. The index range is now read once into a host vector by one `GuestMemory::Read`, which performs the same flush, guard and mapping checks over the whole range. The expansion loop moved unchanged into `ExpandPatchIndices_nid_no_patch` (declared in `Draw.hpp`), which also refuses a source or output too small for the primitives.
- **Behaviour:**
  - The indices read are the same: every index used is below `indexCount`, and the range is `indexCount × indexSize`.
  - Index size is already restricted to 2 or 4 earlier in the function.
  - The only difference is that all indices now come from one copy taken at one moment, instead of being read one at a time.
- **Measured** in the driver (`driver_guard_cost.cpp`, `data/index_expansion_cost.csv`; 10,000 quads of 16-bit indices on page-cached memory, single thread):

| | per draw |
|---|---|
| Old: one `GuestMemory::Read` per index (40,000 reads) | 2.7–4.1 ms |
| New: one read and `ExpandPatchIndices_nid_no_patch` | 45–55 µs |

  About 60× faster, matching the earlier estimate of about 3 ms. How often GTA draws quad lists or strips through this path is unknown without a trace.
- **Tests:**
  - **There was no coverage before.** The existing `quadListTests` in `tests/Graphics.cpp` fails with "incorrect graphics stage count" before reaching the expansion, so nothing covered it.
  - **New unit test:** it tests `ExpandPatchIndices_nid_no_patch` directly against hand-derived expected outputs: quads and strips, first and last provoking vertex, 16- and 32-bit indices (including 70000 and 70009, which do not fit 16 bits), automatic indices, and refusal of a short source and of a short output.
  - **First attempt, dropped:** a test of the draw's index limit, which never reached the expansion for the same reason.
  - **Results:** 19 tests pass under validation, adding `agc_driver_quad_list_index_overflow`, with only the device-creation VUID, 3 per device creation (`data/ctest_expansion_validation.log`).
- **Mistakes along the way:**
  - The first placement of the new function was inside an anonymous namespace, so it was not exported to the test; it now sits next to `DrawRecipes()`.
  - One edit script matched an earlier occurrence of its end marker and duplicated part of `Graphics.cpp`; it was restored from a copy before anything was committed.

## Summary of results

The snapshot is the better design. It is never slower than the certificate in a regime that matters, and it is much faster once a thread reads more than four separate gaps.

| 4 threads, 12 guards, 1,000 reg/s (ns/check) | main | cert draft | cert fixed | snapshot |
|---|---|---|---|---|
| Disjoint, one area per thread | 1065 | 17 | 10 | 17 |
| Disjoint, spread over all gaps | 1018 | 829 | 805 | 46 |
| Cycling over 5 gaps | 973 | 1108 | 852 | 19 |
| Overlapping a guard | 944 | 3392 | 1756 | 427 |

- **Ties:** with one area per thread, both designs cut a check from about 1 µs to under 20 ns.
- **Certificate weakness:** it caches 4 gaps per thread, so a thread that touches more misses on every read and costs as much as main or more. The snapshot stays at 8–58 ns at every guard count up to 256.
- **Overlaps:** fix 3 halves the certificate's cost; the snapshot halves it again.
- **Writer cost:** the snapshot spends 8–35 µs of CPU per registration, against 6–9 µs for the certificate. It is included in the totals and only matters at thousands of registrations per second.
- **Deciding GTA measurement:** per-thread, per-frame certificate hits, misses and snapshot stale reads, plus registrations per second, captured with `tools/trace_digest.py` in the same scene on main and on each design. If threads touch at most 4 gaps between registrations, the two designs are equal; otherwise the snapshot wins.

## Raw data (`data/`)

The repository's `.gitignore` excludes `*.log`, so the `.log` files here were missing from the branch until `docs(agc)` commit `13b8a28d2` force-added them; earlier versions of this README listed them before they were committed. A local agent adding more logs needs `git add -f`.

| File | Contents |
|---|---|
| `bench_table.csv` | Run used for the tables above: idle machine, 200 ms per cell, `table` mode, includes the `null` overhead design. |
| `bench_full.csv` | First full run: 250 ms per cell, every rate from 0 to 5000 for every guard count, no `null` design. Its 4-thread rows are noisier (main about 1.1–1.3 µs gross); use it for the rate sweeps. |
| `model_c_<m>.log`, `model_s_<m>.log` | TSan model runs, 3000 rounds each, for the certificate (`c`) and the snapshot (`s`), mutant `m`; `model_s_4.log` holds the TSan race reports. |
| `ctest_main_validation.log` | Guard tests on main `cf79276fe` (10 tests; no guard test exists there) under `VK_LAYER_KHRONOS_validation`. |
| `ctest_cert_validation.log` | The same tests plus `guard_certificate`, on `2a4339321`. |
| `ctest_snapshot_validation.log` | The same tests plus `guard_snapshot`, on `7133b7d37`. |
| `ctest_combined.log` | The same tests on `edaacd785` (combined shader-buffer check). |
| `ctest_proof_validation.log` | The same tests on `9a7128ddb` (proofs). |
| `driver_cost_run1..3.csv`, `driver_cost_summary.txt` | In-driver cost harness results (`driver_guard_cost.cpp`). |
| `bench_wine_quick.csv` | `guard_bench.cpp` built with MinGW and run under Wine, 4 threads and 12 guards. |
| `windows_teardown_wine.log` | The thread-teardown reproduction (`windows_teardown.cpp`) under Wine. |
| `mingw_syntax_check.log` | MinGW compile check of the changed sources. |
| `read_callers.txt` | The direct `GuestMemory` read calls found by the census. |
| `independent_review.md` | The independent review's findings. |
| `ctest_expansion_validation.log` | 19 tests on `88142952c` (index expansion batching). |
| `index_expansion_cost.csv` | Old and new index expansion cost, 3 runs. |
| `ctest_head_validation.log` | 18 tests on `5293a5cc7` code (after the review fix and with both race tests). |
| `ctest_final_validation.log` | 18 tests on `7d96fe73e` (after the fix). |
| `ctest_log_mapping_validation.log` | 16 tests on `cff455dfe` (registration log and mapping proofs). |
| `ctest_guestmemory.log` | `agc_write_tracking` and `agc_write_watch_coverage` on `cff455dfe`. |
| `log_bench.csv` | `log_bench.cpp` output: draw proofs with and without the registration log under registration load. |
| `ctest_drawproof_validation.log` | 16 tests, including `recorder` and `sample_order`, on `876e52aca`. |
| `probe.csv` | `probe_bench.cpp` output: snapshot lookups against owner-held proofs against a per-thread range cache. |
| `build2.log` … `build12.log`, `cfg2.log` | Incremental builds for the snapshot, the certificate, main and the combined check, and the reconfigure log. |

Validation-error counts in the ctest logs: `VUID-VkDeviceCreateInfo-pNext-pNext` appears 27 times on main and 30 times on each change (3 per test, device creation only). No other VUIDs, and no `was not found but was requested by env var VK_INSTANCE_LAYERS` line.

A first quick run, made while a build used all 4 CPUs, is not in `data/`. At 4 threads and 12 guards, ns/check (harness overhead included) was:

| Pattern, reg/s | main | cert draft | cert fixed | snapshot |
|---|---|---|---|---|
| disjoint-local, 0 | 542 | 145 | 135 | 168 |
| disjoint-spread, 0 | 542 | 531 | 564 | 186 |
| cyclic-5, 0 | 590 | 792 | 795 | 141 |
| overlap, 0 | 663 | 1327 | 736 | 388 |

This run matched the published main figure (557–694 ns) but is CPU-contended, so it is not used for conclusions.

## Environment

- Linux 6.18 cloud container, 4 vCPU, Intel Xeon @ 2.10 GHz.
- g++ 13.3 (`-O2`); clang 18 present but without the TSan runtime.
- Mesa lavapipe (llvmpipe, LLVM 20.1.2) and Ubuntu 24.04 `vulkan-validationlayers`.
- Release build configured with `-DBUILD_TESTING=ON -DANYPS5_AGC_EXECUTION_TESTS=<the guard tests>`.
- No `/dev/udmabuf`, so `native_depth_bda_clear` skips.
