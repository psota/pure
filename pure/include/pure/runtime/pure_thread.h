// Author: James Psota
// File:   pure_thread.h 

// Copyright (c) 2024 James Psota
// __________________________________________

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
// 
//   http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef PURE_RUNTIME_PURE_THREAD_H
#define PURE_RUNTIME_PURE_THREAD_H

#pragma once

#include <mpi.h>
#include <pthread.h>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <variant>
#include <vector>

#if DEBUG_CHECK
// verifying no duplicates in pending dequeues (in debug mode only)
#include <algorithm>
#endif

#define REDUCER_INIT_VAL_PLACEHOLDER (-99999.0)

#include "pure/common/pure_pipeline.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/common/pure_user.h"
#include "pure/runtime/mpi_comm_split_manager.h"
#include "pure/support/benchmark_timer.h"
#include "pure/transport/bcast_channel.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/channel_manager.h"
#include "pure/transport/pending_channel_manager.h"
#include <fstream> // note: only used TRACE_COMM is true, but we're including this for simplicity.

// old one -- remove?
#include "pure/transport/all_reduce_channel.h"
// #include "pure/transport/experimental/allreduce_small_payload.h"

#define FIX_NULL_COMM_WORLD(pc) ((pc == nullptr) ? pure_comm_world : pc)

// forward declarations
class PureProcess;
class SendChannel;
class RecvChannel;
class ReduceChannel;
class BcastChannel;
class BundleChannelEndpoint;
class BatcherManager;
class PureComm;
class MpiCommSplitManager;

template <typename, unsigned int>
class AllReduceSmallPayloadProcess;

class PureThread {

  public:
    PureThread(PureProcess* /*parent_pure_process_arg*/, int /*pure_rank*/,
               int /*mpi_process_rank_arg*/, int /*num_mpi_processes_arg*/,
               int /*thread_num_in_mpi_process_arg*/, int /* total_threads*/,
               int /* max_threads_per_process */, int /* cpu_id */,
               int /*orig_argc_arg*/, char** /*orig_argv_arg*/);
    ~PureThread();
    void Finalize();

    int        Get_rank(PureComm const* const pc = nullptr) const;
    inline int GetMpiRank() const { return mpi_process_rank; }

    inline int Get_size() const { return total_threads; }
    inline int GetThreadNumInProcess() const {
        return thread_num_in_mpi_process;
    }
    int MPIRankFromPureRank(int pure_rank);

    // WARNING: currently only works in pure comm world
    int ThreadNumOnNodeFromPureRank(int pure_rank);

    inline void GetThreadsPerProcess() const {
        sentinel("DEPRECATED function. Use GetMaxThreadsPerProcess.");
    }
    inline int GetMaxThreadsPerProcess() const {
        return max_threads_per_process;
    }
    int GetNumThreadsThisProcess() const;

    inline int    GetArgc() const { return orig_argc; }
    inline char** GetArgv() const { return orig_argv; }
    std::string   ToString() const;

    // inline void StoreCpuId() {
    //     // this should only be called AFTER this thread has been pinned to
    //     the
    //     // CPU
    //     cpu_id = get_cpu_on_linux();
    // }
    inline int GetCpuId() const {
        // this should only be called AFTER this thread has been pinned to the
        // CPU
        return cpu_id;
    }

    SendChannel* InitSendChannel(
            void* const /*buf*/, int /*count*/,
            const MPI_Datatype& /*datatype*/, int /*dest_pure_rank*/,
            int /*tag*/, PureRT::BundleMode bundle_mode = BundleMode::none,
            unsigned int  channel_endpoint_tag = CHANNEL_ENDPOINT_TAG_DEFAULT,
            bundle_size_t user_specified_bundle_sz =
                    USER_SPECIFIED_BUNDLE_SZ_DEFAULT,
            bool   using_rt_send_buf_hint   = true,
            bool   using_sized_enqueue_hint = false,
            size_t max_buffered_msgs        = PROCESS_CHANNEL_BUFFERED_MSG_SIZE,
            PureComm* const pure_comm       = nullptr);

    SendChannel* GetOrInitSendChannel(
            int count, const MPI_Datatype datatype, int dest_pure_rank, int tag,
            bool   using_rt_send_buf_hint   = true,
            bool   using_sized_enqueue_hint = false,
            size_t max_buffered_msgs        = PROCESS_CHANNEL_BUFFERED_MSG_SIZE,
            PureComm* const pure_comm       = nullptr);

    RecvChannel* InitRecvChannel(
            int /*count*/, const MPI_Datatype& /*datatype*/,
            int /*sender_pure_rank*/, int /*tag*/,
            bool   using_rt_recv_buf  = true,
            size_t max_buffered_msgs  = PROCESS_CHANNEL_BUFFERED_MSG_SIZE,
            PurePipeline&&  pipeline  = PurePipeline(0),
            PureComm* const pure_comm = nullptr);

    // note: as of Nov 2020 you can't pass in a pipeline using this. Consider
    // pulling it back in if it's really necessary in an application.
    RecvChannel* GetOrInitRecvChannel(
            int count, const MPI_Datatype datatype, int sender_pure_rank,
            int tag, bool using_rt_recv_buf = false,
            size_t max_buffered_msgs  = PROCESS_CHANNEL_BUFFERED_MSG_SIZE,
            PureComm* const pure_comm = nullptr);

    ReduceChannel*
    InitReduceChannel(int count, const MPI_Datatype& datatype,
                      int root_pure_rank_in_comm, MPI_Op op,
                      channel_endpoint_tag_t channel_endpoint_tag,
                      PureComm* const        pure_comm = nullptr);

    BcastChannel* InitBcastChannel(int count, const MPI_Datatype& datatype,
                                   int                    root_pure_rank,
                                   channel_endpoint_tag_t channel_endpoint_tag,
                                   PureComm*              pc);
    BcastChannel*
    GetOrInitBcastChannel(int count, const MPI_Datatype datatype,
                          int                    root_pure_rank_in_pure_comm,
                          channel_endpoint_tag_t channel_endpoint_tag,
                          PureComm*              pc = nullptr);

    // todo templatize!

#if OLD_AR_CHAN
    AllReduceSmallPayloadProcess*
    GetOrInitAllReduceChannel(int count, const MPI_Datatype datatype, MPI_Op op,
                              channel_endpoint_tag_t reduce_cet,
                              channel_endpoint_tag_t bcast_cet,
                              PureComm*              pure_comm);
#endif

    // 2022 chan
    /* 1 double sum: sum_one_double.h
       small payload: small_payload.h
       ow: sum_one_double_iter_v3.h

       => here I think we may want to use small double

       // possibly encapsulate thread seq
    */
    template <typename T,
              unsigned int max_count = MAX_COUNT_SMALL_PAYLOAD_AR_CHAN>
    AllReduceSmallPayloadProcess<T, max_count>*
    GetOrInitAllReduceChannel(int count, const MPI_Datatype datatype, MPI_Op op,
                              channel_endpoint_tag_t reduce_cet,
                              channel_endpoint_tag_t bcast_cet,
                              PureComm* pure_comm, seq_type thread_seq);

    int GetAndIncUniqueCommTag() const;

    PureComm*        PureCommSplit(int color, int key,
                                   PureComm* origin_pure_comm = nullptr);
    inline PureComm* GetPureCommWorld() const { return pure_comm_world; }

    MPI_Comm GetOrCreateMpiCommFromRanks(const std::vector<int>& ranks,
                                         MPI_Comm                origin_comm);

    // Barrier has the same signature and semantics as MPI_Barrier
    // (http://www.mpich.org/static/docs/v3.2/www3/MPI_Barrier.html)
    void BarrierOld(PureComm const* const pure_comm,
                    bool                  do_work_steal_while_waiting = true);
    void Barrier(PureComm* const pure_comm,
                 bool            do_work_steal_while_waiting = true);
    void RegisterCollabPipeline(PurePipeline& p);
    void AllChannelsInit();
#if PURE_PIPELINE_DIRECT_STEAL
    std::vector<PurePipeline*>& GetStealablePipelines();
#endif
    // user application lock manager
    std::mutex& GetApplicationMutex(const std::string /* name */);

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    void          RegisterVictimPipeline(PurePipeline* const pc);
    void          SetActiveVictimPipeline(PurePipeline* const pc);
    void          ClearActiveVictimPipeline();
    PurePipeline* GetActiveVictimPipeline() const;

#if PCV4_STEAL_VICTIM_RANDOM_PROBE
    inline auto GetCurrVictimIdx() const { return curr_victim_idx; }
    inline void SetCurrVictimIdx(unsigned int new_idx) {
        curr_victim_idx = new_idx;
        // printf("[r%d] victim idx:\t%d\n", pure_rank, curr_victim_idx);
    }
#endif

    // ProcessChannel*        const TryToFindStealableProcessChannel()
    // const;
    bool          MaybeExecuteStolenWorkOfRandomReceiver();
    void          ExhaustRemainingWork(bool is_helper_thread = false);
    PurePipeline* GetVictimPipeline()
            /*gcc issue: __attribute__((always_inline)) */;
#endif

    inline std::ofstream& GetTraceCommOutstream() {
        printf("getting trace file %p from rank %d\n", &trace_comm_outstream,
               pure_rank);
        return trace_comm_outstream;
    }

#if DO_PRINT_CONT_DEBUG_INFO
    inline FILE* GetContDebugFile() { return cont_debug_file_handle; }
#endif

#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    void     PrintProcessChannelChunkStats() const;
    void     PrintIndependentPipelineChunkStats(PurePipeline*) const;
    uint64_t ChunkCountForPP(PurePipeline const* const pp) const;

    // work steals by me
    uint64_t stat_num_work_steal_considerations =
            0; // MaybeWorkStealFromRandomReceiver calls
    uint64_t stat_num_work_steal_attempts =
            0; // ExecuteDequeueContinuationUntilDone calls
#endif

#if COLLECT_THREAD_TIMELINE_DETAIL
    // for some reason including process_channel.h to get the using
    // namespace declarations isn't sufficient here
    friend class PurePipeline;
    friend class ProcessChannel;
    // #ifdef PROCESS_CHANNEL_VERSION
    // #if RUNTIME_IS_PURE
    // #if PROCESS_CHANNEL_MAJOR_VERSION == 1
    // #elif PROCESS_CHANNEL_MAJOR_VERSION == 2
    //     friend class process_channel_v2::ProcessChannel;
    // #elif PROCESS_CHANNEL_MAJOR_VERSION == 3
    //     friend class process_channel_v3::ProcessChannel;
    // #elif PROCESS_CHANNEL_MAJOR_VERSION == 4
    //     friend class process_channel_v4::ProcessChannel;
    // #else
    // #error "You must specify a PROCESS_CHANNEL_VERSION of 1, 2x, 3x, 4x
    // in the environment." #endif #endif #endif

    friend class DirectMPIChannel;

  private:
    // Adding a new runtime timer:
    // 1. below
    // 2. pure_thread.cpp constructor: a) initializer list; b) constructor
    // body (add to runtime_timers vector) and add one to the reserve
    // parameter
    BenchmarkTimer pc_deq_wait_end_to_end;
    BenchmarkTimer pc_deq_wait_empty_queue_timer;
    BenchmarkTimer pc_deq_wait_probe_pending_dequeues;
    BenchmarkTimer pc_deq_wait_continuation;
    BenchmarkTimer pp_deq_wait_collab_cont_for_helpers_to_finish;

    BenchmarkTimer pc_enqueue_end_to_end;
    BenchmarkTimer pc_enqueue_wait_this_only;
    BenchmarkTimer pc_memcpy_timer;
    BenchmarkTimer pc_enq_wait_probe_pending_enqueues;
    BenchmarkTimer pc_enq_wait_sr_collab_parent_wrapper;
    BenchmarkTimer pp_wait_collab_continuation_driver;
    BenchmarkTimer pp_execute_cont;
    BenchmarkTimer pp_enq_wait_collab_cont_wait_for_work;
    BenchmarkTimer pp_execute_ind_cont_wrapper;

    BenchmarkTimer dc_mpi_isend;
    BenchmarkTimer dc_mpi_send_wait;
    BenchmarkTimer dc_mpi_irecv;
    BenchmarkTimer dc_mpi_recv_wait;
    BenchmarkTimer dc_deq_wait_continuation;

    BenchmarkTimer exhaust_remaining_work;
    BenchmarkTimer all_channel_init;
#endif

#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION
    std::vector<std::pair<PurePipeline*, uint64_t>> pipeline_chunk_stats;
    void IncrementProcessChannelChunkCountDoneByMe(PurePipeline* const pp,
                                                   unsigned int chunks_done);
#endif

#if PURE_PIPELINE_DIRECT_STEAL
    std::vector<PurePipeline*> my_stealable_pure_pipelines;
    std::vector<PurePipeline*> stealable_pure_pipelines;
    int                        stealable_pipeline_idx = 0;
#endif

#if PCV4_OPTION_OOO_WAIT
    // possibly put this above BenchmarkTimers depending on size
    friend class PendingChannelManager<ProcessChannel>;
    PendingChannelManager<ProcessChannel> pc_enqueue_mgr;
    PendingChannelManager<ProcessChannel> pc_dequeue_mgr;
#endif

  public:
    inline std::vector<BenchmarkTimer*>& RuntimeTimers() {
        return runtime_timers;
    }

    BatcherManager* GetDequeueBatcherManager(int sender_mpi_rank) const;
    BatcherManager* GetEnqueueBatcherManager(int receiver_mpi_rank) const;

    inline void EnqueueFlatBatcherIncompleteSendChan(SendChannel* const sc) {
        flat_batcher_outstanding_scs.push(sc);
    }

    inline SendChannel* DequeueFlatBatcherCompletedSendChan() {
        SendChannel* sc = flat_batcher_outstanding_scs.front();
        flat_batcher_outstanding_scs.pop();
        return sc;
    }

    inline int FlatBatcherNumOutstandingEnqueues() const {
        return flat_batcher_outstanding_scs.size();
    }

    // hack remove
    // void StopRecvFlatBatcher();

  private:
    // easier to just keep this here to be used for generate_stats call
    // (will be empty at first)
    std::vector<BenchmarkTimer*> runtime_timers;
    std::vector<BundleChannelEndpoint*>
            outstanding_channel_endpoints; // for
                                           // deallocation
                                           // purposes
    std::unordered_map<ChannelMetadata, SendChannel*, ChannelMetadataHasher>
            managed_send_channels;
    std::unordered_map<ChannelMetadata, RecvChannel*, ChannelMetadataHasher>
            managed_recv_channels;
    std::unordered_map<ChannelMetadata, BcastChannel*, ChannelMetadataHasher>
                             managed_bcast_channels;
    std::mutex               trace_comm_outstream_dir_mutex;
    std::queue<SendChannel*> flat_batcher_outstanding_scs;

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // TODO: remove this / swap out for something faster such as
    // https://lemire.me/blog/2017/09/18/visiting-all-values-in-an-array-exactly-once-in-random-order/
    std::vector<PureThread*> victim_threads_sorted;
#if PCV4_STEAL_VICTIM_RANDOM_PROBE
    std::mt19937 pc_victim_generator;
#endif
    alignas(CACHE_LINE_BYTES)
            std::atomic<PurePipeline*> pp_with_active_stealable_cont = nullptr;
    char pad__pp_with_active_stealable_cont[CACHE_LINE_BYTES -
                                            sizeof(std::atomic<PurePipeline*>)];
#endif
    std::ofstream trace_comm_outstream;

#if DO_PRINT_CONT_DEBUG_INFO
    FILE* cont_debug_file_handle;
#endif

    PureProcess* parent_pure_process;
    char**       orig_argv;

    PureComm* pure_comm_world; // thread-specific version of
                               // "PURE_COMM_WORLD"

    int mpi_process_rank;
    int num_mpi_processes;
    int max_threads_per_process;
    int thread_num_in_mpi_process; /* Application-defined thread, ranging
                                      from 0
                                      to (threads_per_mpi_process-1) # */
    int pure_rank;                 // global thread rank
    int total_threads;
    int cpu_id = -1;

    int orig_argc;
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
#if PCV4_STEAL_VICTIM_RANDOM_PROBE
    unsigned int curr_victim_idx;
#endif
    bool all_channels_init = false;
#endif
    bool barrier_sense    = false;
    bool did_finalization = false;
    // for error checking to make sure the same SendChannel or RecvChannel
    // is not initialized twice. it's possible I may want to change this
    // from an assertion to actually allow the same one to be returned, in
    // which case the factory function name should be changed from Init* to
    // Get*

#if DEBUG_CHECK
    std::unordered_map<BundleChannelEndpointMetadata, SendChannel*,
                       BundleChannelEndpointMetadataHasher>
            initialized_send_channels;
    std::unordered_map<BundleChannelEndpointMetadata, RecvChannel*,
                       BundleChannelEndpointMetadataHasher>
            initialized_recv_channels;
#endif

    void VerifyUniqueEndpoint(BundleChannelEndpoint*,
                              PureRT::EndpointType /*endpoint_type*/,
                              int /*count*/, const MPI_Datatype& /*datatype*/,
                              int /*sender_pure_rank*/, int /*dest_pure_rank*/,
                              int /*tag*/, PureComm* const);
};
#endif
