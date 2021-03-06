/*************************************************************************
 * Copyright (c) 2015-2020, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#ifndef NCCL_DEVICE_H_
#define NCCL_DEVICE_H_

#include "nccl.h"
#include "rccl_bfloat16.h"
#include "align.h"
#include <stdint.h>

// Convert volatile access to atomic
#if defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
#define LOAD(VAR) __atomic_load_n((VAR), __ATOMIC_SEQ_CST)
#define STORE(DST, SRC) __atomic_store_n((DST), (SRC), __ATOMIC_SEQ_CST)
#else
#define LOAD(VAR) *(VAR)
#define STORE(DST, SRC) *(DST) = (SRC)
#endif

#define NCCL_NUM_FUNCTIONS 5 // SendRecv not included for now
typedef enum { ncclCollBroadcast, ncclCollReduce, ncclCollAllGather, ncclCollReduceScatter, ncclCollAllReduce, ncclCollGather, ncclCollScatter, ncclCollAllToAll, ncclCollAllToAllv, ncclCollSendRecv} ncclFunc_t;
extern const char* ncclFuncStr[NCCL_NUM_FUNCTIONS+4];

#define NCCL_NUM_ALGORITHMS 3 // Tree/Ring/CollNet
#define NCCL_ALGO_TREE 0
#define NCCL_ALGO_RING 1
#define NCCL_ALGO_COLLNET 2
extern const char* ncclAlgoStr[NCCL_NUM_ALGORITHMS];

#define NCCL_NUM_PROTOCOLS 3 // Simple/LL/LL128
#define NCCL_PROTO_LL 0
#define NCCL_PROTO_LL128 1
#define NCCL_PROTO_SIMPLE 2
extern const char* ncclProtoStr[NCCL_NUM_PROTOCOLS];

#define NCCL_MAX_OPS 2048
#define NCCL_STEPS 8

union ncclLLFifoLine {
  /* Flags have to be *after* data, because otherwise, an incomplete receive
     from the network may receive the flag but not the data.
     Note this is assuming that either we receive contiguous chunks of data
     (sockets) or data is written with an atomicity of 8 bytes (IB/RDMA). */
  struct {
    uint32_t data1;
    uint32_t flag1;
    uint32_t data2;
    uint32_t flag2;
  };
  uint64_t v[2];
  int4 i4;
};

#define WARP_SIZE 64
#define MAXCHANNELS 32
#define NCCL_MAX_NTHREADS 256
#define NCCL_LL_MAX_NTHREADS NCCL_MAX_NTHREADS
#define NCCL_LL_LINES_PER_THREAD 8
#ifdef TEST_LL_CLEANUP
#define NCCL_LL_CLEAN_MASK 0x078 // Set to 0x100 to disable cleanup
#define NCCL_LL_FLAG_MAX   0x100
#define NCCL_LL_FLAG(a) ((uint32_t)((a) % NCCL_LL_FLAG_MAX))
#else
#define NCCL_LL_CLEAN_MASK 0x7ffffff8
#define NCCL_LL_FLAG(a) ((uint32_t)(a))
#endif
// Make sure the clean mask will last for at least NCCL_NSTEPS
static_assert(NCCL_LL_CLEAN_MASK % NCCL_STEPS == 0, "Invalid NCCL_LL_CLEAN_MASK value");

#define NCCL_LL128_LINESIZE 64
#define NCCL_LL128_LINEELEMS (NCCL_LL128_LINESIZE/sizeof(uint64_t))
#define NCCL_LL128_DATAELEMS (NCCL_LL128_LINEELEMS-1)

#define NCCL_LL128_MAX_NTHREADS 256
#define NCCL_LL128_ELEMS_PER_THREAD 120

// Receiving from up to 3 sources is more compute intensive than sending
// to 3 dests. Use 70% for reduce and 30% for bcast.
#define NCCL_LL128_SPLIT(nt) ((nt*7/(10*32))*32)

#define NCCL_LL128_SHMEM_ELEMS_PER_THREAD 8
#define NCCL_LL128_SHMEM_SIZE (NCCL_LL128_SHMEM_ELEMS_PER_THREAD*NCCL_LL128_MAX_NTHREADS)

#define NCCL_DIRECT_GPU 0x01
#define NCCL_DIRECT_NIC 0x10

#define MAXBARRIERS 2
#define MAXWARPS (NCCL_MAX_NTHREADS/WARP_SIZE)

struct ncclConnInfo {
  // Regular comm mechanism
  char *buffs[NCCL_NUM_PROTOCOLS]; // Local for recv, remote for send
  uint64_t *tail;     // Local for recv, remote for send
  uint64_t *head;     // Local for send, remote for recv

  int direct;         // Direct communication
  void **ptrExchange; // Pointer exchange for direct communication

  int *fifo;          // Size fifo for proxy

  uint64_t step;      // Keep where we are
  uint64_t llLastCleaning;

  // GPU's HDP_MEM_FLUSH_ADDR: HDP Memory Coherency Flush Control. This register
  // allows software to explicitly initiate a flush read to HDP memory. See more
  // descriptions in primitives.h.
  uint32_t* next_hdp_reg;  // Next GPU in ring (for p2p transport use only)
  uint32_t* curr_hdp_reg;  // Curr GPU in ring (for rdma transport use only)
};

struct ncclConnector {
  int connected;
  struct ncclProxyArgs *proxyAppend;
  struct ncclTransportComm* transportComm;
  void* transportResources; // Host-side resources
  struct ncclConnInfo conn;
  struct ncclComm *comm;
};

struct ncclRing {
  // Shortcuts for userRanks[1] and userRanks[n-1]
  int prev;
  int next;

  // Maps an internal nccl index to user-specified rank order. This is necessary
  // since we need to know how the user expects data to be ordered across
  // devices. Ordered from current device.
  int* userRanks;
  int* devUserRanks;
};


#define NCCL_MAX_TREE_ARITY 3
struct ncclTree {
  int depth;
  int up;
  int down[NCCL_MAX_TREE_ARITY];
};

struct ncclPeer {
  struct ncclConnector send;
  struct ncclConnector recv;
};

struct ncclDevComm;

#pragma pack(push)  /* push current alignment to stack */
#pragma pack(4)     /* set alignment to 4 bytes boundary */
/* CollectiveArgs + ncclColl are to be a power of two, currently 64 bytes, */
/* to make sure reads to host from the CUDA kernel are aligned. */
/* Make sure to adjust padding at the end of ncclColl. */
struct CollectiveArgs {
  struct ncclDevComm* comm;
  uint64_t opCount;

  // local and remote input, output, and buffer
  const void * sendbuff;
  void * recvbuff;

  // Op-specific fields. Make sure the common part stays the
  // same on all structs of the union
  union {
    struct {
      uint16_t nThreads;
    } common;
    struct {
      uint16_t nThreads;
      uint8_t bid;
      uint8_t nChannels;
      uint32_t root;
      size_t count;
      size_t lastChunkSize;
    } coll;
    struct {
      uint16_t nThreads;
      uint16_t unused;
      int32_t delta;
      size_t sendCount;
      size_t recvCount;
    } p2p;
    struct {
      uint16_t nThreads;
      uint8_t bid;
      uint8_t nChannels;
      size_t count;
      size_t* extra;
    } a2av;
  };
};
struct ncclColl {
  union {
    struct {
      struct CollectiveArgs args;
      uint16_t funcIndex;
      uint16_t nextIndex;
      uint8_t  active;
    };
    int data[0x10];
  };
};
static_assert(sizeof(struct ncclColl) == (0x10*sizeof(int)), "ncclColl must have a pow2 size");

struct ncclChannel {
  union {
    struct {
      struct ncclRing ring;
      struct ncclTree treeUp;
      struct ncclTree treeDn;
      struct ncclTree collTreeUp;
      struct ncclTree collTreeDn;

      int id;

      // Communication structures
      struct ncclPeer* peers;
      struct ncclPeer* devPeers;

      // Operation list for aggregation
      struct ncclColl* collectives;
      size_t* collectivesExtra;
      int collStart;
      int collCount;
      int collFifoHead; // Only used by GPU
      int collFifoTail; // Only used by CPU

      uint32_t* sync;
      uint64_t* barrier;
      uint64_t* barrier_next;
#ifdef ENABLE_PROFILING
      struct timeval tvs;
      uint64_t sizes;
      int active_req;
      uint64_t send_byte;
      uint64_t recv_byte;
      float bw_cumulative;
      int bw_count;
#endif
    };
    int data[0x80];
  };
};
static_assert(sizeof(struct ncclChannel) == 0x80*sizeof(int), "ncclChannel must have a pow2 size");
#pragma pack(pop)   /* restore original alignment from stack */

#ifdef ENABLE_PROFILING
struct ncclProf {
  union {
    struct {
      uint64_t total_cycle;
      uint64_t wait_cycle[MAXCHANNELS];      // total wait cycle
      uint64_t wait_recv_cycle[MAXCHANNELS]; // recv wait cycle
      // primtive cycles
      uint64_t send_cycle;
      uint64_t directSend_cycle;
      uint64_t recv_cycle;
      uint64_t directRecv_cycle;
      uint64_t copySend_cycle;
      uint64_t directCopySend_cycle;
      uint64_t recvCopySend_cycle;
      uint64_t directRecvCopySend_cycle;
      uint64_t recvReduceCopy_cycle;
      uint64_t recvReduceSend_cycle;
      uint64_t recvReduceCopySend_cycle;
      uint64_t directRecvReduceCopySend_cycle;
      // primitive bytes
      uint64_t send_byte;
      uint64_t directSend_byte;
      uint64_t recv_byte;
      uint64_t directRecv_byte;
      uint64_t copySend_byte;
      uint64_t directCopySend_byte;
      uint64_t recvCopySend_byte;
      uint64_t directRecvCopySend_byte;
      uint64_t recvReduceCopy_byte;
      uint64_t recvReduceSend_byte;
      uint64_t recvReduceCopySend_byte;
      uint64_t directRecvReduceCopySend_byte;
    };
    int data[0x80];
  };
};
#endif

#ifdef ENABLE_COLLTRACE
typedef enum {
  ncclCollTraceKernelLaunchType,
  ncclCollTraceCollEndType,
  ncclCollTraceAbortType
} ncclCollTraceDataType_t;

struct ncclCollTrace {
  uint8_t type;
  uint8_t bid;
  int16_t funcIndex;
  uint32_t data_0;
  uint64_t timeStamp;
  uint64_t opCount;
  uint64_t data_1;
};
static_assert(sizeof(struct ncclCollTrace) == 8*sizeof(int), "ncclCollTrace must have a pow2 size");

#define COLLTRACE_NUM_ITEMS 1024
#endif

struct ncclDevComm {
  int rank;
  int nRanks;
  int buffSizes[NCCL_NUM_PROTOCOLS];

  // Flag to ask NCCL kernels to abort
  volatile uint32_t *abortFlag;

  // Channels, device side
  struct ncclChannel* channels;

#ifdef ENABLE_PROFILING
  // Profiling counters
  struct ncclProf* devProf;
#endif

#ifdef ENABLE_COLLTRACE
  struct ncclCollTrace* collTrace;
  uint32_t collTraceHead, *collTraceTail;
  pthread_t collTraceThread;
  bool collTraceExit;
#endif
};

#endif
