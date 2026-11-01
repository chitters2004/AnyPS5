# AGC guard check: certificate vs snapshot

Branch `draft/guard-snapshot`, based on `draft/agc-guard`. This file is for agents and reviewers reading the branch. The `notes-file` rule in `tools/check_conventions.py` rejects it, so drop this commit before opening a pull request and move the text into the PR body.

## Commits

| SHA | Change |
|---|---|
| `2e098c60f` | fix: advance the guard growth before the first insertion. A throw between `push_back`s no longer leaves a range out of every certificate. |
| `1241edd3c` | fix: the certificate lives on the heap and is owned by the TEB/thread slot. A `thread_local` release object deletes it and points the slot at a retired sentinel (growth 0, never matches), so reads during Windows emutls teardown take the exact path instead of touching freed memory. |
| `2a4339321` | perf: classify a miss once (`ResidentGuardOverlaps_nid_no_patch` returns color/depth bits) and run only the overlapping registries' exact paths. An overlapping read drops from 6 scans and 2 exact passes to 2 scans and 1 exact pass. **Pick this commit for the certificate design.** |
| `7133b7d37` | perf: replace the certificate with a process-wide snapshot. |
| `edaacd785` | perf: `ShaderResources::requireGuestBuffers` makes one combined check (`RequireNoMultisampleTarget` gained an optional `htileClear`), not separate depth and color checks. |

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

`guard_model.cpp` (not in the repo) is built with `g++ -fsanitize=thread`. Three readers check the most recently published guard while a writer registers guards (every fifth registration throws between two inserts), removes old ones and triggers rebuilds. The property: a registration that happens-before a check is always seen, unless that guard was later removed.

| Variant | Result (3000 rounds) |
|---|---|
| certificate, clean | 0 violations, TSan clean |
| certificate, no growth bump | violations on almost every check |
| certificate, growth bumped after the `push_back`s with a throw between | 38,401 violations (the fix 1 bug) |
| snapshot, clean | 0 violations, 730 rebuilds, TSan clean |
| snapshot, insertion not added | violations on almost every check |
| snapshot, rebuild without the growth check | 19,466 violations |
| snapshot, plain stores instead of atomics | TSan data race in `publish` |

Limits: on x86 (TSO), TSan does not model fence reordering, so a missing or wrong fence would not be caught. Weak-memory hosts are unverified.

## Driver tests (lavapipe, llvmpipe LLVM 20.1.2)

Run with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 ctest -R <guard tests> -V` over `guard_snapshot` (or `guard_certificate`), `alpha_to_coverage`, `depth_plane_guest_layers`, `native_multisample_depth`, `multisample_retirement`, `depth_storage`, `depth_surface_last_writer`, `sample_multisample_target`, `native_depth_bda_clear` (skips: no udmabuf), `depth_fast_clear_submit` and `graphics`.

- **Results:** all pass on main (`cf79276fe`), on `2a4339321` (certificate with fixes), on `7133b7d37` and on `edaacd785`. The layer loaded in every run.
- **Validation:** the only validation error, in every build, is `VUID-VkDeviceCreateInfo-pNext-pNext` at device creation (the layer is older than the headers), 3 per test. No new VUIDs.
- **New guard tests:** the snapshot test adds the stale-rebuild check, the overflow check (300 guards) and rebuilds running concurrently in the stress test. The hit paths are checked with the driver image's writable segments frozen read-only.
- **Conventions:** `tools/check_conventions.py --base upstream/main` passes on every code commit; this report commit fails `notes-file`.

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
