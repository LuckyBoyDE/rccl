# Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All rights reserved.
HIP_PATH ?= $(wildcard /opt/rocm/hip)
ifeq (,$(HIP_PATH))
HIP_PATH = ../../..
endif
HIPCC = $(HIP_PATH)/bin/hipcc

EXE = ib_test
CXXFLAGS = -g -O3 -Iinclude -I../../src/include -DENABLE_TRACE -DRCCL_IB_TEST -ldl -lnuma

files = $(EXE).cpp utils.cpp ../../src/transport/net_ib.cc ../../src/misc/ibvwrap.cc ../../src/debug.cc

all: $(EXE)

$(EXE): $(files)
	$(HIPCC) $(CXXFLAGS) $^ -o $@
	#scp $(EXE) rocm-framework-3:$(shell pwd)

clean:
	rm -f *.o $(EXE)
