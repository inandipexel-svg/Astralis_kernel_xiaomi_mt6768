# SPDX-License-Identifier: GPL-2.0
#
# DAMON 4.19 backport, limited to physical monitoring and policy modules.

obj-y                           := core.o
obj-$(CONFIG_DAMON_PADDR)       += ops-common.o paddr.o modules-common.o
obj-$(CONFIG_DAMON_RECLAIM)     += reclaim.o
obj-$(CONFIG_DAMON_LRU_SORT)    += lru_sort.o
