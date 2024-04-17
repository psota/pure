// Author: James Psota
// File:   pure_process.h 

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

#ifndef PURE_RUNTIME_PURE_PROCESS_H
#define PURE_RUNTIME_PURE_PROCESS_H

#pragma once

#include "pure/transport/mpi_comm_manager.h"

#include "mpi.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sstream>
#include <thread>

#include "pure/runtime/mpi_comm_split_manager.h"

// #include "pure/3rd_party/pthread/pthread_barrier_apple.h"
#include "pure/common/pure_pipeline.h"
#include "pure/common/pure_user.h"
#include "pure/runtime/pure_comm.h"
#include "pure/support/helpers.h"
#include "pure/transport/bcast_channel.h"
#include "pure/transport/process_channel.h"
#include "pure/transport/recv_bundle_channel.h"
#include "pure/transport/reduce_channel.h"
#include "pure/transport/send_bundle_channel.h"

#define BUNDLE_CONTENTS_DEBUG 1

using std::make_pair;

// forward declaration
class PureThread;
class BundleChannelEndpoint;
class DirectMPIChannel;
class SendFlatBatcher;
class RecvFlatBatcher;
class BatcherManager;
class BundleChannel;
class SendToSelfChannel;
// class MPICommManager;
// class MpiCommSplitManager;

struct PureRankInitDetails {
    int pure_rank = -1;
    int mpi_rank  = -1;
    int cpu_id    = -1;

    PureRankInitDetails(int pure_rank, int mpi_rank, int cpu_id)
        : pure_rank(pure_rank), mpi_rank(mpi_rank), cpu_id(cpu_id){};
};

class PureProcess {

    friend PureThread;

    static const int           mpi_msg_tag_base = 30000;
    static const bundle_size_t default_bundle_size_for_num_msgs_per_bundle =
            -1000;
    static const channel_endpoint_tag_t default_cet_for_mpi_tag_for_bundle = 0;

  private:
// TODO: clean up storage, alignment, false sharing issues
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    alignas(CACHE_LINE_BYTES) std::vector<PureThread*> victim_threads;
    std::mutex victim_threads_mutex;

    std::vector<PurePipeline*> independent_pure_pipelines;
    std::mutex                 independent_pure_pipelines_mutex;
#if PRINT_PROCESS_CHANNEL_STATS
    std::mutex print_ind_pipeline_stats_mutex;
#endif

#endif

    std::vector<PureRankInitDetails>       pure_rank_init_vec;
    std::unordered_map<unsigned, unsigned> rank_to_cpu_id_map;

    std::unordered_multimap<std::string, std::mutex&> application_mutexes;
    std::mutex sender_bundle_channel_init_mutex,
            receiver_bundle_channel_init_mutex;
    std::unordered_map<int, vector<BundleChannel*>> sender_bundle_channels,
            receiver_bundle_channels;

    // for tagged BundleChannels: map of BundleChannel pointers
    std::mutex tagged_sender_bundle_channel_init_mutex,
            tagged_receiver_bundle_channel_init_mutex,
            tagged_reduce_bundle_channel_init_mutex;
    std::unordered_map<int, BundleChannel*> tagged_sender_bundle_channels,
            tagged_receiver_bundle_channels;
    std::unordered_map<int, BcastProcessChannel*>
            tagged_bcast_process_channels_map;
    std::unordered_map<int, ReduceProcessChannel*>
            tagged_reduce_process_channels_map;
    std::unordered_map<BundleChannelEndpointMetadata, ProcessChannel*,
                       BundleChannelEndpointMetadataHasher>
            process_channels;
    std::unordered_map<BundleChannelEndpointMetadata, SendToSelfChannel*,
                       BundleChannelEndpointMetadataHasher>
            send_to_self_channels;

    // allreduce
    // hack to store any type of allred channel. ugh.
    std::unordered_map<ChannelMetadata, void*, ChannelMetadataHasher>
               managed_allreduce_channels;
    std::mutex managed_allreduce_channels_mutex;
    //

    std::vector<DirectMPIChannel*> direct_mpi_channels;
    std::vector<RdmaMPIChannel*>   rdma_mpi_channels;
    std::vector<void*>             process_channel_raw_buffers;

    std::mutex direct_mpi_channels_mutex;
    std::mutex enqueue_batcher_manager_mutex;
    std::mutex dequeue_batcher_manager_mutex;
    std::mutex rdma_mpi_channels_mutex;
    std::mutex application_mutex_mutex;
    std::mutex tagged_bcast_process_channel_init_mutex,
            tagged_reduce_process_channel_init_mutex;

    // data structure that keeps track of all outstanding ProcessChannels for
    // this PureProcess
    std::mutex process_channel_init_mutex;

    int* rank_to_proc_map_array;
    int* pure_rank_to_thread_num_on_node;

    PureThread**     pure_threads;
    char***          argv_for_each_thread;
    int*             thread_return_values;
    SendFlatBatcher* send_flat_batcher;
    RecvFlatBatcher* recv_flat_batcher;

    BatcherManager** enqueue_batcher_managers;
    BatcherManager** dequeue_batcher_managers;
    MPICommManager   mpi_comm_manager;

    PureCommProcessDetail* pure_comm_world_process_detail;
    MpiCommSplitManager    split_mpi_comm_manager;

    // commonly-accessed fields during application runtime
    alignas(CACHE_LINE_BYTES) const int num_threads;
    const int         mpi_size{}; // MPI_COMM_WORLD
    const int         pure_size;
    int               num_threads_this_proc;
    int               mpi_rank{}; // MPI_COMM_WORLD
    std::atomic<int>  num_threads_initialized = 0;
    std::atomic<int>  num_threads_finalized   = 0;
    std::atomic<bool> run_send_flat_batcher   = true;
    std::atomic<bool> run_recv_flat_batcher   = true;

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // used in PureThread::ExhaustRemainingWork. says how many threads have
    // reached the effecitve end of the program.
    std::atomic<int> num_threads_done;
#endif
    // arguments
    int argc_copy;

    // a flag to allow thief to see if it should even try to take a lock to get
    // a victim. optimization for programs that have work stealing enabled but
    // no channels (yet?) that have a collaborative dequeue continuation
    static bool singleton_pure_process_initialized;
    bool        pinning_cpus = false;

    // Synchronization variables
    alignas(CACHE_LINE_BYTES)
            std::atomic<bool> world_pure_comm_process_details_initialized =
                    false;
    char pad0[CACHE_LINE_BYTES - sizeof(std::atomic<bool>)];

    /////////////////////////////////////////////////////////////////////////////////////

    bool UseDirectChannelBatcher(BundleChannelEndpoint* const bce) const;
    void BindBcastChannel(PureThread* pt, BcastChannel* const bc,
                          int root_pure_rank, channel_endpoint_tag_t cet,
                          PureComm* const);
    void BindReduceChannel(PureThread*          calling_pure_thread,
                           ReduceChannel* const rc, int root_pure_rank_in_comm,
                           channel_endpoint_tag_t cet, PureComm* const);
    int
    NumMessagesPerBundle(PureRT::BundleMode /*bundle_mode*/,
                         bundle_size_t user_specified_bundle_sz =
                                 default_bundle_size_for_num_msgs_per_bundle);

    void InitThreadAffinityDetails(const char* filename = nullptr);
    void InitNumThreadsThisProc(const char* /* thread_map_fname */);
    void PrintThreadMap();

    int MPITagForBundle(
            PureRT::BundleMode /*bundle_mode*/, int /*sender_mpi_rank*/,
            channel_endpoint_tag_t cet = default_cet_for_mpi_tag_for_bundle);

    void BindToSendToSelfChannel(BundleChannelEndpoint* bce,
                                 PureRT::EndpointType   endpoint_type,
                                 PureComm* const        pure_comm);
    void BindToProcessChannel(PureThread* const /*pt*/,
                              BundleChannelEndpoint* const /*bce*/,
                              PureRT::EndpointType /*endpoint_type*/,
                              bool /*using_rt_send_buf_hint*/, size_t,
                              PureComm* const, PurePipeline&&);

    void BindToFlatBatcher(PureRT::EndpointType         endpoint_type,
                           BundleChannelEndpoint* const bce);

    void BindToBatcherManager(PureRT::EndpointType         endpoint_type,
                              BundleChannelEndpoint* const bce,
                              int                          mate_mpi_rank);

    void BindToDirectMPIChannel(PureRT::EndpointType /*et*/,
                                BundleChannelEndpoint* const /*bce*/,
                                int /*mate_mpi_rank*/,
                                bool /* using_rt_send_buf_hint */,
                                bool /* using_sized_enqueue_int*/,
                                PureComm* const, PurePipeline&&);

    void BindToRdmaMPIChannel(PureRT::EndpointType         endpoint_type,
                              BundleChannelEndpoint* const bce,
                              int partner_mpi_rank, PurePipeline&&);

    void BindToTaggedBundleChannel(BundleChannelEndpoint* const /*bce*/,
                                   int /*mate_mpi_rank*/,
                                   PureRT::EndpointType /*endpoint_type*/,
                                   channel_endpoint_tag_t /*cet*/,
                                   bundle_size_t /*user_specified_bundle_sz*/,
                                   int /*thread_num_in_mpi_process*/);

    void BindToBundleChannel(BundleChannelEndpoint* const /*bce*/,
                             int /*mate_mpi_rank*/,
                             PureRT::EndpointType /*endpoint_type*/,
                             PureRT::BundleMode /*unused*/,
                             int /*thread_num_in_mpi_process*/);

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    void RegisterVictimThread(PureThread* const pt);
    void RegisterIndependentPurePipeline(PurePipeline* const pp);

    // debugging only -- probably remove
    void PrintVictimPipelines() const;

    PurePipeline* GetVictimPipeline(PureThread* const) const
            // having issues with this atttribute with gcc
            // __attribute__((always_inline))
            ;
    auto GetNumVictimThreads() const {
        // must be called after AllChannelsInit has been initiated by ALL
        // threads
        return victim_threads.size();
    }
    auto& GetVictimThreads() const { return victim_threads; }

#if PRINT_PROCESS_CHANNEL_STATS
    void PrintIndependentPipelineChunkStats(PurePipeline* pp);
#endif
#endif
    void InitializeThreadNumToCoreMap(int);
    void PopulateHelperThreadToCpuIdVector(
            int               num_helper_threads,
            std::vector<int>& thread_num_on_node_to_cpu_id_map);

    bool constexpr IsProcPerNuma() const {
        return (IS_PURE_PROC_PER_NUMA == 1);
    }

    bool constexpr IsProcPerNumaMilan() const {
        return (IS_PURE_PROC_PER_NUMA_MILAN == 1);
    }

    int constexpr MPIProcsPerNode() const { return IsProcPerNuma() ? 2 : 1; }

    /////////////////////////////////////////////////
  public:
    PureProcess(int argc, char const** argv, int mpi_size, int threads_per_proc,
                int total_threads, const char* thread_map_filename);
    ~PureProcess();
    void PureFinalize();

    inline int GetMpiRank() const { return mpi_rank; }

    int  RunPureThreads();
    void OrigMainTrampoline(PureThread*        pure_thread,
                            std::optional<int> cpu_id_this_thread);
    void ExhaustRemainingWorkDriver(PureThread* pt, int helper_thread_id);

    // void ThreadBreakpoint(int /*thread_id*/, const std::string& /*label*/);

    inline bool PureCommWorldInitialized() const {
        return world_pure_comm_process_details_initialized.load();
    }

    inline void SetPureCommWorldInitialized() {
        world_pure_comm_process_details_initialized.store(true);
    }

    inline PureCommProcessDetail* GetPureCommWorldProcessDetail() {
        assert(pure_comm_world_process_detail != nullptr);
        return pure_comm_world_process_detail;
    }

    inline void SetPureCommWorldProcessDetail(PureCommProcessDetail* pd) {
        pure_comm_world_process_detail = pd;
    }

    inline bool IsMultiProcess() const { return (mpi_size > 1); }

    inline int ThreadNumOnNodeFromPureRank(int pure_thread_rank) {
        check(pure_thread_rank >= 0 && pure_thread_rank < pure_size,
              "pure_thread_rank (%d) must be less than pure_size (%d)",
              pure_thread_rank, pure_size);
        const auto thread_num =
                pure_rank_to_thread_num_on_node[pure_thread_rank];

#if DEBUG_CHECK
        const int threads_per_node = num_threads * MPIProcsPerNode();
        check(thread_num >= 0 && thread_num < threads_per_node,
              "Thread num is %d, but expected it to be both positive and less "
              "than the number of threads per node (%d)",
              thread_num, threads_per_node);
#endif

        return thread_num;
    }

    inline int MPIRankFromThreadRank(int pure_thread_rank) {
        check(pure_thread_rank >= 0 && pure_thread_rank < pure_size,
              "pure_thread_rank (%d) must be less than pure_size (%d)",
              pure_thread_rank, pure_size);
        return rank_to_proc_map_array[pure_thread_rank];
    }

    void BindChannelEndpoint(BundleChannelEndpoint* const /*bce*/,
                             int /*mate_mpi_rank*/,
                             PureRT::EndpointType /*endpoint_type*/,
                             PureRT::BundleMode /*bundle_mode*/,
                             int /*thread_num_in_mpi_process*/,
                             channel_endpoint_tag_t /*channel_endpoint_tag*/,
                             bundle_size_t /*user_specified_bundle_sz*/,
                             bool /*using_rt_send_buf_hint*/,
                             bool /*using_sized_enqueue_hint*/,
                             size_t /*max_buffered_msgs*/,
                             PurePipeline&& /* cont_pipeline */,
                             PureThread* /* pure_thread */, PureComm* const);

    void Barrier(PureThread* pure_thread, bool& thread_barrier_sense, int, int,
                 bool /* do work steal while waiting */, MPI_Comm,
                 PureCommProcessDetail* const);

    inline int GetMaxNumThreadsAnyProcess() const { return num_threads; }

    inline int GetNumThreadsThisProcess() const {
        return num_threads_this_proc;
    }

    inline void MarkOneThreadInitialized() { ++num_threads_initialized; }

    inline bool MainThreadsAllInitialized() const {
        return (num_threads_initialized.load(std::memory_order_acquire) ==
                num_threads_this_proc);
    }

    inline BatcherManager*
    GetEnqueueBatcherManager(int receiver_mpi_rank) const {
        return enqueue_batcher_managers[receiver_mpi_rank];
    }

    inline BatcherManager* GetDequeueBatcherManager(int sender_mpi_rank) const {
        return dequeue_batcher_managers[sender_mpi_rank];
    }

    /////////////////////////////
    // TODO: abstract into a class

    inline bool RunSendFlatBatcher() const {
        return run_send_flat_batcher.load(std::memory_order_acquire);
    }

    inline void StopSendFlatBatcher() {
        run_send_flat_batcher.store(false, std::memory_order_release);
    }

    // recv
    inline bool RunRecvFlatBatcher() const {
        return run_recv_flat_batcher.load(std::memory_order_acquire);
    }

    inline void StopRecvFlatBatcher() {
        run_recv_flat_batcher.store(false, std::memory_order_release);
    }
    /////////////////////////////

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // returns true if all threads are done
    inline bool MarkOneThreadDone() {
        return (++num_threads_done == num_threads_this_proc);
    }

    inline bool AllThreadsDone() {
        return (num_threads_done == num_threads_this_proc);
    }
#endif
    std::mutex& GetApplicationMutex(const std::string /* name */);
};
#endif
