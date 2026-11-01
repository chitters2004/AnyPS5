# Independent review of the branch code diff

A separate agent reviewed `git diff origin/draft/agc-guard..draft/guard-snapshot -- core/` at `26c7b2105` (before `b7a439237`), reading only the code, without the author's conclusions. Summary of its report:

## Finding 1: a read spanning several snapshot intervals got only the first interval's tag (high severity, high confidence; fixed in `b7a439237`)

`guardSnapshotOverlaps` binary-searched the first merged interval ending after the read and returned its tags only. Merged intervals are disjoint, so a read can cover several. Example: color guard at [0x1000,0x2000), depth guard at [0x3000,0x4000), read [0x1800,0x3800) → color tag only.
- The depth exact path was skipped: `RequireNoMultisampleTarget` ran only the color path, and `RequireNoResidentDepthRead_nid_no_patch` returned early.
- After the color guard retired (the snapshot keeps it, because removals do not update the snapshot), the next read classified to 0, and `requireGuestBuffers` or a recipe proof recorded a proof that skipped the depth guard until an unrelated registration.
- The reverse layout lost the color guard the same way.

Suggested fix, applied: keep scanning while `begin < end` and OR the tags. A regression test with a color and a depth guard separated by a gap was added in `5293a5cc7` and fails without the fix.

## Areas checked with no issue found

1. **Registration ordering:** every state a check reads is published before the finishing growth advance at all three sites (`guard()`, `GuardOneFragmentFmask` after `7d96fe73e`, `DepthSurfaceView`); all stores of `false` to the resident flags happen under the registry unique lock.
2. **Memory ordering:** the snapshot, the log and `RangeProof` all follow the standard seqlock pattern.
3. **Lost updates and ABA:** the rebuild publishes only if growth is unchanged under the writer mutex; log stamps only grow; a wrapped ring is treated as uncovered. One low-severity note: Finish's exception path advances growth without the writer mutex, which can skew another thread's logged stamp, but it also sets `lost`, which disables revalidation for good.
4. **Proof keys:** `RangeProof` uses the exact range; `guestBuffersProof` uses the allocation count, which is safe because allocations are only appended; moved buffers bypass it; HTILE acceptance cannot record a proof.
5. **Mapping proofs:** the generation advances after each mutation; every `GuestAllocationsInvalidate` caller (Thread.cpp:365, DirectMemory.cpp:1434, GuestHeap.cpp:234) calls it after the change; `GuestArenaSetProtection` notifies after the change; the sum of three monotone counters is sound. Not audited: every Windows `WindowsMappings` path for an access reduction outside a mutation (the same residual risk the page cache already carries).
6. **Exceptions and deadlocks:** the writer mutex is a leaf; the rebuild takes each registry shared lock alone.
7. **Other:** low confidence: `MultisampleTarget.hpp` dropped `<algorithm>` and `<limits>`, so a file that relied on them through the header might not build. The Linux build and the MinGW syntax check of the changed files both pass.
