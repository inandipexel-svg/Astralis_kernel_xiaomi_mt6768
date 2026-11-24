# Hysteris — Changelog

## Stable Integration 4.19

**Base:** `17.0`  
**Target:** Xiaomi merlin / MediaTek MT6768  
**Kernel:** Linux 4.19.325  
**Architecture:** ARM64

This changelog records the project changes introduced after the `17.0` baseline and carried through `test/integration-4.19`, subsequent integration branches, and `stable/integration-4.19`. It covers the complete 20-commit source-change series, plus this changelog's documentation commits.

## Memory management and reclaim

### Faultaround page aging
- Marked pages prefetched by filemap faultaround as old when inserted into the page cache.
- Updated the faultaround path in `mm/filemap.c` and the page-fault path in `mm/memory.c` so prefetched pages do not immediately receive the same recency treatment as pages directly accessed by the faulting workload.

### GFP_NOIO reclaim behavior
- Avoided futile anonymous-page reclaim for `GFP_NOIO` allocations when swapcache availability is low.
- Documented the related VM behavior in `Documentation/sysctl/vm.txt`.

### Bounded direct reclaim
- Added bounds to direct-reclaim over-scanning in `mm/vmscan.c`.
- Applied reclaim-progress checks across memcg and global reclaim paths, including `shrink_node_memcg()`, `shrink_node()`, and `should_continue_reclaim()`.
- Prevented reclaim from continuing excessive scan work when the expected reclaim progress is not being achieved.

### Inactive LRU rotation
- Added a conditional tail-insertion path in `putback_inactive_pages()` for eligible inactive pages.
- The path applies to pages that are unmapped, clean, not under writeback, unreferenced, and satisfy `is_page_cache_freeable()`.
- Other pages continue through the normal LRU insertion path.
- Preserves reclaimable-page rotation when a page is isolated during writeback completion.

### Timed memory boost
- Added the sysfs interface `/sys/kernel/mm/vmscan/memory_boost` with read/write permissions (0644).
- Writing `1` activates a temporary boost with a five-second expiry; writing `0` disables it.
- The policy applies to global kswapd reclaim and biases scanning toward file pages during the boost window.
- Added a dynamic file-cache reserve based on the greater of 5% of node-present pages or the aggregate high watermark.
- The reserve condition prevents the boost from remaining active when available file cache falls below the configured reserve.

### Memory boost race-safety
- Made reserve-triggered boost cancellation conditional with `cmpxchg()`.
- Prevents a cancellation from clearing a newer boost request issued concurrently.

### Swap-thrashing detection and reclaim policy
- Added swap activity monitoring using the `PSWPIN` and `PSWPOUT` VM event counters.
- Samples aggregate per-CPU counters at an interval of approximately 125 ms, depending on kernel HZ.
- Activates the detector when combined swap activity exceeds 12,000 events per second and keeps it active for a one-second hold period.
- Resets the sampling baseline when observed counters decrease.
- The implementation is conditional on `CONFIG_VM_EVENT_COUNTERS`.
- During detected swap thrashing, caps global kswapd reclaim swappiness at 40 while the dynamic file-cache reserve is available.
- Retains proportional reclaim behavior rather than forcing file-only scanning.
- Does not disable reclaim or alter direct-reclaim and memcg reclaim policy.

### Swap-thrashing telemetry
- Added the read-only sysfs interface `/sys/kernel/mm/vmscan/swap_thrash` (0444).
- Reports `1` while the detector's hold window is active and `0` otherwise.
- Corrected the sysfs output newline formatting.

## Block I/O

### BFQ
- Added BFQ as an optional I/O scheduler for the MT6768 kernel.
- Updated block scheduler Kconfig and Makefile integration.
- Enabled the corresponding configuration option in the MT6768 defconfig.

### ADIOS
- Added the ADIOS I/O scheduler for the multi-queue block layer (`blk-mq`).
- Added the scheduler implementation in `block/adios-iosched.c` and its Kconfig/Makefile integration.

## Scheduler

### schedutil load boost
- Added an optional ondemand-style load-boost mechanism to schedutil.
- The feature is controlled through the scheduler configuration and is not a replacement for the normal schedutil utilization-based frequency selection.

### UTIL_EST decay
- Adjusted fair-scheduler utilization-estimation handling so `util_est` can decay when the estimated utilization is above CPU capacity.
- Removed the previous capacity-related restriction that prevented the decay path from running in that condition.

## TCP and network timeout handling

- Clamped SYN-ACK retry handling to the request-socket timeout range.
- Made request-socket timeout reads use a consistent snapshot.
- Capped TCP retransmission-timeout backoff shifts to prevent shift overflow.
- Limited the TCP `probe0` timeout shift.
- Updated the relevant request-socket and TCP timeout paths in `include/net/` and `net/ipv4/`.

## Timers and clockevents

- Serialized clock-event broadcast-device replacement to protect concurrent updates.
- Updated the clockevents exchange helper documentation.
- Avoided redundant IRQ-save handling when raising the timer softirq.
- Updated the related clockevents, tick-broadcast, timer, and internal tick interfaces.

## zsmalloc

- Changed empty zspage release so it occurs outside `class->lock`.
- Reduced the amount of work performed while holding the zsmalloc class lock while preserving the empty-page release path.

## MT6768 integration and kernel identity

- Updated the MT6768 defconfig kernel identity to `HysterisPlus`.
- Enabled automatic kernel version information in the build configuration.

## Build configuration

- Uses `vendor/mt6768_defconfig` with the `vendor/` configuration fragment directory.
- Configured for ARM64 and the `Image.gz-dtb` output.
- The paired XXKSU builder configuration targets the `stable/integration-4.19` kernel branch.
