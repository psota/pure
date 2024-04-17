// Author: James Psota
// File:   pure_process.cpp 

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

#include <algorithm>
#include <boost/preprocessor/stringize.hpp>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <unordered_set>

#include "pure/3rd_party/low-overhead-timers/low_overhead_timers.h"
#include "pure/runtime/batcher_manager.h"
#include "pure/runtime/mpi_comm_split_manager.h"
#include "pure/runtime/pure_process.h"
#include "pure/runtime/pure_thread.h"
#include "pure/transport/bcast_channel.h"
#include "pure/transport/bcast_process_channel.h"
#include "pure/transport/bundle_channel.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/direct_mpi_channel.h"
#include "pure/transport/experimental/recv_flat_batcher.h"
#include "pure/transport/experimental/send_flat_batcher.h"
#include "pure/transport/mpi_comm_manager.h"
#include "pure/transport/mpi_pure_helpers.h"
#include "pure/transport/process_channel.h"
#include "pure/transport/process_channel_v4.h"
#include "pure/transport/rdma_mpi_channel.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/reduce_channel.h"
#include "pure/transport/reduce_process_channel.h"
#include "pure/transport/send_bundle_channel.h"
#include "pure/transport/send_channel.h"

#ifdef LINUX
#include <numa.h>
#endif

static const auto max_chars = 10;

bool PureProcess::singleton_pure_process_initialized = false;
thread_local PureThread* pure_thread_global          = nullptr;

// Exactly one pure process exists per MPI process
PureProcess::PureProcess(int argc, char const** argv, int mpi_size,
                         int threads_per_proc, int total_threads,
                         const char* thread_map_filename)
    : mpi_comm_manager(mpi_size), mpi_size(mpi_size),
      num_threads(threads_per_proc), pure_size(total_threads) {

    // Note that MPI_Init has already been called at this point by
    // Pure::Initialize

    // fprintf(stderr, "num_threads = %d threads_per_proc = %d\n ", num_threads,
    //         threads_per_proc);

    // we only want one PureProcess to be created
    assert(!singleton_pure_process_initialized);
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    num_threads_done.store(0, std::memory_order_relaxed);
#endif
#if MPI_ENABLED
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
#else
    mpi_rank = 0;
#endif

    // set up thread to process mapping
    pure_rank_to_thread_num_on_node = new int[pure_size];
    rank_to_proc_map_array          = new int[pure_size];
    InitThreadAffinityDetails(thread_map_filename);
    InitNumThreadsThisProc(thread_map_filename);

    // fprintf(stderr, "after init threads -- %d num_threads_this_proc\n",
    //         num_threads_this_proc);
    assert(num_threads_this_proc > 0);

    process_channel_raw_buffers.reserve(total_threads); // heuristic
    pure_threads = new PureThread*[num_threads_this_proc];

#if ENABLE_PURE_FLAT_BATCHER
    send_flat_batcher = new SendFlatBatcher(this, mpi_size, mpi_rank,
                                            num_threads_this_proc);
    recv_flat_batcher = new RecvFlatBatcher(this, mpi_size, mpi_rank,
                                            num_threads_this_proc);
#endif

    dequeue_batcher_managers = new BatcherManager*[mpi_size];
    for (auto i = 0; i < mpi_size; ++i) {
        // we rely on this being nullptr to indicate that it hasn't be
        // initialized yet.
        dequeue_batcher_managers[i] = nullptr;
    }
    enqueue_batcher_managers = new BatcherManager*[mpi_size];
    for (auto i = 0; i < mpi_size; ++i) {
        // we rely on this being nullptr to indicate that it hasn't be
        // initialized yet.
        enqueue_batcher_managers[i] = nullptr;
    }

    // create local copy of argv for each thread
    argc_copy            = argc;
    argv_for_each_thread = new char**[num_threads_this_proc];
    for (auto t = 0; t < num_threads_this_proc; ++t) {

        auto this_arg_array = new char*[argc];
        for (auto arg = 0; arg < argc; ++arg) {
            const size_t max_arg_len = 320;
            // binary name with full path could be long; this was a problem with
            // the long path name (argv[0]) for jacobi.
            // this could be especially long now with the long directory names
            // and hashes.
            assert(strlen(argv[arg]) < max_arg_len);
            auto this_arg = new char[max_arg_len];
#ifdef LINUX
            strcpy(this_arg, argv[arg]);
#elif OSX
            strlcpy(this_arg, argv[arg], max_arg_len);
#endif
            this_arg_array[arg] = this_arg;
        }
        argv_for_each_thread[t] = this_arg_array;
    }

    int thread_count = 0;
    check(pure_rank_init_vec.size() == pure_size,
          "Rank init details must be %d in size but is %lu", pure_size,
          pure_rank_init_vec.size());
    for (const auto& rank_init_details : pure_rank_init_vec) {
        // only create the PureThreads that are part of my MPI process
        if (rank_init_details.mpi_rank == mpi_rank) {
            pure_threads[thread_count] =
                    new PureThread(this, rank_init_details.pure_rank, mpi_rank,
                                   mpi_size, thread_count, pure_size,
                                   num_threads, rank_init_details.cpu_id, argc,
                                   argv_for_each_thread[thread_count]);
            ++thread_count;

            // hack debug
            if (mpi_rank == 0) {
                fprintf(stderr, "pure rank %.4d\tmpi_rank %.2d\n",
                        rank_init_details.pure_rank,
                        rank_init_details.mpi_rank);
            }
        }
    }

    PureCommProcessDetail::RegisterPureCommSplitDetailsMpiType();

    if (!IsProcPerNumaMilan()) {
        check(thread_count == num_threads_this_proc,
              "[MPI Rank %d] thread count (%d) needs to equal num threads for "
              "this "
              "process (%d)",
              mpi_rank, thread_count, num_threads_this_proc);
    }

    // This is just for error checking
    singleton_pure_process_initialized = true;
    thread_return_values               = new int[num_threads_this_proc];

    // Debugging prints for rank 0 only
    if (mpi_rank == 0) {
        std::cout << "TOTAL_THREADS: " << total_threads << std::endl;
#if MPI_ENABLED
        printf(KGRN "+++ MPI RUNNING (%d MPI ranks) +++\n" KRESET, mpi_size);
#else
        printf(KYEL "--- NO MPI ---\n" KRESET);
#endif
        std::cout << "threads_per_proc: " << threads_per_proc << std::endl;

        if (thread_map_filename && strlen(thread_map_filename) > 0) {
            printf("THREAD_MAP_FILENAME: %s\n", thread_map_filename);
        } else {
            printf("THREAD_MAP_FILENAME: none (default)\n");
        }
        assert(total_threads <= mpi_size * threads_per_proc);
        check(total_threads > (mpi_size - 1) * threads_per_proc,
              "total_threads (%d) is too small for the given "
              "PURE_NUM_PROCS "
              "(%d) and PURE_RT_NUM_THREADS (%d). It should be at least "
              "%d.",
              total_threads, mpi_size, threads_per_proc,
              ((mpi_size - 1) * threads_per_proc) + 1);

        fprintf(stderr, "PROCESS_CHANNEL_VERSION:\t\t%d\n",
                PROCESS_CHANNEL_VERSION);
        fprintf(stderr, "PROCESS_CHANNEL_BUFFERED_MSG_SIZE:\t%d\n",
                PROCESS_CHANNEL_BUFFERED_MSG_SIZE);
        fprintf(stderr, "BUFFERED_CHAN_MAX_PAYLOAD_BYTES:\t%d\n",
                BUFFERED_CHAN_MAX_PAYLOAD_BYTES);
        fprintf(stderr, "PRINT_PROCESS_CHANNEL_STATS:\t\t%d\n",
                PRINT_PROCESS_CHANNEL_STATS);
        if (USE_RDMA_MPI_CHANNEL) {
            fprintf(stderr, KMAG KBOLD "USE_RDMA_MPI_CHANNEL:\t\t\t%d\n" KRESET,
                    USE_RDMA_MPI_CHANNEL);
        } else {
            fprintf(stderr, "USE_RDMA_MPI_CHANNEL:\t\t\t%d\n",
                    USE_RDMA_MPI_CHANNEL);
        }
        fprintf(stderr, "COLLECT_THREAD_TIMELINE_DETAIL:\t\t%d\n",
                COLLECT_THREAD_TIMELINE_DETAIL);

#ifdef PCV_4_NUM_CONT_CHUNKS
        fprintf(stderr, "PCV_4_NUM_CONT_CHUNKS:\t\t\t%d\n",
                PCV_4_NUM_CONT_CHUNKS);
        fprintf(stderr, "PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL:\t%d\n",
                PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL);
#endif
        fprintf(stderr, "USE_HELPER_THREADS_IF_FREE_CORES:\t%d\n",
                USE_HELPER_THREADS_IF_FREE_CORES);
        fprintf(stderr, "libpure LINK_TYPE:\t\t\t%s\n",
                BOOST_PP_STRINGIZE(LINK_TYPE));
    } // mpi rank 0 -- printing
}

PureProcess::~PureProcess() {
    const size_t num_process_channels = process_channels.size();
    const size_t num_direct_channels  = direct_mpi_channels.size();

    fprintf(stderr,
            KBOLD
            "[PROCESS %d]\tMPI Channels:\t%lu\tProcess Channels: %lu\n" KRESET,
            mpi_rank, num_direct_channels, num_process_channels);

#if PRINT_PROCESS_CHANNEL_STATS
    char        outname[128];
    const char* json_stats_dir = std::getenv("JSON_STATS_DIR");
    sprintf(outname, "%s/channels_p%.4d.csv", json_stats_dir, mpi_rank);

    FILE* outfile = fopen(outname, "w");
    fprintf(outfile, "channel type, mpi rank, endpoint direction (MPI), send "
                     "mpi rank, recv mpi rank, "
                     "pure send rank, pure "
                     "recv rank\n");

    for (auto& direct_mpi_channel : direct_mpi_channels) {
        // printf("  * p%d: DMC %s\n", mpi_rank,
        //        direct_mpi_channel->GetDescription());
        fprintf(outfile, "direct_mpi, %d, %s\n", mpi_rank,
                direct_mpi_channel->GetDescription());
    }

    for (auto& pc_pair : process_channels) {
        const auto pc = pc_pair.second;
        fprintf(outfile, "process, %d, , %d, %d, %d, %d\n", mpi_rank, mpi_rank,
                mpi_rank, pc->GetSenderRank(), pc->GetReceiverRank());
    }

    fclose(outfile);
    if (mpi_rank == 0) {
        printf("See channel stats in %s\n", json_stats_dir);
    }
#endif

    // Report on channel statistics. Let MPI rank 0 be the stats keeper.
    const int reduce_root = 0;
    int       total_process_channels, total_direct_channels;

#if MPI_ENABLED
    MPI_Reduce(&num_process_channels, &total_process_channels, 1, MPI_INT,
               MPI_SUM, reduce_root, MPI_COMM_WORLD);
    MPI_Reduce(&num_direct_channels, &total_direct_channels, 1, MPI_INT,
               MPI_SUM, reduce_root, MPI_COMM_WORLD);
#else
    total_process_channels = num_process_channels;
    total_direct_channels  = num_direct_channels;
    // single process should have only process channels
    assert(total_direct_channels == 0);
#endif

    if (mpi_rank == reduce_root) {
        if (total_direct_channels % 2 != 0) {
            fprintf(stderr,
                    "TEMP -- turn this off -- total_direct_channels (%d) must "
                    "be even (should count both send "
                    "and receive channels)",
                    total_direct_channels);
        }

        // divide by 2 to count total channels (send/recv pairs) for direct
        // channels. The above counts will include one for sender and one for
        // recevier for direct channels.
        total_direct_channels /= 2;

        size_t total_channels = total_process_channels + total_direct_channels;
        if (total_channels > 0) {
            printf("\n***** PROCESS %d CHANNEL STATISTICS RELATIVE TO OTHER "
                   "MPI RANKS *****\n",
                   mpi_rank);
            printf("  Process Channels: %d/%lu (%lf%%)\n",
                   total_process_channels, total_channels,
                   percentage<int>(total_process_channels, total_channels));
            printf("  Direct Channels: %d/%lu (%lf%%)\n\n",
                   total_direct_channels, total_channels,
                   percentage<int>(total_direct_channels, total_channels));
        }
    }

    for (auto t = 0; t < num_threads_this_proc; ++t) {
        char** this_arg_array = argv_for_each_thread[t];
        for (auto arg = 0; arg < argc_copy; ++arg) {
            delete[](this_arg_array[arg]);
        }
        delete[](this_arg_array);
    }
    delete[](argv_for_each_thread);

    // free maps that contain all of the allocated bundle channels. clear calls
    // the underlying BundleChannel destructors.
    for (auto& sender_bundle_channel : sender_bundle_channels) {
        vector<BundleChannel*> vec = sender_bundle_channel.second;
        for (auto& it_vec : vec) {
            delete (it_vec);
        }
    }

    for (auto& receiver_bundle_channel : receiver_bundle_channels) {
        vector<BundleChannel*> vec = receiver_bundle_channel.second;
        for (auto& it_vec : vec) {
            delete (it_vec);
        }
    }

#if PRINT_PROCESS_CHANNEL_STATS
    char fname[64];
    sprintf(fname, "runs/temp_latest/wait_stats_process_%d.csv", mpi_rank);
    FILE* f = fopen(fname, "a");
    assert(f != nullptr);
    fprintf(f, "wait_call_s, wait_call_ns, pending_dequeues_this_thread, "
               "process_channel\n");
    fclose(f);
#endif

#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // work-stealing-related work chunk stats
    fprintf(stderr,
            KBOLD KCYN
            "[process %d]\tDequeue Continuation Chunks Done By Rank By "
            "ProcessChannel -- FIXME:\n" KRESET,
            mpi_rank);

    printf("\n");

    // now, print a table with each channel as rows and chunk completions by
    // column
    printf(KBOLD KGREY "PC\t\t\t   ");
    printf("recv CPU\t");
    printf("steals\t%%\t\t");
    for (auto i = 0; i < num_threads_this_proc; ++i) {
        printf("      rank %d\t", pure_threads[i]->Get_rank());
    }
    printf("\n" KRESET);
    for (const auto& bceh_pc_pair : process_channels) {
        const auto pc = bceh_pc_pair.second;
        if (!pc->HasAnyCollabCont()) {
            continue;
        }
        printf(KGREY "%p\t" KRESET, pc);
        pc->PrintSenderReceiver();
        printf("\t");

        if (pinning_cpus) {
            printf("%d\t", pc->GetReceiverCpu());
        } else {
            printf("?\t");
        }

        uint64_t tot                = 0;
        uint64_t stage_chunk_steals = 0;
        // sum up steals
        for (auto i = 0; i < num_threads_this_proc; ++i) {
            const auto pp         = pc->GetDequeuePurePipeline();
            const auto this_count = pure_threads[i]->ChunkCountForPP(pp);
            if (pure_threads[i]->Get_rank() != pc->GetReceiverRank()) {
                // then it was stolen
                stage_chunk_steals += this_count;
            }
            tot += this_count;
        }
        printf("%" PRIu64 "/%" PRIu64 "  \t", stage_chunk_steals, tot);
        printf("%0.2f%%\t", percentage<uint64_t>(stage_chunk_steals, tot));

        const auto pp = pc->GetDequeuePurePipeline();
        for (auto i = 0; i < num_threads_this_proc; ++i) {
            printf("%*" PRIu64 "\t", max_chars,
                   pure_threads[i]->ChunkCountForPP(pp));
        }
        printf("\n");
    }

    printf("\n");
#endif // PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION

    for (auto& process_channel : process_channels) {
        auto pc = process_channel.second;
        // was allocated with placement new, so just call destructor directly
        pc->~ProcessChannel();
    }
    // printf("process_channel_raw_buffers size upon PureProcess rank %d ending
    // "
    //        "is %lu\n",
    //        mpi_rank, process_channel_raw_buffers.size());
    for (auto& b : process_channel_raw_buffers) {
        free(b);
    }

    for (auto& tagged_sender_bundle_channel : tagged_sender_bundle_channels) {
        delete (tagged_sender_bundle_channel.second);
    }
    for (auto& tagged_receiver_bundle_channel :
         tagged_receiver_bundle_channels) {
        delete (tagged_receiver_bundle_channel.second);
    }
    for (auto& elt : tagged_bcast_process_channels_map) {
        delete (elt.second);
    }
    for (auto& elt : tagged_reduce_process_channels_map) {
        delete (elt.second);
    }

    for (auto& direct_mpi_channel : direct_mpi_channels) {
        delete direct_mpi_channel;
    }

    for (auto& rc : rdma_mpi_channels) {
        delete rc;
    }

#if ENABLE_PURE_FLAT_BATCHER
    delete (send_flat_batcher);
    delete (recv_flat_batcher);
#endif

    for (auto i = 0; i < mpi_size; ++i) {
        if (enqueue_batcher_managers[i] != nullptr) {
            delete enqueue_batcher_managers[i];
        }
    }
    delete[](enqueue_batcher_managers);

    for (auto i = 0; i < mpi_size; ++i) {
        if (dequeue_batcher_managers[i] != nullptr) {
            delete dequeue_batcher_managers[i];
        }
    }
    delete[](dequeue_batcher_managers);

    delete[](thread_return_values);
    delete[](rank_to_proc_map_array);
    delete[](pure_rank_to_thread_num_on_node);

    if (application_mutexes.size() > 0) {
        printf("Process %d allocated %lu application_mutex(es)\n", mpi_rank,
               application_mutexes.size());
    }

    PureCommProcessDetail::DeregisterPureCommSplitDetailsMpiType();

    ////////////////////////////////////////////////////////////
    // note: you must destruct process channels before receive channels, as we
    // rely on
    // receive channels to point to the last buffer that is used by the receive
    // channel attached to a ProcessChannel (note: only applicable for process
    // channel v2)

    for (auto i = 0; i < num_threads_this_proc; ++i) {
        delete (pure_threads[i]);
    }
    delete[] pure_threads;

#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // pretty-ify
    fprintf(stderr, "\n");
#endif

} // ~PureProcess

void PureProcess::PureFinalize() {
    assert(num_threads_finalized.load() >= 0 &&
           num_threads_finalized.load() < num_threads);
    const auto num_threads_finalized_after_increment = ++num_threads_finalized;
    assert(num_threads_finalized_after_increment <= num_threads);

    if (num_threads_finalized_after_increment == num_threads) {
        // ONLY the last thread in does the finalization
        // For now, we just need to finalize the DirectChannelBatchers
        for (auto i = 0; i < mpi_size; ++i) {
            if (enqueue_batcher_managers[i] != nullptr) {
                enqueue_batcher_managers[i]->Finalize();
            }
        }

        for (auto i = 0; i < mpi_size; ++i) {
            if (dequeue_batcher_managers[i] != nullptr) {
                dequeue_batcher_managers[i]->Finalize();
            }
        }

#if ENABLE_PURE_FLAT_BATCHER
        // flat batchers
        send_flat_batcher->Finalize();
        recv_flat_batcher->Finalize();
#endif
    }
}

#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
void PureProcess::PrintIndependentPipelineChunkStats(PurePipeline* pp) {
    assert(pp->IsIndependent());
    std::lock_guard<std::mutex> l(print_ind_pipeline_stats_mutex);
    printf(KBOLD KGREY "Ind. PP (Stage)\trank\tcpu\t  ");
    printf("Steals\t%%\t");
    for (auto i = 0; i < num_threads_this_proc; ++i) {
        printf("    rank %d\t", pure_threads[i]->Get_rank());
    }

    auto     owning_rank        = pp->OwningThread()->Get_rank();
    uint64_t tot                = 0;
    uint64_t stage_chunk_steals = 0;
    // sum up steals
    for (auto i = 0; i < num_threads_this_proc; ++i) {
        const auto this_count = pure_threads[i]->ChunkCountForPP(pp);
        if (pure_threads[i]->Get_rank() != owning_rank) {
            // then it was stolen
            stage_chunk_steals += this_count;
        }
        tot += this_count;
    }

    printf(KGREY "\n%p\t%d\t" KRESET, pp, owning_rank);
    if (pinning_cpus) {
        printf("%d\t", get_cpu_on_linux());
    } else {
        printf("?\t");
    }
    printf("%" PRIu64 "/%" PRIu64 "  \t", stage_chunk_steals, tot);
    printf("%0.2f%%\t", percentage<uint64_t>(stage_chunk_steals, tot));

    for (auto i = 0; i < num_threads_this_proc; ++i) {
        printf("%*" PRIu64 "\t", max_chars,
               pure_threads[i]->ChunkCountForPP(pp));
    }
    printf("\n");
}
#endif

void PureProcess::InitializeThreadNumToCoreMap(int mpi_procs_per_node) {
#if LINUX
    const char*           cpus_for_binding = std::getenv("NUMACTL_CPUS");
    std::vector<unsigned> thread_num_on_node_to_cpu_id_map;

    if (cpus_for_binding && strlen(cpus_for_binding) > 0) {
        pinning_cpus = true;

        // pure_rank_init_vec must already be created at this point
        check(pure_rank_init_vec.size() == pure_size,
              "pure_rank_init_vec should already be created upon entry to "
              "InitializeThreadNumToCoreMap (even if only using the default "
              "thread to mpi proc mapping)");

        std::stringstream ss(cpus_for_binding);
        int               i;

        while (ss >> i) {
            // check(i >= 0 && i < num_threads,
            //       "expected core id (from NUMACTL_CPUS) to be within core "
            //       "bounds (0-%d) but is %d.",
            //       num_threads, i);
            thread_num_on_node_to_cpu_id_map.push_back(i);

            if (ss.peek() == ',' || ss.peek() == ' ') {
                ss.ignore();
            }
        }
        thread_num_on_node_to_cpu_id_map.shrink_to_fit();

        // now loop through the list of cpu ids, placing them sequentially in
        // the pure_rank_init_vec objects
        const int threads_per_node   = num_threads * mpi_procs_per_node;
        int       thread_num_on_node = 0;

        for (auto& pure_rank_init_obj : pure_rank_init_vec) {
            auto this_core_id =
                    thread_num_on_node_to_cpu_id_map[thread_num_on_node];
            check(this_core_id >= 0 || this_core_id < PURE_CORES_PER_NODE,
                  "Expected CPU id from passed in array of CPU ids to be "
                  "between 0 and %d, but it was %d. Passed in NUMACTL_CPUS was "
                  "%s",
                  PURE_CORES_PER_NODE, cpus_for_binding);
            pure_rank_init_obj.cpu_id = this_core_id;

            thread_num_on_node++;
            if (thread_num_on_node == threads_per_node) {
                thread_num_on_node = 0; // reset for next node
            }
        }
    }
#endif
}

void PureProcess::PopulateHelperThreadToCpuIdVector(
        int               num_helper_threads_expected,
        std::vector<int>& thread_num_on_node_to_cpu_id_map) {

    const int invalid_proc = -1;
#if LINUX
    const char* cpus_for_binding = std::getenv("ALL_CPUS_ALTERNATING");

    check(strlen(cpus_for_binding) > 0, "Expected env var ALL_CPUS_ALTERNATING "
                                        "to be set to all CPU ids on this "
                                        "machine for use with helper threads.");

    if (PIN_HELPER_THREADS_IF_NUMA_SCHEME && cpus_for_binding &&
        strlen(cpus_for_binding) > 0) {

        // just go through all of the existing real ranks and
        assert(pure_rank_init_vec.size() == pure_size);
        thread_num_on_node_to_cpu_id_map.reserve(num_helper_threads_expected);

        std::stringstream ss(cpus_for_binding);
        int               i;

        int numa_node_for_this_mpi_rank = -1;
        if (IsProcPerNuma()) {
            // cache this for later use
            // we only want the CPUs for the NUMA node for this MPI rank
            // first, determine it. We'll only use the first of these.
            const auto iter = std::find_if(
                    pure_rank_init_vec.begin(), pure_rank_init_vec.end(),
                    [this](const PureRankInitDetails& rank_details) {
                        return (rank_details.mpi_rank == this->mpi_rank);
                    });

            assert(iter != pure_rank_init_vec.end());
            numa_node_for_this_mpi_rank = numa_node_of_cpu(iter[0].cpu_id);
        }

        while (ss >> i) {
            // is this CPU already allocated for a real pure thread?
            const auto p = std::find_if(
                    pure_rank_init_vec.begin(), pure_rank_init_vec.end(),
                    [i](const PureRankInitDetails& rank_details) {
                        return (rank_details.cpu_id == i);
                    });

            if (p == pure_rank_init_vec.end()) {
                // no match with a CPU id of an existing pure thread
                // cpu id "i" will contain a helper thread
                if (IsProcPerNuma()) {
                    if (numa_node_of_cpu(i) == numa_node_for_this_mpi_rank) {
                        thread_num_on_node_to_cpu_id_map.push_back(i);
                    }
                } else {
                    thread_num_on_node_to_cpu_id_map.push_back(i);
                }
            }

            if (ss.peek() == ',' || ss.peek() == ' ') {
                ss.ignore();
            }
        }

        check(thread_num_on_node_to_cpu_id_map.size() ==
                      num_helper_threads_expected,
              "Expected %d helpers created, but created %d (note: "
              "IsProcPerNuma() is %d; num_threads_this_proc = %d)",
              num_helper_threads_expected,
              thread_num_on_node_to_cpu_id_map.size(), IsProcPerNuma(),
              num_threads_this_proc);
    } else {
        thread_num_on_node_to_cpu_id_map.insert(
                thread_num_on_node_to_cpu_id_map.end(),
                num_helper_threads_expected, invalid_proc);
    }
#else
    // keep consistent with the above
    thread_num_on_node_to_cpu_id_map.insert(
            thread_num_on_node_to_cpu_id_map.end(), num_helper_threads_expected,
            invalid_proc);
#endif
    assert(thread_num_on_node_to_cpu_id_map.size() ==
           num_helper_threads_expected);
}

int PureProcess::RunPureThreads() {

#if PCV4_STEAL_VICTIM_NUMA_AWARE
    if (pinning_cpus == false) {
        fprintf(stderr, KRED "You must be pinning ranks to nodes to do "
                             "NUMA-aware stealing.\n" KRESET);
        std::abort();
    }
#endif

    // give a nice name to the master thread
    char parent_name[16];
    sprintf(parent_name, "PARENT_M_%d", mpi_rank);
    pthread_set_name(parent_name);

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL && \
        USE_HELPER_THREADS_IF_FREE_CORES
    const auto total_cores_on_node = PURE_CORES_PER_NODE;
    const auto max_num_helper_threads =
            IsProcPerNuma()
                    ? ((total_cores_on_node / 2) - num_threads_this_proc)
                    : (total_cores_on_node - num_threads_this_proc);

    // possibly override num helper threads
    int         num_helper_threads;
    const char* requested_max_env = std::getenv("PURE_MAX_HELPER_THREADS");
    if (strlen(requested_max_env) > 0) {
        const int requested_max = atoi(requested_max_env);
        num_helper_threads = std::min(max_num_helper_threads, requested_max);
    } else {
        num_helper_threads = max_num_helper_threads;
    }

    fprintf(stderr,
            KGRN
            "--> Creating %d helper threads on process %d (CONSIDER A LOWER "
            "NUMBER TO LEAVE SPACE FOR THE OS -- or do some smarter "
            "pausing, "
            "waiting, sleeping, etc. as in folly's spinlock)\n" KRESET,
            num_helper_threads, mpi_rank);

    vector<std::thread*> helper_threads;
    vector<PureThread*>  helper_pure_threads;
    helper_threads.reserve(num_helper_threads);
    helper_pure_threads.reserve(num_helper_threads);

    // set up core mapping when using NUMA_SCHEME
    vector<int> thread_num_on_node_to_cpu_id_map;
    PopulateHelperThreadToCpuIdVector(max_num_helper_threads,
                                      thread_num_on_node_to_cpu_id_map);

    // TODO: modify this to truncate number of helper threads (optionally)

    for (auto i = 0; i < num_helper_threads; ++i) {
        // punting on some of these arguments
        // helper threads start at rank -1 and count down from there.
        assert(thread_num_on_node_to_cpu_id_map[i] >= 0);
        fprintf(stderr,
                KRED "[mpi rank %.3d] helper thread %d --> CPU %d\n" KRESET,
                mpi_rank, i, thread_num_on_node_to_cpu_id_map[i]);
        PureThread* pt = new PureThread(
                this, -1 * i - 1, mpi_rank, mpi_size, num_threads_this_proc + i,
                pure_size, num_threads, thread_num_on_node_to_cpu_id_map[i], -1,
                nullptr);
        helper_pure_threads.push_back(pt);
        std::thread* t;

        // extend to this later
        // assert(!pinning_cpus);
        t = new std::thread(&PureProcess::ExhaustRemainingWorkDriver, this, pt,
                            i);
        helper_threads.push_back(t);
    }
#endif
    ///////////////////////////////////////////////////////////////////

    // Kick off the flat batcher worker thread(s)
    // TODO make configs

#if ENABLE_PURE_FLAT_BATCHER
    fprintf(stderr,
            "HACK -- only do one batcher per proc. will need to generalize.\n");

    std::thread* flat_batcher_send_thread;
    std::thread* flat_batcher_recv_thread;
    if (mpi_rank == 0) {
        flat_batcher_send_thread = new std::thread(
                &SendFlatBatcher::BatcherThreadRunner, send_flat_batcher);
    }
    if (mpi_rank == 1) {
        flat_batcher_recv_thread = new std::thread(
                &RecvFlatBatcher::BatcherThreadRunner, recv_flat_batcher);
    }
#endif
    // then, actually create the threads, passing in the desired CPU
    // affinity for that thread, if specified (and linux)

    // psota hack (?)
    if (num_threads_this_proc == 1) {
        // for a single thread per process, we pin CPUs at the process level
        // (on Cori, using srun), not at the thread level. So, we pass no
        // argument for the core id.
        if (mpi_rank == 0) {
            fprintf(stderr, KLT_RED "NOTICE: Not creating any threads from "
                                    "parent process.\n" KRESET);
        }
        OrigMainTrampoline(pure_threads[0], std::nullopt);
    } else {
        vector<std::thread*> threads; // for joining later
        threads.reserve(num_threads_this_proc);
        for (auto i = 0; i < num_threads_this_proc; ++i) {
            PureThread*  pt = pure_threads[i];
            std::thread* t;
            if (pinning_cpus) {
                assert(pt->GetCpuId() != -1);
                int core_id = pt->GetCpuId();
                t = new std::thread(&PureProcess::OrigMainTrampoline, this, pt,
                                    core_id);
            } else {
                t = new std::thread(&PureProcess::OrigMainTrampoline, this, pt,
                                    std::nullopt);
            }
            threads.push_back(t);
        }

        /////////////////////////////////////////////////////////////
        /* Wait on the other threads within this node to finish running
         * (main)
         */
        for (auto t : threads) {
            t->join();
            delete (t);
        }
    }

///////////////////////////////////////////////////////////////////

// shut down the batcher thread
// TODO: config
#if ENABLE_PURE_FLAT_BATCHER
    if (mpi_rank == 0) {
        StopSendFlatBatcher();
        flat_batcher_send_thread->join();
        delete (flat_batcher_send_thread);
    }
    if (mpi_rank == 1) {
        StopRecvFlatBatcher();
        flat_batcher_recv_thread->join();
        delete (flat_batcher_recv_thread);
    }
#endif
    ///////////////////////////////////////////////////////////////////

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL && \
        USE_HELPER_THREADS_IF_FREE_CORES
    for (auto t : helper_threads) {
        t->join();
        delete (t);
    }
    for (auto pt : helper_pure_threads) {
        delete (pt);
    }
#endif
    ///////////////////////////////////////////////////////////////////

    bool error_return_val = false;
    for (auto i = 0; i < num_threads_this_proc; ++i) {
        if (thread_return_values[i] != EXIT_SUCCESS) {
            error_return_val = true;
            fprintf(stderr,
                    KRED "ERROR: exit code of thread %d of process %d was "
                         "%d\n" KRESET,
                    i, mpi_rank, thread_return_values[i]);
        }
    }
    return (error_return_val ? EXIT_FAILURE : EXIT_SUCCESS);

} // function RunPureThreads

// bool constexpr PureProcess::IsProcPerNuma() const {
//     return (IS_PURE_PROC_PER_NUMA == 1);
//     // const char* override_runtime = std::getenv("OVERRIDE_RUNTIME");
//     // const auto  is_proc_per_numa =
//     //         (override_runtime != nullptr) &&
//     //         (std::string(override_runtime)
//     //                  .compare(std::string("PureProcPerNUMA")) == 0);
//     // return is_proc_per_numa;
// }

// int constexpr PureProcess::MPIProcsPerNode() const {
//     return IsProcPerNuma() ? 2 : 1;
// }

void PureProcess::InitThreadAffinityDetails(const char* thread_pos_fname) {

    const auto is_proc_per_numa = IsProcPerNuma();

    // MODE 1: Thread-to-mpi-rank affinity via threadmap file
    // try the custom rank to MPI proc mapping via threadmap file
    if (thread_pos_fname != nullptr) {
        std::ifstream infile(thread_pos_fname);

        // make sure parsed_mpi_rank is monotonically increasing
        int curr_mpi_rank = 0;

        if (infile.good()) {
            std::string line;
            while (std::getline(infile, line)) {
                std::istringstream iss(line);
                int                thread_rank, parsed_mpi_rank;
                char               delim;

                if (!(iss >> thread_rank >> delim >> parsed_mpi_rank) ||
                    (delim != ':')) {
                    sentinel("Invalid thread position input line: %s. Format "
                             "should be <pure_rank>:<mpi_rank>",
                             iss.str().c_str());
                } else {
                    check_always(
                            parsed_mpi_rank < mpi_size && parsed_mpi_rank >= 0,
                            "Parsed MPI rank %d must be less than MPI size "
                            "%d "
                            "and greater than zero",
                            parsed_mpi_rank, mpi_size);
                    check_always(
                            thread_rank < pure_size && thread_rank >= 0,
                            "Parsed thread rank %d must be >=0 and less than "
                            "pure size %d",
                            thread_rank, pure_size);

                    check_always(parsed_mpi_rank >= curr_mpi_rank,
                                 "the threadmap file must have monotonically "
                                 "increasing mpi ranks");
                    if (curr_mpi_rank < parsed_mpi_rank) {
                        curr_mpi_rank = parsed_mpi_rank;
                    }

                    // create the pure_rank_init_vec objects in the same
                    // order as the core id mapping. we'll add those in
                    // later.
                    pure_rank_init_vec.emplace_back(thread_rank,
                                                    parsed_mpi_rank, -1);

                    // fprintf(stderr,
                    //         "pure_rank_init_vec [size %d] <- pure rank %d ->
                    //         " "mpi %d\n", pure_rank_init_vec.size(),
                    //         thread_rank, parsed_mpi_rank);
                }
            }
        } else {
            // have a filename, but can't find file
            sentinel("Can not find thread map file %s. Pass an empty thread "
                     "map file if you want to use default thread mapping.",
                     thread_pos_fname);
        }

    } else {
        // use standard contiguous mapping, accounting for the possiblity
        // of pure_size < mpi_size * num_threads. Equivalent to "mode 1" of
        // Cray MPICH.

        for (auto thr = 0; thr < pure_size; ++thr) {
            const int computed_mpi_rank = thr / num_threads;
            pure_rank_init_vec.emplace_back(thr, computed_mpi_rank, -1);

            // fprintf(stderr,
            //         "init thread map: r%d mapping to mpi rank %d. "
            //         "num_threads=%d\n",
            //         thr, computed_mpi_rank, num_threads);
        }
    }

    // now, initialize a reverse-lookup data structure to get from a pure
    // rank to MPI rank
    const int invalid_proc = -1;
    std::fill_n(rank_to_proc_map_array, pure_size, invalid_proc);
    for (auto const& rank_init_details : pure_rank_init_vec) {
        rank_to_proc_map_array[rank_init_details.pure_rank] =
                rank_init_details.mpi_rank;
    }

    // mostly error checking below here
    // ____________________________________________

    // before returning, verify that all elements have a good value
    check_always(pure_rank_init_vec.size() == pure_size,
                 "pure_rank_init_vec %d but pure size is %d",
                 pure_rank_init_vec.size(), pure_size);
    for (auto i = 0; i < pure_size; ++i) {
        check_always(pure_rank_init_vec[i].mpi_rank >= 0 &&
                             pure_rank_init_vec[i].mpi_rank < mpi_size,
                     "MPI rank %d not proper given that there are %d MPI ranks",
                     pure_rank_init_vec[i].mpi_rank, mpi_size);
        check_always(pure_rank_init_vec[i].pure_rank >= 0 &&
                             pure_rank_init_vec[i].pure_rank < pure_size,
                     "Pure rank %d not proper",
                     pure_rank_init_vec[i].pure_rank);
        assert(rank_to_proc_map_array[i] != invalid_proc);
    }

    // only relevant on linux when pinning CPUs
    const int mpi_procs_per_node = MPIProcsPerNode();
    InitializeThreadNumToCoreMap(mpi_procs_per_node);

    // initialize global (all ranks) rank to thread_num map
    const int threads_per_node   = num_threads * mpi_procs_per_node;
    int       thread_num_on_node = 0;
    std::fill_n(pure_rank_to_thread_num_on_node, pure_size, -2);
    for (const auto& pure_rank_init_obj : pure_rank_init_vec) {
        pure_rank_to_thread_num_on_node[pure_rank_init_obj.pure_rank] =
                thread_num_on_node++;
        if (thread_num_on_node == threads_per_node) {
            thread_num_on_node = 0; // reset for next node
        }
    }
    for (int i = 0; i < pure_size; ++i) {
        assert(pure_rank_to_thread_num_on_node[i] >= 0 ||
               pure_rank_to_thread_num_on_node[i] <
                       std::min(PURE_CORES_PER_NODE, pure_size));
    }

    if (mpi_size >= 2 && is_proc_per_numa) {
#if LINUX
        assert(mpi_size % 2 == 0);
        check_always(pinning_cpus, "PureProcPerNUMA is only supported when "
                                   "NUMA_SCHEME is something other than none.");

        if (mpi_rank == 0) {
            PrintThreadMap(); // debug
        }

        // verify that all ranks in a given mpi process are on the same numa
        // node
        for (auto m = 0; m < mpi_size; ++m) {
            int nn_for_mpi_rank = -1;
            for (const auto& rank_init_details : pure_rank_init_vec) {
                if (rank_init_details.mpi_rank == m) {
                    const auto nn_for_thread =
                            numa_node_of_cpu(rank_init_details.cpu_id);

                    // clean up

                    if (nn_for_mpi_rank == -1) {
                        nn_for_mpi_rank = nn_for_thread;
                    }
                    check_always(
                            nn_for_thread == nn_for_mpi_rank,
                            "expected all threads within an MPI process to "
                            "be on "
                            "the same NUMA node in PureProcPerNUMA runtime "
                            "mode. "
                            "Yet, for rank %d with numa node = %d (CPU %d) "
                            "and "
                            "nn_for_mpi_rank is %d (mpi proc %d). Remember "
                            "that "
                            "if you are running with a threadmap file, you "
                            "need "
                            "to run using a sequential NUMA_SCHEME (e.g., "
                            "bind_sequence, bind_sequence_ht, "
                            "bind_sequence_20_numa_threads); current "
                            "NUMA_SCHEME "
                            "is %s. Also, if you are using Numa nodes and are "
                            "not filling each node, you may have to create a "
                            "special cpu config "
                            "to only use the cpus (in sequence) that you need "
                            "such as bind_sequence_8_numa_threads.",
                            rank_init_details.pure_rank, nn_for_thread,
                            rank_init_details.cpu_id, nn_for_mpi_rank, m,
                            std::getenv("NUMA_SCHEME"));
                }
            }
        }
#else
        sentinel("PureProcPerNUMA mode only supported on LINUX OSes");
#endif
    }
}

void PureProcess::InitNumThreadsThisProc(const char* thread_map_filename) {
    // determine number of threads for this mpi rank. if thread_map_filename
    // is specified, rely on that. Otherwise, use the standard blocking
    // technique with the largest rank potentially having fewer threads than
    // threads_per_proc
    if (thread_map_filename == nullptr) {
        if (mpi_rank == mpi_size - 1 && pure_size % num_threads != 0) {
            // this is the last rank, possibly has fewer threads in the
            // block thread layout approach
            num_threads_this_proc = pure_size % num_threads;
        } else {
            num_threads_this_proc = num_threads;
        }

        // fprintf(stderr,
        //         "m%d -- InitNumThreadsThisProc - num_threads_this_proc = %d "
        //         "num_threads = %d   pure_size = %d\n",
        //         mpi_rank, num_threads_this_proc, num_threads, pure_size);

    } else {

        fprintf(stderr,
                "other path! m%d -- InitNumThreadsThisProc - "
                "num_threads_this_proc = %d   "
                "num_threads = %d   pure_size = %d\n",
                mpi_rank, num_threads_this_proc, num_threads, pure_size);

        // a thread_map_filename was specified
        if (pure_size == mpi_size * num_threads) {
            // fully-packed proc
            num_threads_this_proc = num_threads;
        } else {
            // traverse the map and count how many are here
            num_threads_this_proc = 0;
            for (const auto& rank_init_details : pure_rank_init_vec) {
                if (rank_init_details.mpi_rank == mpi_rank) {
                    ++num_threads_this_proc;
                }
            }
        }
    }
    assert(num_threads_this_proc > 0);
}

void PureProcess::PrintThreadMap() {
    for (const auto& rank_init_details : pure_rank_init_vec) {
        fprintf(stderr,
                "RANK MAPPING: [core %.2d (NU: %d)]:\tpure rank %.3d on "
                "MPI proc %.2d\n",
                rank_init_details.cpu_id,
                pinning_cpus ?
#if LINUX
                             numa_node_of_cpu(rank_init_details.cpu_id)
#else
                             -1
#endif
                             : -1,
                rank_init_details.pure_rank, rank_init_details.mpi_rank);
    }
}

// This trampoline function just does a few things before and after calling
// main.
// N.B. This function is called by EACH of the THREADS spawned above. Not at
// the process level anymore.
void PureProcess::OrigMainTrampoline(PureThread*        pure_thread,
                                     std::optional<int> cpu_id_this_thread) {

    set_pure_thread_name(pure_thread->Get_rank(), pure_thread->GetMpiRank(),
                         pure_thread->GetThreadNumInProcess());

    if (cpu_id_this_thread.has_value()) {
        set_cpu_affinity_on_linux(cpu_id_this_thread.value());
    }

#if DEBUG_CHECK
    if (pure_thread->Get_rank() == 0) {
        PrintThreadMap();
    }
#endif
    // set global variable to be (optionally) used in applications
    pure_thread_global = pure_thread;
    const int main_ret = __orig_main(pure_thread->GetArgc(),
                                     const_cast<char**>(pure_thread->GetArgv()),
                                     pure_thread);
    thread_return_values[pure_thread->GetThreadNumInProcess()] = main_ret;
}

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
void PureProcess::ExhaustRemainingWorkDriver(PureThread* pt,
                                             int         helper_thread_id) {
    char tname[16];
    sprintf(tname, "--HELPER_%.3d--", helper_thread_id);
    pthread_set_name(tname);

    set_cpu_affinity_on_linux(pt->GetCpuId());

    // wait until all of the main threads have initialized
    while (MainThreadsAllInitialized() == false) {
    }

    const bool is_helper = true;
    pt->ExhaustRemainingWork(is_helper);
}
#endif

void PureProcess::BindChannelEndpoint(
        BundleChannelEndpoint* const bce, int mate_mpi_rank,
        PureRT::EndpointType endpoint_type, PureRT::BundleMode bundle_mode,
        int                    thread_num_in_mpi_process,
        channel_endpoint_tag_t channel_endpoint_tag,
        bundle_size_t user_specified_bundle_sz, bool using_rt_send_buf_hint,
        bool using_sized_enqueue_hint, size_t max_buffered_msgs,
        PurePipeline&& cont_pipeline, PureThread* pure_thread,
        PureComm* const pure_comm) {

    assert(BundleChannelEndpoint::ValidEndpointType(endpoint_type));
    assert(mate_mpi_rank >= 0 ||
           mate_mpi_rank == PureRT::PURE_UNUSED_RANK /* when not used */);
    assert(mate_mpi_rank < mpi_size);
    assert(user_specified_bundle_sz > 0 ||
           user_specified_bundle_sz == PureRT::PURE_UNUSED_BUNDLE_SIZE);

#if ENABLE_PURE_FLAT_BATCHER
    // super hack to debug in lldb with no mpi running.
    fprintf(stderr, "forcing batcher mode\n");
    BindToFlatBatcher(endpoint_type, bce);
    return;
#endif
    //////////////////////////////

    if (mate_mpi_rank == mpi_rank) {
        // note/TODO: probably should allow ProcessChannels when bundling is
        // in effect to keep the user code simple. keeping this for now
        // (1/26), but likely change this.
        assert(bundle_mode == PureRT::BundleMode::none);

        if (bce->IsSendToSelfChannel()) {
            // bind to a send to self channel
            BindToSendToSelfChannel(bce, endpoint_type, pure_comm);
        } else {
            // bind to a process channel
            BindToProcessChannel(pure_thread, bce, endpoint_type,
                                 using_rt_send_buf_hint, max_buffered_msgs,
                                 pure_comm, std::move(cont_pipeline));
        }
    } else {
        // Use MPI -- intra-node
        switch (bundle_mode) {
        case PureRT::BundleMode::none:
            // bind to a direct MPI channel
#if USE_RDMA_MPI_CHANNEL
            BindToRdmaMPIChannel(endpoint_type, bce, mate_mpi_rank,
                                 std::move(cont_pipeline));
#else
            // MPI channels.
            if (UseDirectChannelBatcher(bce)) {
                BindToBatcherManager(endpoint_type, bce, mate_mpi_rank);
            } else {
                BindToDirectMPIChannel(endpoint_type, bce, mate_mpi_rank,
                                       using_rt_send_buf_hint,
                                       using_sized_enqueue_hint, pure_comm,
                                       std::move(cont_pipeline));
            }
#endif
            break;
        default:
            fprintf(stderr,
                    "ERROR: invalid bundle mode with integer value %d\n",
                    as_integer(bundle_mode));
            exit(-401);

        } // end switch on bundle mode
    }

} // function BindChannelEndpoint

bool PureProcess::UseDirectChannelBatcher(
        BundleChannelEndpoint* const bce) const {
    // for now, go with a simple rule of big messages don't get batched. revisit
    // this later based on actual performance.
    return ENABLE_PURE_FLAT_BATCHER &&
           (bce->NumPayloadBytes() <=
            MAX_DIRECT_CHAN_BATCHER_MESSAGE_PAYLOAD_BYTES);
}

void PureProcess::BindBcastChannel(PureThread*         calling_pure_thread,
                                   BcastChannel* const bc,
                                   int                 root_pure_rank_in_comm,
                                   channel_endpoint_tag_t cet,
                                   PureComm*              pure_comm) {
    BcastProcessChannel* bpc;
    {
        std::lock_guard<std::mutex> bc_init_lock(
                tagged_bcast_process_channel_init_mutex);
        auto bcast_pc_iter = tagged_bcast_process_channels_map.find(cet);

        if (bcast_pc_iter == tagged_bcast_process_channels_map.end()) {
            // didn't find, so create it

            const auto pure_ranks_this_comm_this_process =
                    pure_comm->GetProcessDetails()->NumPureRanksThisProcess();
            check(pure_ranks_this_comm_this_process > 0,
                  "BindBcastChan: num ranks (threads) in process in PureComm "
                  "is %d",
                  pure_ranks_this_comm_this_process);

            const auto root_mpi_rank_in_pure_comm =
                    pure_comm->CommMpiRank(root_pure_rank_in_comm);
            const auto my_mpi_rank_in_pure_comm = pure_comm->MyCommMpiRank();
            const bool is_root_process =
                    (root_mpi_rank_in_pure_comm == my_mpi_rank_in_pure_comm);

            const auto pure_comm_is_multiprocess =
                    pure_comm->GetProcessDetails()->NumUniqueMpiRanks() > 1;

            check(bc->NumPayloadBytes() > 0,
                  "payload bytes (count) for bcast channel is zero. Possibly "
                  "just conditionally create this if > 0");
            bpc = new BcastProcessChannel(
                    bc->GetCount(), bc->GetDatatype(),
                    root_mpi_rank_in_pure_comm, bc->NumPayloadBytes(),
                    root_pure_rank_in_comm, pure_ranks_this_comm_this_process,
                    pure_comm_is_multiprocess, is_root_process,
                    pure_comm->GetMpiComm());

            auto created_pair =
                    tagged_bcast_process_channels_map.insert({cet, bpc});
            assert(created_pair.second);
        } else {
            bpc = bcast_pc_iter->second;
        }
    }
    bpc->BindBcastChannel(bc);
}

void PureProcess::BindReduceChannel(PureThread*          calling_pure_thread,
                                    ReduceChannel* const rc,
                                    int                  root_pure_rank_in_comm,
                                    channel_endpoint_tag_t cet,
                                    PureComm* const        pure_comm) {
    ReduceProcessChannel* rpc;
    {
        std::lock_guard<std::mutex> rc_init_lock(
                tagged_reduce_process_channel_init_mutex);
        auto reduce_pc_iter = tagged_reduce_process_channels_map.find(cet);

        if (reduce_pc_iter == tagged_reduce_process_channels_map.end()) {
            // didn't find, so create it

            // IMPORTANT: pure_comm is just one thread-level PureComm for the
            // thread that got there first. Don't use any thread-specific fields
            // or method calls.

            const auto pure_ranks_this_comm_this_process =
                    pure_comm->GetProcessDetails()->NumPureRanksThisProcess();
            check(pure_ranks_this_comm_this_process > 0,
                  "BindReduceChannel: num ranks (threads) in process in "
                  "PureComm "
                  "is %d",
                  pure_ranks_this_comm_this_process);
            const auto mpi_rank_for_root_in_pure_comm =
                    pure_comm->CommMpiRank(root_pure_rank_in_comm);
            const auto my_mpi_rank_in_pure_comm = pure_comm->MyCommMpiRank();

            // const int mpi_rank_for_root =
            // MPIRankFromThreadRank(root_pure_rank); const bool is_root_process
            // = (mpi_rank_for_root == mpi_rank);
            const bool is_root_process =
                    (root_pure_rank_in_comm == my_mpi_rank_in_pure_comm);
            const auto pure_comm_is_multiprocess =
                    pure_comm->GetProcessDetails()->NumUniqueMpiRanks() > 1;

            rpc = new ReduceProcessChannel(
                    rc->GetCount(), rc->GetDatatypeBytes(), rc->GetDatatype(),
                    mpi_rank_for_root_in_pure_comm, rc->NumPayloadBytes(),
                    root_pure_rank_in_comm, pure_ranks_this_comm_this_process,
                    pure_comm_is_multiprocess, is_root_process, rc->GetOp());

            auto created_pair =
                    tagged_reduce_process_channels_map.insert({cet, rpc});
            assert(created_pair.second);
        } else {
            rpc = reduce_pc_iter->second;
        }
    }

    rpc->BindReduceChannel(pure_comm->GetThreadNumInCommInProcess(), rc);
}

int PureProcess::NumMessagesPerBundle(PureRT::BundleMode bundle_mode,
                                      bundle_size_t user_specified_bundle_sz) {
    int num_msgs;
    switch (bundle_mode) {
    case PureRT::BundleMode::none:
        num_msgs = 1;
        break;
    case PureRT::BundleMode::specified_channel_tag:
        assert(user_specified_bundle_sz > 0);
        assert(user_specified_bundle_sz !=
               default_bundle_size_for_num_msgs_per_bundle);
        num_msgs = user_specified_bundle_sz;
        break;
    case PureRT::BundleMode::one_msg_per_sender_thread:
        num_msgs = num_threads;
        break;
    default:
        std::cerr << "Invalid bundle_mode " << as_integer(bundle_mode)
                  << ". Exiting..." << std::endl;
        exit(-3403);
    }

    return num_msgs;
} // function NumMessagesPerBundle

int PureProcess::MPITagForBundle(PureRT::BundleMode     bundle_mode,
                                 int                    sender_mpi_rank,
                                 channel_endpoint_tag_t cet) {

    int tag;
    switch (bundle_mode) {
    case PureRT::BundleMode::specified_channel_tag:
        assert(cet != default_cet_for_mpi_tag_for_bundle);
        tag = static_cast<int>(cet);
        break;
    case PureRT::BundleMode::one_msg_per_sender_thread:
        tag = mpi_msg_tag_base + sender_mpi_rank;
        break;
    default:
        sentinel("Invalid bundle_mode %d", bundle_mode);
        exit(-3404);
    } // switch

    return tag;

} // function MPITagForBundle

bool UseBufferedChan(BundleChannelEndpoint const* const bce) {
    return (bce->NumPayloadBytes() <= BUFFERED_CHAN_MAX_PAYLOAD_BYTES);
}
// private

void PureProcess::BindToSendToSelfChannel(BundleChannelEndpoint* bce,
                                          PureRT::EndpointType   endpoint_type,
                                          PureComm* const        pure_comm) {

    BundleChannelEndpointMetadata key{pure_comm,
                                      bce->GetCount(),
                                      bce->GetDatatype(),
                                      bce->GetSenderPureRank(),
                                      bce->GetDestPureRank(),
                                      bce->GetTag()};

    auto it = send_to_self_channels.find(key);
    if (it == send_to_self_channels.end()) {
        const auto new_one = new SendToSelfChannel();
        bce->SetSendToSelfChannel(new_one);
        send_to_self_channels.insert({key, new_one});
    } else {
        bce->SetSendToSelfChannel(it->second);
    }

    if (endpoint_type == PureRT::EndpointType::SENDER) {
        SendChannel* sc = dynamic_cast<SendChannel*>(bce);
        sc->SetInitializedState();
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        RecvChannel* rc = dynamic_cast<RecvChannel*>(bce);
        rc->SetInitializedState();
    }
}

void PureProcess::BindToProcessChannel(PureThread* const            pure_thread,
                                       BundleChannelEndpoint* const bce,
                                       PureRT::EndpointType endpoint_type,
                                       bool            using_rt_send_buf_hint,
                                       size_t          max_buffered_msgs,
                                       PureComm* const pure_comm,
                                       PurePipeline&&  cont_pipeline) {

    // NOTE: using_rt_recv_buf is unused at this time.

    // build a hash key to either find or insert
    BundleChannelEndpointMetadata key{pure_comm,
                                      bce->GetCount(),
                                      bce->GetDatatype(),
                                      bce->GetSenderPureRank(),
                                      bce->GetDestPureRank(),
                                      bce->GetTag()};
    ProcessChannel*               appropriate_process_channel;

    {
        std::lock_guard<std::mutex> process_channel_init_lock(
                process_channel_init_mutex);

        auto process_channel_found_iterator = process_channels.find(key);
        if (process_channel_found_iterator == process_channels.end()) {
            // first thread in (of two) creates a new Process Channel
            // using placement-new to enforce 64-byte alignment (reported by
            // UBSAN) this can be upgraded to aligned_alloc with C++17.
            int         allocated_bytes;
            void* const buf = jp_memory_alloc<1, CACHE_LINE_BYTES>(
                    sizeof(ProcessChannel), &allocated_bytes);
            assert(sizeof(buf) == 8);
            // save this for later freeing
            process_channel_raw_buffers.push_back(buf);

            appropriate_process_channel = new (buf)
                    ProcessChannel(bce->NumPayloadBytes(), UseBufferedChan(bce),
                                   max_buffered_msgs);
            process_channels.insert({key, appropriate_process_channel});
        } else {
            appropriate_process_channel =
                    process_channel_found_iterator->second;
        }
    } // unlocks process_channel_init_mutex

    // do thread-specific setup
    if (endpoint_type == PureRT::EndpointType::SENDER) {
        if (using_rt_send_buf_hint) {
            appropriate_process_channel->AllocateSendRTBuffer();
        }
        // IMPORTANT: AllocateSendRTBuffer must be called before this, as it
        // initializes send_buf_ within ProcessChannel
        appropriate_process_channel->BindSendChannel(
                dynamic_cast<SendChannel* const>(bce));
    } else {
        assert(endpoint_type == PureRT::EndpointType::RECEIVER);
        appropriate_process_channel->BindRecvChannel(
                dynamic_cast<RecvChannel* const>(bce));
#if PCV4_OPTION_EXECUTE_CONTINUATION
#if DEBUG_CHECK
        for (const auto& cont_details : cont_pipeline.pipeline) {
            cont_details.AssertNonmonotoneCont();
        }
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
        if (cont_pipeline.HasAnyCollabStage()) {
            this->RegisterVictimThread(pure_thread);
        }
#endif
        if (!cont_pipeline.Empty()) {
            // only set the continuation in the channel if it's not empty
            appropriate_process_channel->SetDeqContPipeline(
                    std::move(cont_pipeline));
        }
        // DON'T USE cont_pipeline after this point -- it's been moved
#endif
    } // end receiver endpoint

    // two-way binding; needs to be called by both the SendChannel and
    // RecvChannel
    bce->SetProcessChannel(appropriate_process_channel);
} // function BindToProcessChannel

void PureProcess::BindToFlatBatcher(PureRT::EndpointType         endpoint_type,
                                    BundleChannelEndpoint* const bce) {

    if (endpoint_type == PureRT::EndpointType::SENDER) {
        SendChannel* sc = dynamic_cast<SendChannel*>(bce);
        sc->SetSendFlatBatcher(send_flat_batcher);
        sc->SetInitializedState();
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        RecvChannel* rc = dynamic_cast<RecvChannel*>(bce);
        rc->SetRecvFlatBatcher(recv_flat_batcher);
        rc->SetInitializedState();
    }
}

void PureProcess::BindToBatcherManager(PureRT::EndpointType endpoint_type,
                                       BundleChannelEndpoint* const bce,
                                       int mate_mpi_rank) {

    // first, set up the batcher for this MPI mate if necessary
    // there should be just one DCB between this pure process and mate_mpi_rank
    // for each of sender and receiver.
    if (endpoint_type == PureRT::EndpointType::SENDER) {
        {
            std::lock_guard<std::mutex> lg((enqueue_batcher_manager_mutex));
            if (enqueue_batcher_managers[mate_mpi_rank] == nullptr) {
                // there should be just one dequeue batcher manager from this
                // proc to mate_mpi_rank. Create on demand.
                enqueue_batcher_managers[mate_mpi_rank] = new BatcherManager(
                        NUM_DIRECT_CHAN_BATCHERS_PER_MATE, mate_mpi_rank,
                        endpoint_type, num_threads);
            }
        }
        SendChannel* sc = dynamic_cast<SendChannel*>(bce);
        sc->SetEnqueueBatcherManager(enqueue_batcher_managers[mate_mpi_rank]);
        sc->SetInitializedState();
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        {
            std::lock_guard<std::mutex> lg((dequeue_batcher_manager_mutex));
            if (dequeue_batcher_managers[mate_mpi_rank] == nullptr) {
                // there should be just one dequeue batcher manager from this
                // proc to mate_mpi_rank. Create on demand.
                dequeue_batcher_managers[mate_mpi_rank] = new BatcherManager(
                        NUM_DIRECT_CHAN_BATCHERS_PER_MATE, mate_mpi_rank,
                        endpoint_type, num_threads);
            }
        }
        RecvChannel* rc = dynamic_cast<RecvChannel*>(bce);
        rc->SetDequeueBatcherManager(dequeue_batcher_managers[mate_mpi_rank]);
        rc->SetInitializedState();
    }
}

void PureProcess::BindToDirectMPIChannel(PureRT::EndpointType endpoint_type,
                                         BundleChannelEndpoint* const bce,
                                         int  mate_mpi_rank,
                                         bool using_rt_send_buf_hint,
                                         bool using_sized_enqueue_hint,
                                         PureComm* const pure_comm,
                                         PurePipeline&&  cont_pipeline) {

    // NOTE: using_rt_recv_buf is unused at this time.

    // the third arg wants max_threads_per_process, hence num_threads, not
    // num_threads_this_proc

    // Pretty sure that multiple different threads can create these concurrently
    // -- disambiguation will be done using the tag (and possibly the mpi comm)
    const auto sender_thread_num_within_world_comm =
            pure_comm->CommWorldThreadNum(bce->GetSenderPureRank());
    const auto receiver_thread_num_within_world_comm =
            pure_comm->CommWorldThreadNum(bce->GetDestPureRank());

    auto dmc = new DirectMPIChannel(
            bce, mate_mpi_rank, pure_comm->GetMpiComm(),
            sender_thread_num_within_world_comm,
            receiver_thread_num_within_world_comm, endpoint_type,
            using_rt_send_buf_hint, using_sized_enqueue_hint,
            UseBufferedChan(bce), std::move(cont_pipeline));
    bce->SetDirectMPIChannel(dmc);
    {
        // multiple PureThreads could be creating DirectMPIChannels at the
        // same time, so we have to lock this vector that stores the
        // pointers to the created direct channels for destruction at the
        // end of the program.
        std::lock_guard<std::mutex> direct_mpi_channels_lock(
                direct_mpi_channels_mutex);
        direct_mpi_channels.push_back(dmc);
    }
} // function BindToDirectMPIChannel

void PureProcess::BindToRdmaMPIChannel(PureRT::EndpointType endpoint_type,
                                       BundleChannelEndpoint* const bce,
                                       int            mate_mpi_rank,
                                       PurePipeline&& cont_pipeline) {

    auto rmc =
            new RdmaMPIChannel(bce, mpi_comm_manager, mpi_rank, mate_mpi_rank,
                               endpoint_type, std::move(cont_pipeline));
    bce->SetRdmaMPIChannel(rmc);
    {
        std::lock_guard<std::mutex> lock(rdma_mpi_channels_mutex);
        rdma_mpi_channels.push_back(rmc);
    }
}

// TODO: update this to use the new per-thread sequence flag approach (see
// allreduce v3).
void PureProcess::Barrier(
        PureThread* pure_thread, bool& thread_barrier_sense,
        int  thread_num_in_process_in_comm,
        int  num_threads_this_process_this_comm,
        bool do_work_stealing_while_waiting, MPI_Comm mpi_comm,
        PureCommProcessDetail* const pure_comm_process_detail) {

    const auto is_leader = (thread_num_in_process_in_comm == 0);

#if MPI_ENABLED
    // step 1: thread 0 of this process does an MPI_Barrier, only if running
    // with multiple processes
    MPI_Request req;
    MPI_Status  status;
    if (is_leader) {
        auto const ret = MPI_Ibarrier(mpi_comm, &req);
        assert(ret == MPI_SUCCESS);
    }
#endif

    // TODO: (during perf analysis?) -- only non-leaders need to increment

    // step 2: all threads in this process do a thread-level barrier using a
    // sense-reversing barrier
    pure_comm_process_detail->IncNumThreadsReachedBarrier();

    if (is_leader) {
        // leader. wait until everyone has gotten here.

        // TODO: (during perf analysis?) -- only non-leaders need to increment

        while (pure_comm_process_detail->NumThreadsReachedBarrier() <
               num_threads_this_process_this_comm) {
            MAYBE_WORK_STEAL_COND(pure_thread, do_work_stealing_while_waiting);
            x86_pause_instruction();
        }
        // now, reset value for next time and unblock non-leaders
        pure_comm_process_detail->ResetNumThreadsReachedBarrier();
        const auto curr_sense = pure_comm_process_detail->CommBarrierSense();

#if MPI_ENABLED
        // step 3: wait for other processes to complete
        PureRT::do_mpi_wait_with_work_steal(pure_thread, &req, &status);
#endif

        // step 4: release everyone in my process
        pure_comm_process_detail->SetBarrierSense(!curr_sense);
        // process_barrier_sense.store(!curr_sense, std::memory_order_release);
    } else {
        // not the leader
        while (pure_comm_process_detail->CommBarrierSense() !=
               thread_barrier_sense) {
            MAYBE_WORK_STEAL_COND(pure_thread, do_work_stealing_while_waiting);
            x86_pause_instruction();
        }
    }

    // update thread sense for next time
    thread_barrier_sense = !thread_barrier_sense;
} // function Barrier

std::mutex& PureProcess::GetApplicationMutex(const std::string name) {

    std::lock_guard<std::mutex> application_lock(application_mutex_mutex);
    auto                        it = application_mutexes.find(name);

    std::mutex* mutex_for_name;
    if (it == application_mutexes.end()) {
        mutex_for_name = new std::mutex();
        application_mutexes.insert(
                std::pair<string, std::mutex&>(name, *mutex_for_name));
    } else {
        assert(it->first == name);
        mutex_for_name = &(it->second);
    }

    return *mutex_for_name;
}

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL

void PureProcess::RegisterVictimThread(PureThread* const pt) {
    std::lock_guard l(victim_threads_mutex);
    auto const p = std::find(victim_threads.begin(), victim_threads.end(), pt);
    if (p == victim_threads.end()) {
        victim_threads.push_back(pt);
    }
    if (PCV4_APPR_2_DEBUG)
        fprintf(stderr, "*** currently %zu threads as victims\n",
                victim_threads.size());
}

void PureProcess::RegisterIndependentPurePipeline(PurePipeline* const pp) {
    std::lock_guard l(independent_pure_pipelines_mutex);
#if DEBUG_CHECK
    auto const p = std::find(independent_pure_pipelines.begin(),
                             independent_pure_pipelines.end(), pp);
    assert(p == independent_pure_pipelines.end());
#endif
    independent_pure_pipelines.push_back(pp);
}

void PureProcess::PrintVictimPipelines() const {
    printf(KBOLD "PureThread\tPurePipeline\n" KRESET);
    for (const auto& t : victim_threads) {
        const auto pp = t->GetActiveVictimPipeline();
        printf("%p:\t%p\n", t, pp);
    }
}
// end debugging functions

#if PCV4_STEAL_VICTIM_LINEAR_PROBE
PurePipeline*
PureProcess::GetVictimPipeline(PureThread* const pure_thread) const {

    // There's a potential race condition with this because we don't want to
    // lock the accesses. Make sure this vector isn't accessed until
    // AllChannelsInit is called.

    // Linear probe, always starting at zero. everyone will work on the
    // first one that is doing work. must spread out the work more. for now,
    // just loop through entire array until something is found later, maybe
    // loop through in a random order using something like
    // https://lemire.me/blog/2017/09/18/visiting-all-values-in-an-array-exactly-once-in-random-order/
    // another op;timization before that point -- stack the victim threads
    // with the most process channels towards the front of the
    // victim_threads vector

#if USE_HELPER_THREADS_IF_FREE_CORES
    try {
#endif
        // check(victim_threads.size() > 0, "victim_threads in PureProcess
        // should be greater than zero, but is %lu", victim_threads.size());
        // fprintf(stderr, "t %p:\tnum victim threads: %lu\n", pure_thread,
        //         victim_threads.size());

        const int num_victims = victim_threads.size();
#pragma code_align 32 // trying this as per Intel Advisor recommendation
        for (int i = 0; i < num_victims; ++i) {
            //        for (const __restrict auto& t : victim_threads) {
            // writing the loop this way to precompute the number of victim
            // threads to potentially help this be vectorized
            const PureThread* __restrict t = victim_threads[i];
            if (t != pure_thread) {
                // don't steal from my own thread, at least relevant with
                // ExhaustRemainingWork
                PurePipeline* pp = t->GetActiveVictimPipeline();
                if (pp != nullptr) {
                    return pp;
                }
            }
        }
        return nullptr;
#if USE_HELPER_THREADS_IF_FREE_CORES
    } catch (std::exception& e) {
        // for cases when helper threads are turned on, the threads may get
        // deallocated while a helper thread is here.
        return nullptr;
    }
#endif // USE_HELPER_THREADS_IF_FREE_CORES
}
#endif // PCV4_STEAL_VICTIM_LINEAR_PROBE

#if PCV4_STEAL_VICTIM_RANDOM_PROBE
PurePipeline*
PureProcess::GetVictimPipeline(PureThread* const pure_thread) const {
#if USE_HELPER_THREADS_IF_FREE_CORES
    try {
#endif
        const auto num_victims = victim_threads.size();
        for (auto i = 0; i < num_victims; ++i) {
            assert(pure_thread->GetCurrVictimIdx() < num_victims &&
                   pure_thread->GetCurrVictimIdx() >= 0);
            const auto probe_i =
                    (pure_thread->GetCurrVictimIdx() + i) % num_victims;
            const PureThread* __restrict victim_thread =
                    victim_threads[probe_i];
            if (victim_thread != pure_thread) {
                // not me, see if there's some work to do
                PurePipeline* __restrict pp =
                        victim_thread->GetActiveVictimPipeline();
                if (pp != nullptr) {
                    // we tried to steal some work, so update the victim idx
                    // of the pure thread to be the *next* idx
                    pure_thread->SetCurrVictimIdx((probe_i + 1) % num_victims);
                    return pp;
                }
            }
        }

        // didn't find any active victim pure pipelines, so return nullptr
        return nullptr;
#if USE_HELPER_THREADS_IF_FREE_CORES
    } catch (std::exception& e) {
        // for cases when helper threads are turned on, the threads may get
        // deallocated while a helper thread is here.
        assert(USE_HELPER_THREADS_IF_FREE_CORES == 1);
        return nullptr;
    }
#endif // USE_HELPER_THREADS_IF_FREE_CORES
}
#endif // PCV4_STEAL_VICTIM_RANDOM_PROBE

#endif

// consider making this a method of PureComm and making helper methods from this
// as well.

#if 0
PureComm* PureProcess::PureCommSplitDeprecated(int tnum, int pure_rank,
                                               int color, int key,
                                               PureComm* origin_pure_comm) {

    // currently doesn't support nesting of PureComms
    PureCommProcessDetail* const process_details =
            origin_pure_comm->GetProcessDetails();

    // TODO: is tnum proper? Is it maybe tnum within this process within this
    // comm?
    process_details->GetSplitDetails()[tnum] = {pure_rank, color, key};

    // Given that every thread inside origin_pure_comm within this process MUST
    // call PureCommSplit, then we can just use rank 0 within origin_pure_comm
    // to determine the leader.
    if (origin_pure_comm->CommRank() == 0) {
        // leader

        const auto threads_in_comm_in_process =
                process_details->NumPureRanksThisProcess();
        // the leader doesn't increment the counter, so that's why we wait until
        // threads_in_comm_in_process - 1
        while (comm_split_num_threads_reached.load() <
               threads_in_comm_in_process - 1)
            ; // barrier until process_comm_split_details are initialized by all
              // threads

        // 2. only the leader thread does the communication
        const auto origin_comm_size = origin_pure_comm->Size();
        int        origin_mpi_rank;
#if MPI_ENABLED
        MPI_Comm_rank(origin_pure_comm->GetMpiComm(), &origin_mpi_rank);

        PureCommSplitDetails global_details[pure_size];
        int ret = MPI_Allgather(process_comm_split_details, num_threads,
                                split_details_mpi_type, global_details,
                                num_threads, split_details_mpi_type,
                                origin_pure_comm->GetMpiComm());
        assert(ret == MPI_SUCCESS);
#else
        origin_mpi_rank = 0;
        // no need to copy or do the Allgather -- just repoint to keep the code
        // clean
        PureCommSplitDetails* global_details = process_comm_split_details;
#endif

#define COMM_SPLIT_DEBUG_PRINT 0
#if COMM_SPLIT_DEBUG_PRINT
        for (auto t = 0; t < pure_size; ++t) {
            printf("r%d\tGLOBAL DETAILS[%d] = {%d, %d, %d}\t", pure_rank, t,
                   global_details[t].origin_pure_rank, global_details[t].color,
                   global_details[t].key);
        }
        printf("\n");
#endif

        // 3. Now all the the ranks have the required data to create the new
        // global MPI communicator and the process-local Pure comm details.
        std::unordered_set<int> unique_colors;
        for (auto gt = 0; gt < pure_size; ++gt) {
            unique_colors.insert(global_details[gt].color);
        }

        for (auto c : unique_colors) {
            // fprintf(stderr,
            //         KCYN "############## pure world rank %d: looking at color
            //         "
            //              "%d\n" KRESET,
            //         pure_rank, c);

            // TODO: only create this if there's > 0 threads in this process
            // with this color

            // STEP 1: create the MPI comm for this color c
            std::vector<int> origin_mpi_ranks_this_color;
            PureComm         new_pure_comm;

            for (auto t = 0; t < pure_size; ++t) {
                PureCommSplitDetails& details = global_details[t];
                if (details.color == c) {
                    // TODO: only works for comm_world
                    // !!!!!!!!!!!!!!!!!!!!!!!!

                    // Does miniAMR use non-rank-based key ordering with split?
                    int origin_mpi_rank_for_pure_rank;
                    if (origin_pure_comm->GetMpiComm() == MPI_COMM_WORLD) {
                        // this means we are calling split from the "world"
                        // communicator
                        origin_mpi_rank_for_pure_rank =
                                MPIRankFromThreadRank(details.origin_pure_rank);
                    } else {
                        // we are setting an MPI rank
                        origin_mpi_rank_for_pure_rank =
                                origin_pure_comm->CommMpiRank(
                                        details.origin_pure_rank);
                    }

                    origin_mpi_ranks_this_color.push_back(
                            origin_mpi_rank_for_pure_rank);
                    new_pure_comm.AddOriginPureRank(details.origin_pure_rank);
                }
            }

            std::vector<int> new_mpi_ranks;
            int              curr_new_mpi_rank = 0;
            for (auto i = 0; i < origin_mpi_ranks_this_color.size(); ++i) {
                if (origin_mpi_ranks_this_color[i] != curr_new_mpi_rank) {
                    ++curr_new_mpi_rank;
                }
                new_mpi_ranks.push_back(curr_new_mpi_rank);
            }

            int new_mpi_rank_idx = 0;
            for (auto t = 0; t < pure_size; ++t) {
                if (global_details[t].color == c) {
                    new_pure_comm.AddNewMpiRank(
                            new_mpi_ranks[new_mpi_rank_idx++]);
                }
            }

            std::vector<int> uniq_origin_mpi_ranks_this_color =
                    origin_mpi_ranks_this_color;

            // now we have a vector of all of the mpi ranks for color c, but it
            // may not be unique
            std::sort(uniq_origin_mpi_ranks_this_color.begin(),
                      uniq_origin_mpi_ranks_this_color.end());
            uniq_origin_mpi_ranks_this_color.erase(
                    unique(uniq_origin_mpi_ranks_this_color.begin(),
                           uniq_origin_mpi_ranks_this_color.end()),
                    uniq_origin_mpi_ranks_this_color.end());

            // STEP 2: now, create a group and then a comm
            // Do I have to create a comm if I'm not in it??? Probably not, but
            // ignore for now. (test implmentation in place now)
#if MPI_ENABLED
            MPI_Comm new_mpi_comm;
            if (std::find(uniq_origin_mpi_ranks_this_color.begin(),
                          uniq_origin_mpi_ranks_this_color.end(),
                          origin_mpi_rank) ==
                uniq_origin_mpi_ranks_this_color.end()) {
                new_mpi_comm = MPI_COMM_NULL;
            } else {
                new_mpi_comm = PureRT::create_mpi_comm(
                        uniq_origin_mpi_ranks_this_color,
                        origin_pure_comm->GetMpiComm(), unique_comm_tag++);
            }
            new_pure_comm.SetMpiComm(new_mpi_comm);
#endif

            // determine the number of pure ranks in this process
            int pure_ranks_this_process_this_comm = 0;
            for (auto t = 0; t < num_threads; ++t) {
                if (process_comm_split_details[t].color == c) {
                    ++pure_ranks_this_process_this_comm;
                }
            }
            new_pure_comm.SetNumPureRanksThisProcess(
                    pure_ranks_this_process_this_comm);

            // determine ranks for each color
            int max_color = 0;
            for (auto c : unique_colors) {
                if (c > max_color) {
                    max_color = c;
                }
            }
            int rank_counter_for_color[max_color + 1]; // counter for each color
            for (auto i = 0; i <= max_color; ++i) {
                rank_counter_for_color[i] = 0;
            }

            // loop through, setting the color and new MPI rank
            // TODO: deal with key sort
            for (auto r = 0; r < pure_size; ++r) {
                PureCommSplitDetails& details    = global_details[r];
                const auto            this_color = details.color;
                details.new_pure_rank = rank_counter_for_color[this_color]++;
            }

            // now, loop through process_comm_split_details and set the
            // resulting_pure_comm field for all entries that have this color.
            // Each thread needs their own copy of their PureComm as it contains
            // the rank for them.

            // assert all are set
            // global_details now contains the finalized PureComm. But, each
            // thread needs its own copy with some customization...
            for (auto t = 0; t < num_threads; ++t) {
                if (process_comm_split_details[t].color == c) {
                    PureComm* const thread_pure_comm_copy =
                            new PureComm(new_pure_comm);

                    process_comm_split_details[t].resulting_pure_comm =
                            thread_pure_comm_copy;
                    assert(process_comm_split_details[t].resulting_pure_comm !=
                           nullptr);

                    // find corresponding entry in global details
                    auto offset_into_global = num_threads * origin_mpi_rank;
                    const auto& corresponding_global_details =
                            global_details[offset_into_global + t];

                    // fprintf(stderr,
                    //         "\n[%p] ***** origin mpi: %d  Setting new comm "
                    //         "rank to %d for thread %d  global details index "
                    //         "%d\n",
                    //         process_comm_split_details[t].resulting_pure_comm,
                    //         origin_mpi_rank,
                    //         corresponding_global_details.new_pure_rank, t,
                    //         offset_into_global + t);

                    thread_pure_comm_copy->SetCommRank(
                            corresponding_global_details.new_pure_rank);
                }
            } // ends thread copy loop
        }     // ends for each color

        for (auto t = 0; t < num_threads; ++t) {
            // fprintf(stderr,
            //         "CHECKING: "
            //         "process_comm_split_details[%d].resulting_pure_comm = "
            //         "%p\n",
            //         t, process_comm_split_details[t].resulting_pure_comm);
            process_comm_split_details[t].AssertInvarients();
            // fprintf(stderr,
            //         "ORIGIN MPI: %d  check: t%d   color %d --> %p  num ranks
            //         = "
            //         "%d\n",
            //         origin_mpi_rank, t, process_comm_split_details[t].color,
            //         process_comm_split_details[t].resulting_pure_comm,
            //         process_comm_split_details[t].resulting_pure_comm->Size());
        }

        // reset barrier and let other threads go forward
        comm_split_num_threads_reached.store(0);

    } else {
        process_details->IncThreadsReached(); // barrier

        // non-leaders
        while (comm_split_num_threads_reached.load() > 0)
            ; // barrier
    }

    // finish up
    return process_comm_split_details[tnum].resulting_pure_comm;
    // END LOCK
    // ///////////////////////////////////////////////////////////////////////////////
}
#endif

#ifdef DEPRECATED
// private
void PureProcess::BindToBundleChannel(BundleChannelEndpoint* const bce,
                                      int                  mate_mpi_rank,
                                      PureRT::EndpointType endpoint_type,
                                      PureRT::BundleMode /*bundle_mode*/,
                                      int thread_num_in_mpi_process) {

    BundleChannel* appropriate_bundle_channel = nullptr;
    mutex*         appropriate_mutex;
    unordered_map<int, vector<BundleChannel*>>* appropriate_bundle_channels_map;
    int                                         mpi_sender_rank;
    bool sender_endpoint = endpoint_type == PureRT::EndpointType::SENDER;

    if (sender_endpoint) {
        appropriate_mutex               = &sender_bundle_channel_init_mutex;
        appropriate_bundle_channels_map = &sender_bundle_channels;
        mpi_sender_rank                 = mpi_rank;
    } else {
        appropriate_mutex               = &receiver_bundle_channel_init_mutex;
        appropriate_bundle_channels_map = &receiver_bundle_channels;
        mpi_sender_rank                 = mate_mpi_rank;
    }

    {
        std::lock_guard<std::mutex> bc_init_lock(*appropriate_mutex);

        // goal: find a bundle channel that is going to the right
        // destination MPI endpoint, and also has bundle channel endpoint
        // slots available
        auto bundle_channel_vec_iterator =
                appropriate_bundle_channels_map->find(mate_mpi_rank);

        if (bundle_channel_vec_iterator !=
            appropriate_bundle_channels_map->end()) {
            // loop through the iterator to see if any existing bundle
            // channels are not already full. if so, bind to it. otherwise,
            // create new one.
            // TODO(jim): there may be an stl helper that checks for member
            // of a container that fulfills a condition, like Ruby's
            // Enumerable#any?
            for (auto bc : bundle_channel_vec_iterator->second) {
                if (bc->EndpointsAvailable()) {
                    appropriate_bundle_channel = bc;
                    break;
                }
            }
        }

        if (appropriate_bundle_channel == nullptr) {
            // create a new Bundle Channel

            // TODO(jim): switch statement for subclasses of BundleChannel.
            // Refactor code.

            appropriate_bundle_channel = new BundleChannel(
                    mate_mpi_rank, endpoint_type,
                    this->NumMessagesPerBundle(
                            PureRT::BundleMode::one_msg_per_sender_thread),
                    MPITagForBundle(
                            PureRT::BundleMode::one_msg_per_sender_thread,
                            mpi_sender_rank));

            // helpful guide to navigating maps of vectors:
            // http://stackoverflow.com/questions/29312967/stl-unordered-map-inserting-into-a-vector
            auto bundle_channel_vec_iterator =
                    appropriate_bundle_channels_map->find(mate_mpi_rank);
            if (bundle_channel_vec_iterator ==
                appropriate_bundle_channels_map->end()) {
                auto insert_ret_pair = appropriate_bundle_channels_map->insert(
                        make_pair(mate_mpi_rank,
                                  vector<BundleChannel*>(
                                          1, appropriate_bundle_channel)));
                assert(insert_ret_pair.second);

                // note: insert_ret_pair is a pair of (iterator, bool). the
                // boolean says it the
                // above insert succeeded.
                // the iterator is a pair of (key, value), where key is the
                // inserted key, and value
                // is the inserted
                // value. in our case, we first wnat the iterator, and then
                // the value, as we want the inserted vector.
                (insert_ret_pair.first)
                        ->second.push_back(appropriate_bundle_channel);
            } else {
                // append to existing entry in the map.
                // bundle_channel_vec_iterator (pointer) is a pair of (key,
                // value) of the found
                // element
                assert(bundle_channel_vec_iterator->first == mate_mpi_rank);
                bundle_channel_vec_iterator->second.push_back(
                        appropriate_bundle_channel);
            }
        } // ends if need to create a new bundle channel

        // TODO(jim): combine the following functions? Any reason to have
        // separate?
        appropriate_bundle_channel->BindChannelEndpoint(bce); // this is
                                                              // thread-safe

    } // ends critical section based on appropriate_mutex

#if BUNDLE_CONTENTS_DEBUG
    if (sender_endpoint) {
        ts_log_info("[p%d] BEFORE BundleChannel::Initialize for send to p%d",
                    mpi_rank, mate_mpi_rank);
    } else {
        ts_log_info("[p%d] BEFORE BundleChannel::Initialize for receive "
                    "from p%d",
                    mpi_rank, mate_mpi_rank);
    }
#endif

    // thread-safe; only one thread (the leader) ends up initializing.
    appropriate_bundle_channel->Initialize(thread_num_in_mpi_process);

#if BUNDLE_CONTENTS_DEBUG
    if (sender_endpoint) {
        ts_log_info("[p%d] Done BundleChannel::Initialize for send to p%d",
                    mpi_rank, mate_mpi_rank);
    } else {
        ts_log_info("[p%d] Done BundleChannel::Initialize for receive from p%d",
                    mpi_rank, mate_mpi_rank);
    }
#endif

    // two-way binding
    bce->SetBundleChannel(appropriate_bundle_channel);
}

// TODO(jim): rewrite this to only take in the bce and surmise everything
// else from it.
void PureProcess::BindToTaggedBundleChannel(
        BundleChannelEndpoint* const bce, int mate_mpi_rank,
        PureRT::EndpointType endpoint_type, channel_endpoint_tag_t cet,
        bundle_size_t user_specified_bundle_sz, int thread_num_in_mpi_process) {

    BundleChannel*                      appropriate_bundle_channel = nullptr;
    mutex*                              appropriate_mutex;
    unordered_map<int, BundleChannel*>* appropriate_bundle_channels_map;
    int                                 mpi_sender_rank;

    // TODO(jim): refactor this. I think we can create an array indexed by
    // the enum value.
    switch (endpoint_type) {
    case PureRT::EndpointType::SENDER:
        appropriate_mutex = &tagged_sender_bundle_channel_init_mutex;
        appropriate_bundle_channels_map = &tagged_sender_bundle_channels;
        mpi_sender_rank                 = mpi_rank;
        break;
    case PureRT::EndpointType::RECEIVER:
        appropriate_mutex = &tagged_receiver_bundle_channel_init_mutex;
        appropriate_bundle_channels_map = &tagged_receiver_bundle_channels;
        mpi_sender_rank                 = mate_mpi_rank;
        break;
    default:
        sentinel("Invalid endpoint type %d", endpoint_type);
    }

    {
        std::lock_guard<std::mutex> bc_init_lock(*appropriate_mutex);

        // goal: find the bundle channel that has the user-specified channel
        // endpoint tag
        // assert that it still has room to add more bundle channel
        // endpoints
        auto bundle_channel_iterator =
                appropriate_bundle_channels_map->find(cet);

        if (bundle_channel_iterator != appropriate_bundle_channels_map->end()) {
            appropriate_bundle_channel = bundle_channel_iterator->second;
        } else {
            ts_log_info("did NOT find bundle_channel with cet %d; creating a "
                        "new one.",
                        cet);
        }

        bool create_new_bc = (appropriate_bundle_channel == nullptr);

        switch (endpoint_type) {
        case PureRT::EndpointType::SENDER:

            if (create_new_bc) {
                appropriate_bundle_channel = new SendBundleChannel(
                        mate_mpi_rank,
                        this->NumMessagesPerBundle(
                                PureRT::BundleMode::specified_channel_tag,
                                user_specified_bundle_sz),
                        MPITagForBundle(
                                PureRT::BundleMode::specified_channel_tag,
                                mpi_sender_rank, cet));
            }

            assert(appropriate_bundle_channel != nullptr);
            ts_log_info("SENDER [proc thread %d] about to bind %p",
                        thread_num_in_mpi_process, bce);
            dynamic_cast<SendBundleChannel*>(appropriate_bundle_channel)
                    ->BindChannelEndpoint(bce);

            ts_log_info("SENDER [proc thread %d] done binding %p",
                        thread_num_in_mpi_process, bce);
            assert(bce->GetBundleChannel() == appropriate_bundle_channel);

            break;

        case PureRT::EndpointType::RECEIVER:
            if (create_new_bc) {
                appropriate_bundle_channel = new RecvBundleChannel(
                        mate_mpi_rank,
                        this->NumMessagesPerBundle(
                                PureRT::BundleMode::specified_channel_tag,
                                user_specified_bundle_sz),
                        MPITagForBundle(
                                PureRT::BundleMode::specified_channel_tag,
                                mpi_sender_rank, cet));
            }
            assert(appropriate_bundle_channel != nullptr);
            ts_log_info("RECEIVER [proc thread %d] about to bind %p",
                        thread_num_in_mpi_process, bce);
            dynamic_cast<RecvBundleChannel*>(appropriate_bundle_channel)
                    ->BindChannelEndpoint(bce);
            ts_log_info("RECEIVER [proc thread %d] done binding%p",
                        thread_num_in_mpi_process, bce);

            assert(bce->GetBundleChannel() == appropriate_bundle_channel);
            break;

        default:
            sentinel("Invalid endpoint type %d", endpoint_type);
        } // ends switch on endpoint_type

        if (create_new_bc) {
            auto insert_ret_pair = appropriate_bundle_channels_map->insert(
                    make_pair(cet, appropriate_bundle_channel));
            assert(insert_ret_pair.second); // assert that the insertion
                                            // succeeded.
        }

        check(bce->GetBundleChannel() != nullptr,
              "This BCE must have a non-nullptr bundle channel by this "
              "point.");
        bce->CheckInvariants();
    } // ends lock on appropriate_mutex

    ts_log_info("About to exit PureProcess::BindToTaggedBundleChannel")

} // function BindToTaggedBundleChannel

#endif
