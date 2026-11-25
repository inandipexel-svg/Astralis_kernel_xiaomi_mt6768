# Astralis GPU frequency experiment — initial audit

Branch: `test/gpu-oc`
Base: `main`
Scope of this commit: documentation and static source audit only. No OPP, PLL, regulator, thermal, or GED behavior is changed.

## Current hardware-facing limits in the source

Source: `drivers/misc/mediatek/base/power/mt6768/mtk_gpufreq_core.h`

- The highest declared OPP is 1,000,000 kHz at 95,000 (mV x 100 = 950 mV).
- The lowest declared OPP is 299,000 kHz.
- The OPP table contains 32 entries.
- The regulator ceilings are derived from OPP0: VGPU 950 mV and VSRAM_GPU 1,050 mV.
- The table's frequency/voltage pairs are characterized source values; they are not evidence that arbitrary higher frequencies are stable.

Source: `drivers/misc/mediatek/base/power/mt6768/mtk_gpufreq_core.c`

- The driver reads efuse index 30 and maps it to a segment.
- The default segment ceiling is OPP index 7 (823 MHz); MT6767 maps to index 15, MT6769T to index 2, and MT6769Z to index 0. The runtime segment must be read from the target device; the repository name alone does not establish it.
- The PLL DDS helper accepts a broad mathematical range (187.5 MHz–1.9 GHz), and the post-divider selection can choose /2 above 950 MHz. This is a PLL programming range, not a validated GPU operating range.
- The power table is generated from the declared OPP table and a 900 MHz / 900 mV reference point. An arbitrary frequency above the OPP table is not represented by a characterized power entry.

## High-priority control-path finding

The debug proc interface `/proc/gpufreq/gpufreq_fixed_freq_volt` accepts a caller-supplied frequency and voltage. It checks only the VGPU upper ceiling before invoking the direct fixed-frequency and fixed-voltage helpers. That path does not select a characterized OPP and does not naturally pass through the normal OPP limiter path used by `mt_gpufreq_target()` (thermal, PBM, and battery limit indices).

The same handler parses unsigned variables with `%d` in `sscanf`, which is a format/type mismatch. This needs correction independently of any frequency experiment.

The `gpufreq_opp_freq` path also needs review: the current code applies the thermal/power/PBM limiter before applying the keep-OPP override. The order can allow a pinned OPP to override a limiter. Any future manual OPP selection must be clamped by the active segment and every active protection after the requested index is applied.

## GED / DVFS observations

- GED's normal policy requests OPP indices through the GPUFreq API; its switching interval is 50 ms.
- GED initializes its default upper bound from OPP index 0, while the platform GPUFreq driver separately applies efuse-derived segment bounds.
- Do not extend GED's advertised frequency range independently of the platform OPP table and protection model.

## Work required before changing any frequency table

1. Identify the exact A05 board DTS/DTBO and verify the GPU node, regulator constraints, clocks, and compatible strings used at runtime.
2. Read the target device's efuse-derived GPU segment and actual regulator/clock telemetry. Do not infer the segment from the source tree name.
3. Establish that thermal, PBM, battery over-current, low-battery, and battery-percentage protections are enabled and that every manual control is subordinate to them.
4. Correct the manual proc parser and ensure arbitrary frequency/voltage pairs cannot bypass OPP and protection validation.
5. Verify that the power model accounts for every exposed operating point before GED or thermal policy can request it.
6. Only consider operating points supported by board-level electrical and thermal characterization. A 1.5 GHz PLL value being representable is not evidence that the GPU, interconnect, regulator, or package can operate safely at that point.

## Experiment status

- No frequency or voltage values have been changed.
- No protections have been disabled.
- No test build or device stress test has been run.
- 1.5 GHz remains an unvalidated target, not a safe limit or a committed OPP.
