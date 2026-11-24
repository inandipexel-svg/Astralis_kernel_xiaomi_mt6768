.. SPDX-License-Identifier: GPL-2.0

=========================
Combo LRU Reclaim (experimental)
=========================

Status
======

This branch is an experimental workspace based on
``test/spf-419-ci-enabled``.  The first stage records the existing reclaim
path and defines constraints for subsequent policy experiments.  It does not
change the default reclaim policy.

Baseline
========

The target is the Android 4.19 kernel for MT6768 (merlin/merlinx).

The baseline already contains:

* The classic split-LRU implementation in ``mm/vmscan.c``, with separate
  active/inactive and anonymous/file lists.
* The ``workingset`` refault/shadow-entry mechanism in
  ``mm/workingset.c``, which tracks recently evicted page-cache entries and
  detects refaults.
* ``CONFIG_IDLE_PAGE_TRACKING`` and its page-idle implementation.
* ZRAM multi-compression and an optional bounded cold-slot recompression path
  (``CONFIG_ZRAM_MULTI_COMP``, ``CONFIG_ZRAM_AUTO_RECOMP``).

The current builder profile enables the ZRAM multi-compression and automatic
recompression options.  It deliberately leaves ``CONFIG_ZRAM_WRITEBACK``
disabled; this experiment therefore does not rely on evicting ZRAM contents to
a flash-backed block device.

Design constraints
==================

1. Keep the existing split-LRU path as the safe fallback.
2. Keep policy experiments opt-in until they build and pass runtime tests.
3. Do not run multiple independent PADDR samplers against the same
   physical pages.  DAMON_RECLAIM and DAMON_LRU_SORT may run together only
   through the shared PADDR context: they consume one access-observation
   stream while retaining separate DAMOS schemes, quotas, parameters, and
   statistics.  Their LRU actions must preserve the classic list invariants
   and workingset refault history.
4. Treat proactive monitoring/reclaim as a separate layer from ordinary
   page-granularity reclaim.  A monitor-based scheme must have bounded CPU and
   pageout quotas, sensible memory-pressure activation thresholds, and a
   fallback to normal LRU reclaim.
5. Keep ZRAM recompression separate from page-victim selection.  Recompression
   changes the representation/size of compressed swap entries; it does not
   replace the VM's page-reclaim algorithm.

DAMON note
==========

DAMON-based reclaim is a candidate for a separate, opt-in proactive-reclaim
layer, not a replacement for the split-LRU reclaimer.  A 4.19 backport must be
treated as a real subsystem port: the upstream DAMON_RECLAIM series was
developed against a v5.15-rc5 tree and touched 13 files, so copying only the
reclaim module or enabling a Kconfig symbol would be incomplete.

The Hysteris integration keeps DAMON_RECLAIM and DAMON_LRU_SORT as separate
policies but shares one PADDR monitoring context between them.  This is
required because PADDR sampling clears page access state; independent monitor
threads would otherwise interfere with each other's observations.  Policy
updates replace only the schemes owned by that policy, and disabling one
policy does not stop the monitor while the other remains enabled.
  The shared monitor retains exclusive ownership of the global PADDR
sampler while it is active, preventing unrelated DAMON contexts from racing
the same access bits.

The common scheme order is deterministic regardless of built-in init order:
hot-page promotion (``DAMOS_LRU_PRIO``), cold-page demotion
(``DAMOS_LRU_DEPRIO``), then proactive pageout (``DAMOS_PAGEOUT``).

The PADDR LRU-deprioritization path records the file-page workingset marker
when it actually demotes an active page.  Direct DAMON pageout also preserves
the marker for active file pages before entering shrink_page_list().  The
existing workingset shadow-entry and refault implementation remains the
authority for refault-based activation; DAMON does not introduce a parallel
refault heuristic.


Validation
==========

All kernel builds for this branch must run through the dedicated
``NonGKI_Kernel_Build-K419`` repository.  Do not dispatch builds from the
kernel repository.

For every policy change, retain the source commit SHA in the builder
configuration and validate that the builder checked out that exact revision.
Run the build after each isolated integration step.

Runtime comparison should include, at minimum:

* ``pgscan_kswapd``, ``pgscan_direct``, and corresponding ``pgsteal``
  counters.
* Workingset refault, activation, and restore counters.
* Swap-in/swap-out and major-fault activity.
* Direct-reclaim stalls and kswapd CPU time.
* ZRAM stored/compressed bytes and automatic recompression counters.
* Sustained workload latency, CPU use, and thermal behaviour.

A successful compile is necessary but is not evidence of improved reclaim
quality.  Compare policies against the unchanged split-LRU baseline on the
same device and workload before changing defaults.
