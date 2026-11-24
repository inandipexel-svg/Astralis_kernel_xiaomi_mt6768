# BORE 7.0.1 semantic backport for Hysteris (Linux 4.19 / CFS)

## Provenance and target

- Upstream reference: `firelzrd/bore-scheduler`
- Upstream patch: `patches/testing/0001-linux7.4-rc4-bore-7.0.1.patch`
- Target: Hysteris kernel, Linux 4.19, CFS
- Work branch: `bore/v7.1` (branch label only; implementation provenance is BORE 7.0.1)
- Platform: Xiaomi merlin / MediaTek MT6768, ARM64

This is a semantic backport, not a literal application of the upstream patch. Upstream 7.0.1 targets Linux 7.4-rc4 and EEVDF. Hysteris uses CFS's `vruntime` and red-black-tree scheduler. EEVDF deadlines, slice protection, and deadline rescaling do not exist in this target and are not copied.

## Implemented in the current branch

### Burst accounting and penalty

- 8-bit fractional `log2p1` burst calculation.
- BORE 7.0.1 defaults: penalty offset 24, scale 1536, maximum penalty `(40 << 8) - 1`.
- 16-bit previous/current/active penalty state; 8-bit score derived from the upper penalty bits.
- Single smoothness control (default 1): rising penalties are smoothed with the upstream rounding behavior; falling penalties recover immediately.
- Kernel threads are excluded from acquiring a burst penalty.
- Effective BORE priority is applied only to CFS virtual-runtime progression through `calc_delta_bore_fair()`. The task's actual `se->load` is not reweighted.

### Inheritance

- Inheritance modes: 0 = disabled for process forks, 1 = direct parent, 2 = ancestor hub (default).
- Thread creation inherits from the thread-group cache.
- Subtree and thread-group caches use a 75 ms default lifetime.
- Cache refreshes sample at most 63 eligible tasks and scan at most 126 list entries.
- The newly forked task and tasks still in `TASK_NEW` are excluded from the sample.
- `CLONE_PARENT` ancestry is accounted for.
- Linux 4.19's child/sibling lists are traversed under `tasklist_lock`; the newer upstream RCU child-list traversal and its fork/exit list changes are deliberately not transplanted.

### CFS sleep and futex behavior

- Sleep credit is recorded when a fair task is dequeued for sleep.
- On wakeup, up to `sched_credit_cap_us` (default 16000 us) is translated into a bounded CFS `vruntime` credit and consumed once.
- Futex waiters are marked around the 4.19 `freezable_schedule()` path. Since CFS has no EEVDF protected-slice state, the CFS adaptation allows a futex waiter that is already ahead in `vruntime` to bypass the ordinary wakeup granularity. It does not grant preemption when the waiter is behind in `vruntime`.

### Isolation from vendor policy

- No BORE dependency on EAS, schedtune, PPM, vendor boost APIs, or proprietary MTK scheduler policy.
- No BORE writes to PELT or task load weights.
- Existing MTK scheduler integrations remain otherwise untouched.

## Files changed for the backport

- `include/linux/sched.h`: widened BORE penalty state while preserving the existing four Android KABI slots; added task-local cache and sleep-credit state.
- `kernel/sched/fair.c`: BORE 7.0.1 penalty/smoothing math, CFS vruntime weighting, bounded wakeup credit, and futex-aware wakeup granularity.
- `kernel/sched/core.c`: bounded direct/ancestor/thread-group inheritance and fork-state initialization.
- `kernel/sysctl.c`: 7.0.1-style controls: `sched_bore`, `sched_burst_inherit_type`, `sched_burst_smoothness`, `sched_burst_penalty_offset`, `sched_burst_penalty_scale`, `sched_burst_cache_lifetime`, and `sched_credit_cap_us`.
- `kernel/futex.c`: marks the futex sleep interval for the CFS wakeup adaptation.
- `init/Kconfig` and `arch/arm64/configs/vendor/mt6768_bore.config`: existing Kconfig option and merlin config fragment remain the build gate.

## Deliberate EEVDF omissions

The upstream `sched_burst_protect_slice_lv`, EEVDF request/deadline rescaling, and deadline-based futex slice bypass have no direct CFS 4.19 representation. The CFS wakeup-granularity adaptation above covers the relevant futex responsiveness intent without inventing EEVDF state or importing modern scheduler internals.

## Validation status

Source integration and static source review have been performed, but this branch has **not yet been compiled**. The builder's merlin fragment enables `CONFIG_SCHED_BORE=y`; its pinned source SHA must be advanced to the completed integration commit before a build. A successful build and runtime boot are still required before this backport can be called validated.
