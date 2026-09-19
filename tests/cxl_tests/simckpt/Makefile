# Build helpers for the SimCkptDevice P3 user-space stack and (optionally)
# the simckpt.ko kernel module.
#
#   make            -> build ckptbench (static, runs in the gem5 guest)
#   make module     -> build simckpt.ko (needs the guest 6.12+ kernel headers)

CC      ?= gcc
CFLAGS  ?= -O2 -static -Wall
INCLUDES = -Iinclude -Iengine -I.

LIB_SRC = engine/libckpt.c ckptd.c
ENG_SRC = engine/gpu_dma_engine.c engine/sha256.c

all: ckptbench ckptbench_p4 ckptbench_p5 ckptbench_p6 ckptbench_p7 \
	ckptbench_p8 ckptbench_p9 ckptbench_p10 trace_model topology_scale \
	p10_scale

ckptbench: bench/ckptbench.c $(LIB_SRC) engine/libckpt.h ckptd.h include/simckpt_uapi.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench.c $(LIB_SRC)

ckptbench_p4: bench/ckptbench_p4.c $(LIB_SRC) balancer.c \
		engine/libckpt.h ckptd.h balancer.h include/simckpt_uapi.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p4.c $(LIB_SRC) balancer.c

ckptbench_p5: bench/ckptbench_p5.c $(LIB_SRC) \
		engine/libckpt.h ckptd.h include/simckpt_uapi.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p5.c $(LIB_SRC)

ckptbench_p6: bench/ckptbench_p6.c $(LIB_SRC) \
		engine/libckpt.h ckptd.h include/simckpt_uapi.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p6.c $(LIB_SRC)

ckptbench_p7: bench/ckptbench_p7.c $(LIB_SRC) $(ENG_SRC) \
		engine/libckpt.h ckptd.h include/simckpt_uapi.h \
		engine/gpu_dma_engine.h engine/sha256.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p7.c $(LIB_SRC) $(ENG_SRC)

ckptbench_p8: bench/ckptbench_p8.c $(LIB_SRC) engine/gpu_dma_engine.c \
		trace_gen.c engine/libckpt.h ckptd.h include/simckpt_uapi.h \
		engine/gpu_dma_engine.h trace_gen.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p8.c $(LIB_SRC) \
		engine/gpu_dma_engine.c trace_gen.c -lm

ckptbench_p9: bench/ckptbench_p9.c $(LIB_SRC) engine/gpu_dma_engine.c \
		topology.c engine/libckpt.h ckptd.h include/simckpt_uapi.h \
		engine/gpu_dma_engine.h topology.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p9.c $(LIB_SRC) \
		engine/gpu_dma_engine.c topology.c

ckptbench_p10: bench/ckptbench_p10.c $(LIB_SRC) engine/gpu_dma_engine.c \
		topology.c engine/libckpt.h ckptd.h include/simckpt_uapi.h \
		engine/gpu_dma_engine.h topology.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ bench/ckptbench_p10.c $(LIB_SRC) \
		engine/gpu_dma_engine.c topology.c

# Host-side unit test: prove the balancer routes a full-size (512 MiB)
# checkpoint without running gem5 (pure C, milliseconds).
balancer_scale: balancer_scale.c balancer.c balancer.h
	$(CC) -O2 -Wall -I. -o $@ balancer_scale.c balancer.c

# Host-side analytical model replay: parameterized checkpoint trace + model
# at full scale, no gem5 (P8).
trace_model: trace_model.c trace_gen.c trace_gen.h
	$(CC) -O2 -Wall -I. -o $@ trace_model.c trace_gen.c -lm

# Host-side explicit topology + state-machine test at full scale, no gem5 (P9).
topology_scale: topology_scale.c topology.c topology.h ckptd.c ckptd.h
	$(CC) -O2 -Wall -I. -o $@ topology_scale.c topology.c ckptd.c

# Host-side multi-queue batch logic test at full scale, no gem5 (P10).
p10_scale: p10_scale.c
	$(CC) -O2 -Wall -I. -o $@ p10_scale.c

# Kernel module. The guest kernel is a custom 6.12.0+ image whose headers are
# not shipped in the disk image (only 4.15 headers are present), so this target
# requires a matching KDIR, e.g.:
#   make module KDIR=/path/to/linux-6.12-headers
KDIR ?= /lib/modules/$(shell uname -r)/build

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver modules

clean:
	rm -f ckptbench ckptbench_p4 ckptbench_p5 ckptbench_p6 ckptbench_p7 \
		ckptbench_p8 ckptbench_p9 ckptbench_p10 balancer_scale trace_model \
		topology_scale p10_scale
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver clean 2>/dev/null || true

.PHONY: all module clean
