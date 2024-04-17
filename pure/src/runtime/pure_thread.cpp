// Author: James Psota
// File:   pure_thread.cpp 

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
#include <random>
#include <sys/stat.h>
#include <unistd.h>

// #include "pure/common/pure.h"
#include "pure/runtime/pure_thread.h"

#include "pure/support/zed_debug.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/reduce_channel.h"
#include "pure/transport/send_channel.h"
#ifdef LINUX
#include <numa.h>
#endif

#include "pure/runtime/mpi_comm_split_manager.h"
#include "pure/runtime/pure_process.h"
#include "pure/transport/all_reduce_channel.h"
#include "pure/transport/bcast_channel.h"
#include "pure/transport/bundle_channel.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/experimental/allreduce_small_payload.h"

#if COLLECT_THREAD_TIMELINE_DETAIL
#define CHANNEL_INIT_TIMER_START all_channel_init.start()
#define CHANNEL_INIT_TIMER_PAUSE all_channel_init.pause()
#else
#define CHANNEL_INIT_TIMER_START
#define CHANNEL_INIT_TIMER_PAUSE
#endif

#if PRINT_PROCESS_CHANNEL_STATS
#define PT_STAT_INC(t) ++t
#else
#define PT_STAT_INC(t)
#endif

// also defined in process channel v4 -- clean this up
#define PCV4_APPR_2_DEBUG 0

PureThread::PureThread(PureProcess* parent_pure_process_arg, int pure_rank,
                       int mpi_process_rank_arg, int num_mpi_processes_arg,
                       int thread_num_in_mpi_process_arg, int total_threads,
                       int max_threads_per_process, int cpu_id,
                       int orig_argc_arg, char** orig_argv_arg)
    : parent_pure_process(parent_pure_process_arg), pure_rank(pure_rank),
      mpi_process_rank(mpi_process_rank_arg),
      num_mpi_processes(num_mpi_processes_arg),
      thread_num_in_mpi_process(thread_num_in_mpi_process_arg),
      orig_argc(orig_argc_arg), orig_argv(orig_argv_arg),
      total_threads(total_threads),
      max_threads_per_process(max_threads_per_process), cpu_id(cpu_id)

#if COLLECT_THREAD_TIMELINE_DETAIL
      ,
      pc_deq_wait_end_to_end("rt0: PC.Deq Wait: end-to-end + cont"),
      pc_deq_wait_empty_queue_timer("rt1: PC.Deq Wait: queue empty spin"),
      pc_deq_wait_probe_pending_dequeues(
              "rt3: PC.Deq Wait: try other pending dequeues"),
      pc_deq_wait_continuation(
              "rt4: PC.Deq Wait: execute continuation wrapper"),
      pp_deq_wait_collab_cont_for_helpers_to_finish(
              "rt4.1: PP: wait for helper collab finish"),

      pc_enqueue_end_to_end("rt5.0: PC.Enqueue: end-to-end"),

      pc_enqueue_wait_this_only("rt6.0: PC.Enqueue: this only wait + memcpy"),
      pc_memcpy_timer("rt6: PC.Enqueue: memcpy only"),
      pc_enq_wait_probe_pending_enqueues(
              "rt6.1: PC.Enqueue Wait: try other pending enqueues"),

      pc_enq_wait_sr_collab_parent_wrapper("rt6.5: enq wait wrapper (dummy)"),
      pp_wait_collab_continuation_driver("rt6.2: PP: work steal thief driver"),
      pp_execute_cont("rt6.4: PP: do actual cont"),
      pp_enq_wait_collab_cont_wait_for_work("rt6.7: PP: collab wait for work"),
      pp_execute_ind_cont_wrapper(
              "rt6.8: PP: execute ind. cont wrapper (dummy)"),

      dc_mpi_isend("rt7: DC.Enqueue: MPI Start Send"),
      dc_mpi_send_wait("rt8: DC.Enqueue: MPI_Wait"),
      dc_mpi_irecv("rt9: DC.Dequeue: MPI Start Recv"),
      dc_mpi_recv_wait("rt10: DC.Dequeue: MPI_Wait"),
      dc_deq_wait_continuation("rt11: DC: do actual cont"),

      exhaust_remaining_work("rt18: Exhaust ramaining work via work stealing"),
      all_channel_init("rt20: All Channel Initialization")
#endif
{

#if COLLECT_THREAD_TIMELINE_DETAIL
    runtime_timers.reserve(22);
    runtime_timers.push_back(&pc_deq_wait_end_to_end);
    runtime_timers.push_back(&pc_deq_wait_empty_queue_timer);
    runtime_timers.push_back(&pc_deq_wait_probe_pending_dequeues);
    runtime_timers.push_back(&pp_execute_cont);
    runtime_timers.push_back(&pc_deq_wait_continuation);
    runtime_timers.push_back(&pp_deq_wait_collab_cont_for_helpers_to_finish);
    runtime_timers.push_back(&pc_enqueue_end_to_end);
    runtime_timers.push_back(&pc_enqueue_wait_this_only);
    runtime_timers.push_back(&pc_memcpy_timer);
    runtime_timers.push_back(&pc_enq_wait_probe_pending_enqueues);
    runtime_timers.push_back(&pc_enq_wait_sr_collab_parent_wrapper);
    runtime_timers.push_back(&pp_wait_collab_continuation_driver);
    runtime_timers.push_back(&pp_enq_wait_collab_cont_wait_for_work);
    runtime_timers.push_back(&pp_execute_ind_cont_wrapper);
    runtime_timers.push_back(&dc_mpi_isend);
    runtime_timers.push_back(&dc_mpi_send_wait);
    runtime_timers.push_back(&dc_mpi_irecv);
    runtime_timers.push_back(&dc_mpi_recv_wait);
    runtime_timers.push_back(&dc_deq_wait_continuation);
    runtime_timers.push_back(&exhaust_remaining_work);
    runtime_timers.push_back(&all_channel_init);
#endif

    // set up global, primordial pure communicator
    pure_comm_world = PureComm::CreatePureCommWorld(
            this, parent_pure_process, thread_num_in_mpi_process, pure_rank,
            Get_size(), num_mpi_processes,
            parent_pure_process->GetNumThreadsThisProcess());

#if TRACE_COMM
    // this enables writing a trace of all communication to a log file,
    // acceptable for R's igraph input format
    std::stringstream trace_comm_filename;
    // clear out any old files in that directory
    string outdir = "runs/temp_latest/"; // TODO(jim): make this a top level
                                         // global constant.
    {
        std::lock_guard<std::mutex> trace_comm_outstream_dir_lock(
                trace_comm_outstream_dir_mutex);
        struct stat st = {0};
        if (stat(outdir.c_str(), &st) == -1) {
            mkdir(outdir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
            // not checking status code because multiple processes will try to
            // create this directory
            // the locking only handles multiple different threads in the same
            // process.
            // assert(mkdir_status == 0);
        }
    }

    trace_comm_filename << outdir << "pure_comm_trace_r" << pure_rank << ".csv";
    trace_comm_outstream.open(trace_comm_filename.str());

    if (trace_comm_outstream.fail()) {
        std::cerr << "ERROR: comm trace output file "
                  << trace_comm_filename.str()
                  << " failed to open for writing. Exiting." << std::endl
                  << std::endl;
        exit(-200);
    }

    // add CSV header, consistent with header created by MPI tracer.
    trace_comm_outstream
            << "file:line, function, direction, sender_rank, "
               "receiver_rank, msg_len, datatype, tag, "
               "datatype_bytes, total_bytes, function_subtype, channel_type, "
               "channel_endpoint_id, mpi_called "
            << std::endl;
#endif

#if DO_PRINT_CONT_DEBUG_INFO
    char fname[128];
    sprintf(fname, "runs/temp_latest/cont_debug_r%.4d.tsv", pure_rank);
    cont_debug_file_handle = fopen(fname, "w");

    // TSV header
    fprintf(cont_debug_file_handle,
            "executing rank\tthread type\towner rank\tcont name\t"
            "execution mode\tstart chunk\tend chunk\tmin idx\tmax "
            "idx\tbacktrace\n");
    check(cont_debug_file_handle != nullptr, "Failed to open file %s", fname);
#endif
}

PureThread::~PureThread() {
    check(did_finalization,
          "PureThread is deconstructing but was never finalized. Did you "
          "remember to call pure_thread_global->Finalize() from your orig_main "
          "(just before calling the main timer pause)?");
    for (auto endpoint : outstanding_channel_endpoints) {
        delete (endpoint);
    }
#if TRACE_COMM
    trace_comm_outstream.close();
#endif
#if DO_PRINT_CONT_DEBUG_INFO
    fclose(cont_debug_file_handle);
#endif

    delete (pure_comm_world);

#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    fprintf(stderr, "• Work Steal stats for thread rank %d", pure_rank);
    fprintf(stderr, " %" PRIu64 " considerations;\t",
            stat_num_work_steal_considerations);
    fprintf(stderr, " %" PRIu64 " attempts\t(%.2f%% of considerations)\n",
            stat_num_work_steal_attempts,
            percentage<uint64_t>(stat_num_work_steal_attempts,
                                 stat_num_work_steal_considerations));
#endif
}

void PureThread::Finalize() {
    assert(did_finalization == false);
    parent_pure_process->PureFinalize();
    did_finalization = true;
}

int PureThread::Get_rank(PureComm const* const pc) const {
    if (pc == nullptr || pc == pure_comm_world) {
        // assume the user wants comm world
        assert(pure_rank == GetPureCommWorld()->CommRank());
        return pure_rank;
    } else {
        return pc->CommRank();
    }
}

int PureThread::MPIRankFromPureRank(int pure_rank) {
    return parent_pure_process->MPIRankFromThreadRank(pure_rank);
}

int PureThread::ThreadNumOnNodeFromPureRank(int pure_rank) {
    return parent_pure_process->ThreadNumOnNodeFromPureRank(pure_rank);
}

int PureThread::GetNumThreadsThisProcess() const {
    return parent_pure_process->GetNumThreadsThisProcess();
}

// function to create a SendChannel, which in turn creates a bundle channel in
// PureProcess. Note that the pure ranks are *within the passed pure_comm*, if
// any. The BundleChannelEndpoints that are created are also set up to be in the
// same comm as whatever was used during there initialization.
SendChannel* PureThread::InitSendChannel(
        void* const buf, int count, const MPI_Datatype& datatype,
        int dest_pure_rank, int tag, PureRT::BundleMode bundle_mode,
        channel_endpoint_tag_t channel_endpoint_tag,
        bundle_size_t user_specified_bundle_sz, bool using_rt_send_buf_hint,
        bool using_sized_endpoint_hint, size_t max_buffered_msgs,
        PureComm* const pure_comm) {

    CHANNEL_INIT_TIMER_START;

    // this should be enforced for recv channels as well but we have a
    // simplified function signature currently. this basically allows us to use
    // "low" tags for the user program and use high tags for wrapper classes
    // such as ScanChannels.
    if (channel_endpoint_tag != CHANNEL_ENDPOINT_TAG_SCAN_CHAN) {
        assert(tag <= PURE_USER_MAX_TAG);
    }

    // const int mate_mpi_rank =
    //         parent_pure_process->MPIRankFromThreadRank(dest_pure_rank);
    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    // testing: passing in rank in comm instead of pure_rank
    SendChannel* sc =
            new SendChannel(buf, count, datatype, pure_comm_to_use->CommRank(),
                            tag, dest_pure_rank, channel_endpoint_tag,
                            trace_comm_outstream, using_rt_send_buf_hint, this);

    const auto mate_mpi_rank = pure_comm_to_use->CommMpiRank(dest_pure_rank);

    parent_pure_process->BindChannelEndpoint(
            sc, mate_mpi_rank, PureRT::EndpointType::SENDER, bundle_mode,
            thread_num_in_mpi_process, channel_endpoint_tag,
            user_specified_bundle_sz, using_rt_send_buf_hint,
            using_sized_endpoint_hint, max_buffered_msgs,
            std::move(PurePipeline(0)), this, pure_comm_to_use);

    // solely used for dcon purposes
    outstanding_channel_endpoints.push_back(sc);
    sc->CheckInvariants();

    // this may be overly conservative given purecomm. possibly extend to
    // include purecomm.
    VerifyUniqueEndpoint(sc, PureRT::EndpointType::SENDER, count, datatype,
                         pure_rank, dest_pure_rank, tag, pure_comm_to_use);
    CHANNEL_INIT_TIMER_PAUSE;
    return sc;
}

RecvChannel* PureThread::InitRecvChannel(
        int count, const MPI_Datatype& datatype, int sender_pure_rank, int tag,
        bool using_rt_recv_buf, size_t max_buffered_msgs,
        PurePipeline&& pipeline, PureComm* const pure_comm) {

    // Note: using_rt_recv_buf is COMPLETELY unused now

    CHANNEL_INIT_TIMER_START;

    // TODO: make pipeline optional. post on SO if you need to figure out the
    // best approach.
    pipeline.FinalizeConstruction(this, false);

    // const int mate_mpi_rank =
    //         parent_pure_process->MPIRankFromThreadRank(dest_pure_rank);
    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);
    RecvChannel*    rc               = new RecvChannel(
            nullptr, count, datatype, sender_pure_rank, tag,
            pure_comm_to_use->CommRank(), CHANNEL_ENDPOINT_TAG_DEFAULT,
            trace_comm_outstream, nullptr, this);
    rc->CheckInvariants();
    VerifyUniqueEndpoint(rc, PureRT::EndpointType::RECEIVER, count, datatype,
                         sender_pure_rank, pure_rank, tag, pure_comm_to_use);

    const auto mate_mpi_rank = pure_comm_to_use->CommMpiRank(sender_pure_rank);

    const auto bundle_mode              = PureRT::BundleMode::none;
    const auto using_sized_enqueue_hint = false;
    const auto using_rt_send_buf_hint   = false;
    parent_pure_process->BindChannelEndpoint(
            rc, mate_mpi_rank, PureRT::EndpointType::RECEIVER, bundle_mode,
            thread_num_in_mpi_process, CHANNEL_ENDPOINT_TAG_DEFAULT,
            USER_SPECIFIED_BUNDLE_SZ_DEFAULT, using_rt_send_buf_hint,
            using_sized_enqueue_hint, max_buffered_msgs, std::move(pipeline),
            this, pure_comm_to_use);

    // solely used for dcon purposes
    outstanding_channel_endpoints.push_back(rc);
    rc->CheckInvariants();

    CHANNEL_INIT_TIMER_PAUSE;
    return rc;
}

BcastChannel*
PureThread::InitBcastChannel(int count, const MPI_Datatype& datatype,
                             int                    root_pure_rank_in_pure_comm,
                             channel_endpoint_tag_t channel_endpoint_tag,
                             PureComm* const        pure_comm) {

    CHANNEL_INIT_TIMER_START;
    // const MPI_Comm comm = pure_comm->GetMpiComm();
    // no nesting supported yet
    // check(comm == MPI_COMM_WORLD,
    //       "Only MPI_COMM_WORLD is supported for BcastChannels at this
    //       point");

    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    const auto pure_rank_in_pure_comm = pure_comm_to_use->CommRank();
    const auto thread_num_in_pure_comm =
            pure_comm_to_use->GetThreadNumInCommInProcess();

    BcastChannel* const bc = new BcastChannel(
            count, datatype, pure_rank_in_pure_comm, thread_num_in_pure_comm,
            root_pure_rank_in_pure_comm, channel_endpoint_tag, this);
    bc->CheckInvariants();
    parent_pure_process->BindBcastChannel(this, bc, root_pure_rank_in_pure_comm,
                                          channel_endpoint_tag,
                                          pure_comm_to_use);
    outstanding_channel_endpoints.push_back(bc);
    CHANNEL_INIT_TIMER_PAUSE;
    return bc;
}

ReduceChannel*
PureThread::InitReduceChannel(int count, const MPI_Datatype& datatype,
                              int root_pure_rank_in_comm, MPI_Op op,
                              channel_endpoint_tag_t channel_endpoint_tag,
                              PureComm* const        pure_comm) {

    CHANNEL_INIT_TIMER_START;
    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    ReduceChannel* const rc =
            new ReduceChannel(count, datatype, pure_comm_to_use->CommRank(),
                              root_pure_rank_in_comm, channel_endpoint_tag, op,
                              this, pure_comm_to_use);
    parent_pure_process->BindReduceChannel(this, rc, root_pure_rank_in_comm,
                                           channel_endpoint_tag,
                                           pure_comm_to_use);
    rc->CheckInvariants();
    outstanding_channel_endpoints.push_back(rc);
    CHANNEL_INIT_TIMER_PAUSE;
    return rc;
}

SendChannel* PureThread::GetOrInitSendChannel(
        int count, const MPI_Datatype datatype, int dest_pure_rank, int tag,
        bool using_rt_send_buf_hint, bool using_sized_enqueue_hint,
        size_t max_buffered_msgs, PureComm* const pure_comm) {

    // if(pure_rank <= 1) fprintf(stderr, KYEL "[r%d] GetOrInitSendChannel:
    // count %d  datatype %d  dest rank %d  tag %d\n" KRESET,
    //   pure_rank, count, datatype, dest_pure_rank, tag);

    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    // first, try to find it
    const ChannelMetadata test_key(count, datatype, dest_pure_rank, tag,
                                   pure_comm_to_use);
    const auto            iter = managed_send_channels.find(test_key);

    if (iter == managed_send_channels.end()) {
        SendChannel* sc = InitSendChannel(
                nullptr, count, datatype, dest_pure_rank, tag, BundleMode::none,
                CHANNEL_ENDPOINT_TAG_DEFAULT, USER_SPECIFIED_BUNDLE_SZ_DEFAULT,
                using_rt_send_buf_hint, using_rt_send_buf_hint,
                max_buffered_msgs, pure_comm_to_use);
        // purposely not waiting for init right now -- BundleChannels are
        // deprecated.
        managed_send_channels.insert({test_key, sc});
        return sc;
    } else {
#if DEBUG_CHECK
        const auto key = iter->first;
        assert(key.count == count);
        assert(key.datatype == datatype);
        assert(key.partner_pure_rank == dest_pure_rank);
        assert(key.tag == tag);
        assert(key.pure_comm == pure_comm_to_use);
#endif
        return iter->second;
    }
}

RecvChannel* PureThread::GetOrInitRecvChannel(int                count,
                                              const MPI_Datatype datatype,
                                              int sender_pure_rank, int tag,
                                              bool            using_rt_recv_buf,
                                              size_t          max_buffered_msgs,
                                              PureComm* const pure_comm) {

    // Note: using_rt_recv_buf is COMPLETELY unused

    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    // if(pure_rank <= 1) fprintf(stderr, KYEL_BG "[r%d] GetOrInitRecvChannel:
    // count %d  datatype %d  sender rank %d  tag %d\n" KRESET,
    //   pure_rank, count, datatype, sender_pure_rank, tag);
    const ChannelMetadata test_key(count, datatype, sender_pure_rank, tag,
                                   pure_comm_to_use);
    const auto            iter = managed_recv_channels.find(test_key);

    if (iter == managed_recv_channels.end()) {
        RecvChannel* rc = InitRecvChannel(
                count, datatype, sender_pure_rank, tag, using_rt_recv_buf,
                max_buffered_msgs, PurePipeline(0), pure_comm_to_use);
        managed_recv_channels.insert({test_key, rc});
        assert(rc != nullptr);
        rc->Validate();
        return rc;
    } else {
#if DEBUG_CHECK
        const auto key = iter->first;
        assert(key.count == count);
        assert(key.datatype == datatype);
        assert(key.partner_pure_rank == sender_pure_rank);
        assert(key.tag == tag);
        assert(key.pure_comm == pure_comm_to_use);
#endif
        assert(iter->second != nullptr);
        iter->second->Validate();
        return iter->second;
    }
}

BcastChannel* PureThread::GetOrInitBcastChannel(
        int count, const MPI_Datatype datatype, int root_pure_rank_in_pure_comm,
        channel_endpoint_tag_t channel_endpoint_tag, PureComm* pure_comm) {

    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    // using channel_endpoint_tag as tag in ChannelMetadata
    const ChannelMetadata test_key(count, datatype, root_pure_rank_in_pure_comm,
                                   channel_endpoint_tag, pure_comm_to_use);
    const auto            iter = managed_bcast_channels.find(test_key);

    if (iter == managed_bcast_channels.end()) {
        BcastChannel* bc =
                InitBcastChannel(count, datatype, root_pure_rank_in_pure_comm,
                                 channel_endpoint_tag, pure_comm_to_use);
        managed_bcast_channels.insert({test_key, bc});
        return bc;
    } else {
#if DEBUG_CHECK
        const auto key = iter->first;
        assert(key.pure_comm == pure_comm_to_use);
        assert(key.count == count);
        assert(key.datatype == datatype);
        assert(key.partner_pure_rank == root_pure_rank_in_pure_comm);
        assert(key.tag == channel_endpoint_tag);
#endif
        return iter->second;
    }
}

#if OLD_AR_CHAN
AllReduceChannel* PureThread::GetOrInitAllReduceChannel(
        int count, const MPI_Datatype datatype, MPI_Op op,
        channel_endpoint_tag_t reduce_cet, channel_endpoint_tag_t bcast_cet,
        PureComm* pure_comm) {

    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    // using partner_pure_rank --> reduce_cet
    // using tag -> bcast_cet
    const ChannelMetadata test_key(count, datatype, reduce_cet, bcast_cet,
                                   pure_comm_to_use, op);
    const auto            iter = managed_allreduce_channels.find(test_key);

    if (iter == managed_allreduce_channels.end()) {
        AllReduceChannel* arc =
                new AllReduceChannel(this, count, datatype, op, reduce_cet,
                                     bcast_cet, pure_comm_to_use);
        managed_allreduce_channels.insert({test_key, arc});
        return arc;
    } else {
#if DEBUG_CHECK
        const auto key = iter->first;
        assert(key.pure_comm == pure_comm_to_use);
        assert(key.count == count);
        assert(key.datatype == datatype);
        assert(key.partner_pure_rank == reduce_cet);
        assert(key.tag == bcast_cet);
        assert(key.op == op);
#endif
        return iter->second;
    }
}
#endif

template <typename T, unsigned int max_count>
AllReduceSmallPayloadProcess<T, max_count>*
PureThread::GetOrInitAllReduceChannel(int count, const MPI_Datatype datatype,
                                      MPI_Op                 op,
                                      channel_endpoint_tag_t reduce_cet,
                                      channel_endpoint_tag_t bcast_cet,
                                      PureComm*              pure_comm,
                                      seq_type               thread_seq) {

    sentinel(
            "deprecated. init() call isn't handled propertly. for now create "
            "channels directly and all threads should call init exactly once.");

    if (count == 1 && op == MPI_SUM) {
        sentinel("If you are doing a single sum, use MPI_DOUBLE and "
                 "pure/transport/experimental/allreduce_sum_one_double.h");
    }

    PureComm* const pure_comm_to_use = FIX_NULL_COMM_WORLD(pure_comm);

    // using partner_pure_rank --> reduce_cet
    // using tag -> bcast_cet
    const ChannelMetadata test_key(count, datatype, reduce_cet, bcast_cet,
                                   pure_comm_to_use, op);

    AllReduceSmallPayloadProcess<T, max_count>* matching_arc;

    {
        std::lock_guard<std::mutex> lg(
                parent_pure_process->managed_allreduce_channels_mutex);
        /////////////////////////////////////

        const auto iter =
                parent_pure_process->managed_allreduce_channels.find(test_key);

        if (iter == parent_pure_process->managed_allreduce_channels.end()) {
            matching_arc = new AllReduceSmallPayloadProcess<T, max_count>();
            matching_arc->Init(pure_comm_to_use, count, thread_seq);
            parent_pure_process->managed_allreduce_channels.insert(
                    {test_key, matching_arc});
        } else {
            // another thread created it
            matching_arc =
                    static_cast<AllReduceSmallPayloadProcess<T, max_count>*>(
                            iter->second);
        }
    }

    return matching_arc;
}

// explicit instantiations
template AllReduceSmallPayloadProcess<double>*
PureThread::GetOrInitAllReduceChannel<double>(
        int count, const MPI_Datatype datatype, MPI_Op op,
        channel_endpoint_tag_t reduce_cet, channel_endpoint_tag_t bcast_cet,
        PureComm* pure_comm, seq_type thread_seq);

template AllReduceSmallPayloadProcess<int>*
PureThread::GetOrInitAllReduceChannel<int>(
        int count, const MPI_Datatype datatype, MPI_Op op,
        channel_endpoint_tag_t reduce_cet, channel_endpoint_tag_t bcast_cet,
        PureComm* pure_comm, seq_type thread_seq);

template AllReduceSmallPayloadProcess<long long int>*
PureThread::GetOrInitAllReduceChannel<long long int>(
        int count, const MPI_Datatype datatype, MPI_Op op,
        channel_endpoint_tag_t reduce_cet, channel_endpoint_tag_t bcast_cet,
        PureComm* pure_comm, seq_type thread_seq);

// singles
template AllReduceSmallPayloadProcess<double, 1>*
PureThread::GetOrInitAllReduceChannel<double>(
        int count, const MPI_Datatype datatype, MPI_Op op,
        channel_endpoint_tag_t reduce_cet, channel_endpoint_tag_t bcast_cet,
        PureComm* pure_comm, seq_type thread_seq);

template AllReduceSmallPayloadProcess<int, 1>*
PureThread::GetOrInitAllReduceChannel<int>(
        int count, const MPI_Datatype datatype, MPI_Op op,
        channel_endpoint_tag_t reduce_cet, channel_endpoint_tag_t bcast_cet,
        PureComm* pure_comm, seq_type thread_seq);

template AllReduceSmallPayloadProcess<long long int, 1>*
PureThread::GetOrInitAllReduceChannel<long long int>(
        int count, const MPI_Datatype datatype, MPI_Op op,
        channel_endpoint_tag_t reduce_cet, channel_endpoint_tag_t bcast_cet,
        PureComm* pure_comm, seq_type thread_seq);

// creates a PureComm using semantics similar to MPI_Comm_split
PureComm* PureThread::PureCommSplit(int color, int key,
                                    PureComm* origin_pure_comm) {
    if (origin_pure_comm == nullptr) {
        origin_pure_comm = pure_comm_world;
    }
    return origin_pure_comm->Split(this, color, key);
}

MPI_Comm PureThread::GetOrCreateMpiCommFromRanks(const std::vector<int>& ranks,
                                                 MPI_Comm origin_comm) {
    return parent_pure_process->split_mpi_comm_manager.GetOrCreateCommFromRanks(
            this, ranks, origin_comm);
}

void PureThread::BarrierOld(PureComm const* const pure_comm,
                            bool do_work_stealing_while_waiting) {

    parent_pure_process->Barrier(
            this, barrier_sense, pure_comm->GetThreadNumInCommInProcess(),
            pure_comm->GetNumPureRanksThisProcess(),
            do_work_stealing_while_waiting, pure_comm->GetMpiComm(),
            pure_comm->GetProcessDetails());
}

// fix args
void PureThread::Barrier(PureComm* const pure_comm,
                         bool            do_work_stealing_while_waiting) {
    const auto root_thread_num = 0;
    pure_comm->Barrier(do_work_stealing_while_waiting);
}

void PureThread::AllChannelsInit() {
    // first, wait for all other threads to get here
    this->Barrier(pure_comm_world);
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // initialize random starting index for this thread
    // TODO: is this too expensive or does it slow down other threads?
#if PCV4_STEAL_VICTIM_RANDOM_PROBE
    // set up structures for random mode
    std::random_device                 device;
    std::mt19937                       generator(device());
    std::uniform_int_distribution<int> distribution(
            0, parent_pure_process->GetNumVictimThreads() - 1);
    curr_victim_idx = distribution(generator);
#endif

    // now that my PureProcess contains all of the victims, create my own
    // PureThread-local copy in sorted order; the sort key is the "NUMA
    // distance"

#if PCV4_STEAL_VICTIM_NUMA_AWARE
#ifdef LINUX
    assert(numa_available() == 0);
#endif
    // create a temporary vector with distances
    std::vector<std::pair<PureThread*, int>> thread_distances;

    // first, load up this vector with thread distances
    thread_distances.reserve(parent_pure_process->victim_threads.size());
    // make sure core is what we think it is
    assert(this->cpu_id == get_cpu_on_linux());
    const int  hyperthread_sib_distance = 5;
    const int  local_numa_node_distance = 10;
    const auto hyperthread_sibling =
            PureRT::thread_sibling_cpu_id(this->cpu_id);

    for (auto const& vt : parent_pure_process->victim_threads) {
        int distance;
        if (vt->cpu_id == hyperthread_sibling) {
            // numa_distance_of_cpus returns 10 for the same numa node, so
            // we just need to be below that
            distance = hyperthread_sib_distance;
        } else {
            distance = PureRT::numa_distance_of_cpus(this->cpu_id, vt->cpu_id);
            assert(distance >= local_numa_node_distance);
        }
        thread_distances.emplace_back(vt, distance);
    }

    std::sort(thread_distances.begin(), thread_distances.end(),
              [](auto& a, auto& b) { return a.second < b.second; });

    // const int target = 16;
    // if (this->cpu_id == target) {
    //   fprintf(stderr, KCYN "TARGET, VICTIM RANK, VICTIM CPU\n" KRESET);

    //     for (const auto& td : thread_distances) {
    //         const PureThread* t = td.first;
    //         const int         d = td.second;
    //         fprintf(stderr, KCYN "%d, %d, %d\n" KRESET,
    //                 target, t->Get_rank(), t->cpu_id);
    //     }
    // }

    // if (this->cpu_id == target) {
    //     for (const auto& vt : parent_pure_process->victim_threads) {
    //         printf("[for target rank %d] unsorted victim threads: rank %d
    //         (all victims size %lu)\n", target, vt->Get_rank(),
    //                parent_pure_process->victim_threads.size());
    //     }
    // }

    victim_threads_sorted.reserve(parent_pure_process->victim_threads.size());
    for (auto const& td : thread_distances) {
        // now, put these into the actual vector we will use for stealing.
        // don't put in "me", though
#if PCV4_ALLOW_CROSS_NUMA_STEAL
        if (td.first != this) {
#else
#if PCV4_STEAL_VICTIM_HYPERTHREAD_ONLY
        // only hyperthreads are relevant

        // fprintf(stderr, "cpu %d, distance %d\n", td.first->GetCpuId(),
        // td.second);
        if (td.first != this && td.second <= hyperthread_sib_distance) {
#else
        // don't allow cross numa steals -- just add victims that are on the
        // same numa node

        // TODO: verify that these are actually sorted in order!
        if (td.first != this && td.second <= local_numa_node_distance) {
#endif
#endif
            victim_threads_sorted.emplace_back(td.first);
        }
    }
    victim_threads_sorted.shrink_to_fit();
#endif // PCV4_STEAL_VICTIM_NUMA_AWARE

    // int j = 0;
    // if (this->cpu_id == target) {
    //     for (auto const& vts : victim_threads_sorted) {
    //         printf("cpu %d sorted_victims[%d]: victim cpu %d\n", target,
    //         j++,
    //                vts->GetCpuId());
    //     }
    //     fflush(stdout);
    //     sentinel("debug");
    // }

    assert(all_channels_init == false); // should only be called once
    this->all_channels_init = true;

#if PURE_PIPELINE_DIRECT_STEAL
    // now, go through each victim thread and put all of their pipelines
    // into my container for later stealing. DO NOT put my own pipelines
    // into my container.
    for (auto t : parent_pure_process->victim_threads) {
        if (t == this) {
            // we don't want to steal my own
            continue;
        }
        stealable_pure_pipelines.insert(stealable_pure_pipelines.begin(),
                                        t->GetStealablePipelines().begin(),
                                        t->GetStealablePipelines().end());
    }

    // randomize
    auto rng = std::default_random_engine{};
    std::shuffle(stealable_pure_pipelines.begin(),
                 stealable_pure_pipelines.end(), rng);
    stealable_pure_pipelines.shrink_to_fit();
#endif

    // this is currently to signal helper threads that it's ok to go
    parent_pure_process->MarkOneThreadInitialized();

    // Barrier again
    this->Barrier(pure_comm_world);
#endif // ends PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
       // IMPRORTANT: the first "real" iteration of the program and
       // associted timing should start timer just after this call.
}

#if PURE_PIPELINE_DIRECT_STEAL
std::vector<PurePipeline*>& PureThread::GetStealablePipelines() {
    return my_stealable_pure_pipelines;
}
#endif

BatcherManager*
PureThread::GetDequeueBatcherManager(int sender_mpi_rank) const {
    return parent_pure_process->GetDequeueBatcherManager(sender_mpi_rank);
}

BatcherManager*
PureThread::GetEnqueueBatcherManager(int receiver_mpi_rank) const {
    return parent_pure_process->GetEnqueueBatcherManager(receiver_mpi_rank);
}

// void PureThread::StopRecvFlatBatcher() {
//     parent_pure_process->StopRecvFlatBatcher();
// }

std::mutex& PureThread::GetApplicationMutex(const std::string name) {
    return parent_pure_process->GetApplicationMutex(name);
}

string PureThread::ToString() const {
    std::stringstream ss;
    ss << "mpi_process_rank: " << mpi_process_rank << std::endl;
    ss << "thread_num_in_mpi_process: " << thread_num_in_mpi_process
       << std::endl;
    ss << "num_mpi_processes: " << num_mpi_processes << std::endl;
    ss << "max_threads_per_mpi_process: " << max_threads_per_process
       << std::endl;
    ss << "pure_rank: " << pure_rank << std::endl;
    ss << "total_threads: " << total_threads << std::endl;

    return ss.str();
}

void PureThread::VerifyUniqueEndpoint(BundleChannelEndpoint* new_bce,
                                      PureRT::EndpointType   endpoint_type,
                                      int count, const MPI_Datatype& datatype,
                                      int sender_pure_rank, int dest_pure_rank,
                                      int tag, PureComm* const pure_comm) {

#if DEBUG_CHECK

    assert(endpoint_type == PureRT::EndpointType::SENDER ||
           endpoint_type == PureRT::EndpointType::RECEIVER);

    // first, assert that there isn't already a SendChannel/RecvChannel
    // with the same characteristics
    // TODO(jim): remove this for improved performance? add option in
    // makefile for checks?
    BundleChannelEndpointMetadata test_bcem{
            pure_comm, count, datatype, sender_pure_rank, dest_pure_rank, tag};

    bool        is_error = false;
    std::string channel_type;

    if (endpoint_type == PureRT::EndpointType::SENDER) {
        if (initialized_send_channels.find(test_bcem) !=
            initialized_send_channels.end()) {
            is_error     = true;
            channel_type = "send";
        } else {
            initialized_send_channels.insert(
                    {test_bcem, dynamic_cast<SendChannel*>(new_bce)});
        }

    } else {
        if (initialized_recv_channels.find(test_bcem) !=
            initialized_recv_channels.end()) {
            is_error     = true;
            channel_type = "recv";
        } else {
            initialized_recv_channels.insert(
                    {test_bcem, dynamic_cast<RecvChannel*>(new_bce)});
        }
    }

    if (is_error) {
        fprintf(stderr,
                "ERROR: Attempt to create %s channel that matched "
                "existing %s "
                "channel "
                "with the same "
                "characteristics (although possibly a different buffer "
                "pointer). This is not "
                "currently supported. A channel's uniqueness is "
                "defined by the "
                "set of {count, datatype, sender_rank, receiver_rank, "
                "tag}.",
                channel_type.c_str(), channel_type.c_str());
        fprintf(stderr, "\n%s", test_bcem.ToString().c_str());
        std::abort();
    }
#endif
}

void PureThread::RegisterCollabPipeline(PurePipeline& p) {
    assert(p.HasAnyCollabStage());
    // second arg indicates this is independent
    p.FinalizeConstruction(this, true);

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    parent_pure_process->RegisterVictimThread(this);
    parent_pure_process->RegisterIndependentPurePipeline(&p);
#endif
    // else we don't do any collaboration for non-work stealing modes

#if PURE_PIPELINE_DIRECT_STEAL
    // also, we need to populate my vector of stealable pipelines (including
    // this one)
    my_stealable_pure_pipelines.push_back(&p);
#endif

} // ends RegisterCollabPipeline

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//         WORK STEALING METHODS
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
void PureThread::SetActiveVictimPipeline(PurePipeline* const pp) {
    if (PCV4_APPR_2_DEBUG)
        fprintf(stderr,
                KCYN "[r%d] {PureThread %p} setting PP %p to active "
                     "stealable pipeline\n" KRESET,
                pure_rank, this, pp);

    // TODO: try this with seq cst

    pp_with_active_stealable_cont.store(pp, std::memory_order_release);
}

void PureThread::ClearActiveVictimPipeline() {
    if (PCV4_APPR_2_DEBUG) {
        char tname[16];
        pthread_name(tname);
        fprintf(stderr,
                KRED "      [%s] clearing PP %p (owned by r%d) as active "
                     "stealable pp\n" KRESET,
                tname, pp_with_active_stealable_cont.load(), pure_rank);
    }

    // It's possible that pc_with_active_stealable_cont will already be
    // nullptr in the case where a thread is already in
    // ExecuteDequeueContinuationUntilDone for a given PC from a
    // PREVIOUS iteration, then got descheduled by the OS, then comes
    // back in and starts right away because dequeue_cont_curr_chunk
    // gets reset to WORK_AVAIL_INIT_CHUNK by the receiver running
    // StartStealableWorkContinuation. That thread that was descheduled
    // and then rescheduled would then just start executing chunks and
    // possibly set the active PurePipeline in the recv_pure_thread_ to
    // nullptr, even though it is already nullptr. Then other threads
    // would try to help execute ExecuteDequeueContinuationUntilDone but
    // all chunks may already be done, so they would just exit. Then the
    // receiver would double check that the active PurePipeline has been
    // cleared to nullptr, which it would have been. I think this is ok
    // but quite hairy so keep a close eye on this as an area for
    // potential race conditions and logic errors.

    // assert(pc_with_active_stealable_cont.load() != nullptr);
    pp_with_active_stealable_cont.store(nullptr, std::memory_order_release);
}

PurePipeline* PureThread::GetActiveVictimPipeline() const {
#if USE_HELPER_THREADS_IF_FREE_CORES
    // for cases when helper threads are turned on, the threads may
    // get deallocated while a helper thread is here.
    try {
#endif
        // keep this consistent with below line
        return static_cast<PurePipeline* const>(
                pp_with_active_stealable_cont.load(std::memory_order_acquire));
#if USE_HELPER_THREADS_IF_FREE_CORES
    } catch (std::exception& e) {
        return nullptr;
    }
#endif
}

#if PURE_PIPELINE_DIRECT_STEAL
bool PureThread::MaybeExecuteStolenWorkOfRandomReceiver() {
    PT_STAT_INC(stat_num_work_steal_considerations);
    PURE_START_TIMER(this, pp_wait_collab_continuation_driver);

    if (!all_channels_init) {
        // on the first iteration, all channels weren't initialized yet
        // so just do nothing.
        return false;
    }

    const auto steal_attempted = true;
    const auto work_until_done = PCV4_OPTION_WORK_STEAL_EXHAUSTIVELY;

    // for (auto i = 0; i < stealable_pure_pipelines.size(); ++i) {
    const auto start_idx = stealable_pipeline_idx;

    while (true) {
        assert(stealable_pure_pipelines[stealable_pipeline_idx]
                       ->OwningThread() != this);
        const auto did_work = stealable_pure_pipelines[stealable_pipeline_idx]
                                      ->ExecuteDequeueContinuationUntilDone(
                                              this, work_until_done);

        if (did_work) {
            // if you did work, break out, but don't update the index (maybe
            // do it again next time)
            break;
        }

        if (stealable_pipeline_idx == stealable_pure_pipelines.size() - 1) {
            stealable_pipeline_idx = 0;
        } else {
            ++stealable_pipeline_idx;
        }

        if (stealable_pipeline_idx == start_idx) {
            // if you wrapped all the way around, break out
            break;
        }
    }

    PURE_PAUSE_TIMER(this, pp_wait_collab_continuation_driver, -1, this);
    return steal_attempted;
}

#else // not doing direct steal -- checking actively executing in pure thread
      // first

bool PureThread::MaybeExecuteStolenWorkOfRandomReceiver() {
    // TODO: try to optimize stealing from thread-local continuations
    // process channel, as the data may be more likely to be hot in the
    // cache.

    PT_STAT_INC(stat_num_work_steal_considerations);
    PURE_START_TIMER(this, pp_wait_collab_continuation_driver);

    if (!all_channels_init) {
        // on the first iteration, all channels weren't initialized yet
        // so just do nothing.
        return false;
    }

    const auto potential_victim_pp = this->GetVictimPipeline();
    auto       steal_attempted     = false;
    if (potential_victim_pp != nullptr) {

        // THIS IS A WORK IN PROGRESS -- WILL THIS ACTUALLY HELP? SHOULD
        // THIS BE AN OPTION OR JUST LEAVE IT AS FULLY EXHAUST? OR, MAKE IT
        // AN ENV VAR?

        const auto work_until_done = PCV4_OPTION_WORK_STEAL_EXHAUSTIVELY;
        potential_victim_pp->ExecuteDequeueContinuationUntilDone(
                this, work_until_done);
        // count this here (not in ExecuteDequeueContinuationUntilDone
        // as that includes the receiver's call to it as well
        steal_attempted = true;
        PT_STAT_INC(stat_num_work_steal_attempts);
    }
    // else there's no threads with ProcessChannels that currently have
    // work to be stolen
    PURE_PAUSE_TIMER(this, pp_wait_collab_continuation_driver, -1, this);
    return steal_attempted;
}
#endif

// put this in the application at the end of the program
void PureThread::ExhaustRemainingWork(bool is_helper_thread) {
    // TODO: counters from ProcessChannel. Punt for now?
    PURE_START_TIMER(this, exhaust_remaining_work);

    if (is_helper_thread == false) {
        const auto d = parent_pure_process->MarkOneThreadDone();
        if (d) {
            return;
        }
    }

#if DEBUG_CHECK
    if (is_helper_thread) {
        printf(KGRN "HELPER: pure rank %d running ExhaustRemainingWork (on CPU "
                    "%d)\n" KRESET,
               pure_rank, cpu_id);
    } else {
        printf(KYEL "MAIN THREAD: pure rank %d running "
                    "ExhaustRemainingWork (on CPU "
                    "%d)\n" KRESET,
               pure_rank, cpu_id);
    }
    fflush(stdout);
#endif

    while (!parent_pure_process->AllThreadsDone()) {
        const auto pc = this->GetVictimPipeline();
        if (pc != nullptr) {
            const auto work_until_done = true;
            pc->ExecuteDequeueContinuationUntilDone(this, work_until_done);
        }
    }
    PURE_PAUSE_TIMER(this, exhaust_remaining_work, -1, nullptr);
}

PurePipeline* PureThread::GetVictimPipeline() {
#if PCV4_STEAL_VICTIM_NUMA_AWARE
    // here we just loop through the victim pipelines, in
    // closeest-to-furthest order, until we find an actively-executing
    // pipeline

    // DEBUGGING
    // const int target = 40;
    // if (this->cpu_id == target) {
    //     printf("CPU %d: VICTIM THREADS (%d total)\n", target,
    //            victim_threads_sorted.size());
    //     int i = 0;
    //     for (const auto& vt : victim_threads_sorted) {
    //         printf("\t[victim try order %d]\tCPU ID: %d\n", i++,
    //                vt->GetCpuId());
    //     }
    // }
    // END DEBUGGING

    for (const auto& vt : victim_threads_sorted) {
        const auto pp = vt->GetActiveVictimPipeline();
        if (pp != nullptr) {
            return pp;
        }
    }
    return nullptr;
#else
    // v46*

    // HACK -- old version to remove / clean up
    return parent_pure_process->GetVictimPipeline(this);

    // we are putting this here to promote inlining
#if 0
    check(PCV4_STEAL_VICTIM_LINEAR_PROBE == 1,
          "for now, only PCV4_STEAL_VICTIM_LINEAR_PROBE is supported");
    check(USE_HELPER_THREADS_IF_FREE_CORES == 0,
          "for now, no helper cores supported");

    const auto victim_threads = parent_pure_process->GetVictimThreads();
    for (const auto& t : victim_threads) {
        if (t != this) {
            // don't steal from my own thread, at least relevant with
            // ExhaustRemainingWork
            const auto pp = t->GetActiveVictimPipeline();
            if (pp != nullptr) {
                return pp;
            }
        }
    }
    return nullptr;
#endif

#endif
}

#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
void PureThread::IncrementProcessChannelChunkCountDoneByMe(
        PurePipeline* const pp, unsigned int chunks_done) {
    for (auto& pair : pipeline_chunk_stats) {
        if (pp == pair.first) {
            pair.second += chunks_done;
            return;
        }
    }
    // else, there's no matchine one yet. insert it.
    pipeline_chunk_stats.emplace_back(pp, chunks_done);
}

uint64_t PureThread::ChunkCountForPP(PurePipeline const* const pp) const {
    for (const auto& pair : pipeline_chunk_stats) {
        if (pair.first == pp) {
            return pair.second;
        }
    }
    // didn't find it
    return 0;
}

void PureThread::PrintProcessChannelChunkStats() const {
    // FIXME
    // printf(KBOLD KGREY "Rank %d\n" KRESET, pure_rank);
    // for (const auto& pr : pipeline_chunk_stats) {
    //      const auto pp = pr.first;
    //     if (p->HasAnyCollabCont()) {
    //         printf("  ProcessChannel %p: ", pc);
    //         pc->PrintSenderReceiver();
    //         printf("\t%" PRIu64 "\n", pr.second);
    //     }
    // }
}

void PureThread::PrintIndependentPipelineChunkStats(PurePipeline* pp) const {
    parent_pure_process->PrintIndependentPipelineChunkStats(pp);
}

#endif // ends PRINT_PROCESS_CHANNEL_STATSS && PCV4_OPTION_EXECUTE_CONTINUATION
#endif // ends PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
