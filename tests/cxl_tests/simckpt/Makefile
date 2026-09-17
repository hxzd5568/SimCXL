# Build helpers for the SimCkptDevice P3 user-space stack and (optionally)
# the simckpt.ko kernel module.
#
#   make            -> build ckptbench (static, runs in the gem5 guest)
#   make module     -> build simckpt.ko (needs the guest 6.12+ kernel headers)

CC      ?= gcc
CFLAGS  ?= -O2 -static -Wall
INCLUDES = -Iinclude -Iengine -I.

LIB_SRC = engine/libckpt.c ckptd.c

all: ckptbench ckptbench_p4

ckptbench: bench/ckptbench.c $(LIB_SRC) engine/libckpt.h ckptd.h include/simckpt_uapi.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench.c $(LIB_SRC)

ckptbench_p4: bench/ckptbench_p4.c $(LIB_SRC) balancer.c \
		engine/libckpt.h ckptd.h balancer.h include/simckpt_uapi.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p4.c $(LIB_SRC) balancer.c

# Host-side unit test: prove the balancer routes a full-size (512 MiB)
# checkpoint without running gem5 (pure C, milliseconds).
balancer_scale: balancer_scale.c balancer.c balancer.h
	$(CC) -O2 -Wall -I. -o $@ balancer_scale.c balancer.c

# Kernel module. The guest kernel is a custom 6.12.0+ image whose headers are
# not shipped in the disk image (only 4.15 headers are present), so this target
# requires a matching KDIR, e.g.:
#   make module KDIR=/path/to/linux-6.12-headers
KDIR ?= /lib/modules/$(shell uname -r)/build

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver modules

clean:
	rm -f ckptbench ckptbench_p4 balancer_scale
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver clean 2>/dev/null || true

.PHONY: all module clean
