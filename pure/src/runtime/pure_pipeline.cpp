// Author: James Psota
// File:   pure_pipeline.cpp 

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

#include "pure/common/pure_pipeline.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/benchmark_timer.h"
#include "pure/transport/process_channel.h"

#ifndef PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
#define PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB 0
#endif
#ifndef PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
#define PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL 0
#endif

// also defined in process channel v4 -- clean this up. just put into Makefile.
#define PCV4_APPR_2_DEBUG 0

// collab continuations are only possible if a collaborative contintinuation PCV
// is selected
constexpr static chunk_id_t WORK_AVAIL_INIT_CHUNK_INIT =
        std::numeric_limits<chunk_id_t>::min();

static PurePipeline EmptyPurePipeline(0);

// the different schemes use different starting variables for the initial chunk
// value
#if PCV4_OPTION_WS_SINGLE_CHUNK
constexpr static chunk_id_t WORK_AVAIL_INIT_CHUNK = -1;
#endif
#if PCV4_OPTION_WS_GSS
constexpr static chunk_id_t WORK_AVAIL_INIT_CHUNK = 0;
#endif

constexpr static auto collaborative_continuations_possible =
        PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB ||
        PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL;

PurePipeline::PurePipeline(int num_expected_stages) {
    if (num_expected_stages > 0) {
        pipeline.reserve(num_expected_stages);
        stage_ret_vals.reserve(num_expected_stages);
        for (auto i = 0; i < num_expected_stages; ++i) {
            // placeholders so we can write to it later
            stage_ret_vals.emplace_back(std::monostate());
        }
    }

#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
    collab_work_ready_for_sender.store(false);
    sender_done_cont_work.store(false);
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // here we set this to the max chunk value so stealing doesn't happen until
    // this gets lowered
    active_stage_num_chunks_done.store(0);
    dequeue_cont_curr_chunk.store(PCV_4_NUM_CONT_CHUNKS);

    // verify alignment (no false sharing)
    assert(reinterpret_cast<unsigned long>(&active_stage_num_chunks_done) -
                   reinterpret_cast<unsigned long>(&dequeue_cont_curr_chunk) >=
           CACHE_LINE_BYTES);
#endif
}

PurePipeline::PurePipeline(PurePipeline&& other) {
    if (this != &other) {
        pipeline                     = std::move(other.pipeline);
        owning_recv_thread           = other.owning_recv_thread;
        recv_buf_ptr_for_collab_cont = other.recv_buf_ptr_for_collab_cont;
        recv_payload_cnt_for_collab_cont =
                other.recv_payload_cnt_for_collab_cont;
        num_collab_stages = other.num_collab_stages;
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
        active_pipeline_stage = other.active_pipeline_stage.load();
#endif
        num_threads_this_process = other.num_threads_this_process;
        stage_ret_vals           = other.stage_ret_vals;
#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
        collab_work_ready_for_sender =
                other.collab_work_ready_for_sender.load();
        sender_done_cont_work = other.sender_done_cont_work.load();
#endif
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
        deq_pipeline_atomic_ret_v = other.deq_pipeline_atomic_ret_v.load();
#endif
    }
}

PurePipeline& PurePipeline::operator=(PurePipeline&& other) {
    if (this != &other) {
        pipeline                     = std::move(other.pipeline);
        owning_recv_thread           = other.owning_recv_thread;
        recv_buf_ptr_for_collab_cont = other.recv_buf_ptr_for_collab_cont;
        recv_payload_cnt_for_collab_cont =
                other.recv_payload_cnt_for_collab_cont;
        num_collab_stages = other.num_collab_stages;
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
        active_pipeline_stage = other.active_pipeline_stage.load();
#endif
        num_threads_this_process = other.num_threads_this_process;
        stage_ret_vals           = other.stage_ret_vals;
#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
        collab_work_ready_for_sender =
                other.collab_work_ready_for_sender.load();
        sender_done_cont_work = other.sender_done_cont_work.load();
#endif
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
        deq_pipeline_atomic_ret_v = other.deq_pipeline_atomic_ret_v.load();
#endif
    }
    return *this;
}

void PurePipeline::FinalizeConstruction(PureThread* const pt,
                                        bool              is_independent_arg) {
    int collab_stages = 0;
    assert(pipeline.size() <= std::numeric_limits<uint_fast8_t>::max());
    for (auto const& det : pipeline) {
        if (det.collab) {
            collab_stages++;
        }
    }
    num_collab_stages = collab_stages;
    pipeline.shrink_to_fit();
    stage_ret_vals.shrink_to_fit();
    owning_recv_thread       = pt;
    num_threads_this_process = pt->GetNumThreadsThisProcess();
    is_independent           = is_independent_arg;
}

PurePipeline::~PurePipeline() {
#if PRINT_PROCESS_CHANNEL_STATS && PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    if (is_independent && owning_recv_thread != nullptr) {
        owning_recv_thread->PrintIndependentPipelineChunkStats(this);
    }
#endif
}

////////////////////////////////////////////////////////////////

/*
 * to consider here:
 * make chunks optional
 * remove buf_ptr and payload count from ::Execute
 * wrap the extra_cont_params object up more nicely -- at least call it
 * something different
 */

// Intended for independent collaborative pipelines only
PureContRetArr&
PurePipeline::Execute(std::optional<void*> extra_cont_params_arg) {
    assert(!this->Empty());

    // this is the main "driver loop" of the pipeline stages
    PURE_START_TIMER(owning_recv_thread, pp_execute_ind_cont_wrapper);

    // Sanity check that the pipeline initialization doesn't accidentally add
    // too many stages (relative to what was initially allocated)
    assert(stage_ret_vals.size() == NumStages());

    for (auto stage = 0; stage < NumStages(); ++stage) {
        const PureContDetails& curr_cont_det = pipeline[stage];
        // only work stealing supported
        if (PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL &&
            collaborative_continuations_possible && curr_cont_det.collab) {
            extra_cont_params = extra_cont_params_arg;
            // write to process-accessable stage
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
            // removing this code to allow it to compile
            assert(owning_recv_thread != nullptr);
            active_pipeline_stage.store(stage);
            StartStealableWorkContinuation();
            stage_ret_vals[stage] =
                    deq_pipeline_atomic_ret_v.load(std::memory_order_acquire);

            // only store this if the stage is returning
            // consider adding back in later
            // if (std::holds_alternative<ReturningPureContinuation>(
            //             curr_cont_det.cont)) {
            //     stage_ret_vals[stage] = deq_pipeline_atomic_ret_v.load(
            //             std::memory_order_acquire);
            // } else {
            //     stage_ret_vals[stage] = -100.00; // hack -- see if this helps
            // }
#endif
        } else {
            // this continuation isn't collaborative or PCV doesn't
            // allow it to be -- receiver does it itself completely
            PURE_START_TIMER(owning_recv_thread, pp_execute_cont);
            if (std::holds_alternative<ReturningPureContinuation>(
                        curr_cont_det.cont)) {
                stage_ret_vals[stage] =
                        std::get<ReturningPureContinuation>(curr_cont_det.cont)(
                                nullptr, -1, PureRTContExeMode::RECEIVER_FULL,
                                -1, -1, extra_cont_params_arg);
            } else {
                assert(std::holds_alternative<VoidPureContinuation>(
                        curr_cont_det.cont));
                std::get<VoidPureContinuation>(curr_cont_det.cont)(
                        nullptr, -1, PureRTContExeMode::RECEIVER_FULL, -1, -1,
                        extra_cont_params_arg);
                assert(std::holds_alternative<std::monostate>(
                        stage_ret_vals[stage]));
            }
            PURE_PAUSE_TIMER(owning_recv_thread, pp_execute_cont, -1, this);

        } // ends else not collab
    }     // ends all stages of pipeline
    PURE_PAUSE_TIMER(owning_recv_thread, pp_execute_ind_cont_wrapper, -1, this);

    // stage_ret_values was filled throughout the above loop
    return stage_ret_vals;
} // ends Execute

PureContRetArr&
PurePipeline::ExecutePCPipeline(buffer_t           buf_ptr,
                                payload_count_type payload_count) {

    // DEBUG
    // char tname[16];
    // pthread_name(tname);
    // fprintf(stderr,
    //         "Pipeline ExecutePCPipeline, %d payload count\t%d bytes total\t%d
    //         " "bytes/chunk\t(assuming doubles))\t# CHUNKS: %d\n",
    //         payload_count, payload_count * sizeof(double),
    //         payload_count * sizeof(double) / PCV_4_NUM_CONT_CHUNKS,
    //         PCV_4_NUM_CONT_CHUNKS);
    // DEBUG

    assert(owning_recv_thread != nullptr);
    // TODO: should I optimize this - don't even call this function if there's
    // no continuation? Ideally this will be fixed when I make the continuation
    // optional in the InitRecvChannel call.
    assert(!this->Empty());

    if (collaborative_continuations_possible) {
        // Deposit copies of recv buf details for use in continuation.
        // consider not doing this and having the sender just read these
        // vaules directly from the relevant recv_channel_ fields (or
        // helper methods)
        recv_buf_ptr_for_collab_cont     = buf_ptr;
        recv_payload_cnt_for_collab_cont = payload_count;
    }

    // Sanity check that the pipeline initialization doesn't accidentally add
    // too many stages (relative to what was initially allocated)
    assert(stage_ret_vals.size() == NumStages());

    // this is the main "driver loop" of the pipeline stages

    for (auto stage = 0; stage < NumStages(); ++stage) {

        const PureContDetails& curr_cont_det = pipeline[stage];
        if (collaborative_continuations_possible && curr_cont_det.collab) {
#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB || \
        PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
            // write to process-accessable stage
            active_pipeline_stage.store(stage);

#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
            stage_ret_vals[stage] = StartSenderRecieverCollabContinuation();
#endif
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
            StartStealableWorkContinuation();
            stage_ret_vals[stage] =
                    deq_pipeline_atomic_ret_v.load(std::memory_order_acquire);
#endif
#endif
        } else {
            PURE_START_TIMER(owning_recv_thread, pp_execute_cont);
            // this continuation isn't collaborative or PCV doesn't
            // allow it to be -- receiver does it itself
            if (std::holds_alternative<ReturningPureContinuation>(
                        curr_cont_det.cont)) {
                stage_ret_vals[stage] = std::get<ReturningPureContinuation>(
                        curr_cont_det.cont)(buf_ptr, payload_count,
                                            PureRTContExeMode::RECEIVER_FULL,
                                            -1, -1, std::nullopt);
            } else {
                assert(std::holds_alternative<VoidPureContinuation>(
                        curr_cont_det.cont));
                std::get<VoidPureContinuation>(curr_cont_det.cont)(
                        buf_ptr, payload_count,
                        PureRTContExeMode::RECEIVER_FULL, -1, -1, std::nullopt);
            }
            PURE_PAUSE_TIMER(owning_recv_thread, pp_execute_cont, -1, this);
        } // ends else not collab
    }     // ends all stages of pipeline

    return stage_ret_vals;
} // ends Execute

/// pcv42
/// ////////////////////////////////////////////////////////////////////////
#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
// executes all collaborative stages of the pipeline
void PurePipeline::MaybeHelpReceiverWithContinuation(
        PureThread* helping_send_thread) {

    assert(NumCollabStages() > 0);

    if (collaborative_continuations_possible) {
        for (auto active_stage = 0; active_stage < NumCollabStages();
             ++active_stage) {

            PURE_START_TIMER(helping_send_thread,
                             pp_wait_collab_continuation_driver);

            // wait until I can start this stage
            PURE_START_TIMER(helping_send_thread,
                             pp_enq_wait_collab_cont_wait_for_work);
            while (!collab_work_ready_for_sender.load(
                    std::memory_order_acquire))
                ;
            PURE_PAUSE_TIMER(helping_send_thread,
                             pp_enq_wait_collab_cont_wait_for_work, -1, this);

            // give the sender the second half -- it's more likely to be a
            // cache hit because it more recently wrote that value
            constexpr chunk_id_t my_chunk = 1;
            static_assert(
                    my_chunk == PCV_4_NUM_CONT_CHUNKS - 1,
                    "my_chunk must be one less than PCV_4_NUM_CONT_CHUNKS");

            PURE_START_TIMER(helping_send_thread, pp_execute_cont);
            // the sender does the first half of the continuation
            // handle both returning value and non-returning value
            assert(active_pipeline_stage >= 0);
            assert(active_pipeline_stage < NumStages());
            const PureContDetails& cont_details =
                    pipeline[active_pipeline_stage];
            const PureContinuation cont = cont_details.cont;
            if (std::holds_alternative<ReturningPureContinuation>(cont)) {
                sender_cont_work_ret_v = std::get<ReturningPureContinuation>(
                        cont)(recv_buf_ptr_for_collab_cont,
                              recv_payload_cnt_for_collab_cont,
                              PureRTContExeMode::CHUNK, my_chunk, my_chunk,
                              extra_cont_params);
            } else {
                std::get<VoidPureContinuation>(cont)(
                        recv_buf_ptr_for_collab_cont,
                        recv_payload_cnt_for_collab_cont,
                        PureRTContExeMode::CHUNK, my_chunk, my_chunk,
                        extra_cont_params);
            }
            PURE_PAUSE_TIMER(helping_send_thread, pp_execute_cont, -1, this);

            // this invalidates the receiver's line -- optimize?
            collab_work_ready_for_sender.store(false,
                                               std::memory_order_relaxed);
            sender_done_cont_work.store(true, std::memory_order_release);
        }
        PURE_PAUSE_TIMER(helping_send_thread,
                         pp_wait_collab_continuation_driver, -1, this);
    }
}

PureContRetT PurePipeline::StartSenderRecieverCollabContinuation() {

    // 1. alert sender that they should do work now
    collab_work_ready_for_sender.store(true, std::memory_order_release);

    // 2. do my portion (first half of the continuation, as second half has
    // a greater chance of still being in senders cache, so we want the
    // sender to do that (unverified hypothesis)
    const chunk_id_t my_chunk = 0;

    PureContRetT recv_ret;
    PURE_START_TIMER(owning_recv_thread, pp_execute_cont);

    const PureContDetails& cont_details = pipeline[active_pipeline_stage];
    const PureContinuation cont         = cont_details.cont;

    if (std::holds_alternative<ReturningPureContinuation>(cont)) {
        recv_ret = std::get<ReturningPureContinuation>(cont)(
                recv_buf_ptr_for_collab_cont, recv_payload_cnt_for_collab_cont,
                PureRTContExeMode::CHUNK, my_chunk, my_chunk,
                extra_cont_params);
    } else {
        assert(std::holds_alternative<VoidPureContinuation>(cont));
        std::get<VoidPureContinuation>(cont)(recv_buf_ptr_for_collab_cont,
                                             recv_payload_cnt_for_collab_cont,
                                             PureRTContExeMode::CHUNK, my_chunk,
                                             my_chunk, extra_cont_params);
    }
    PURE_PAUSE_TIMER(owning_recv_thread, pp_execute_cont, -1, this);

    // 3. wait until sender done its portion
    PURE_START_TIMER(owning_recv_thread,
                     pp_deq_wait_collab_cont_for_helpers_to_finish);
    while (!sender_done_cont_work.load(std::memory_order_acquire))
        ;
    PURE_PAUSE_TIMER(owning_recv_thread,
                     pp_deq_wait_collab_cont_for_helpers_to_finish, -1, this);

    // 4. reset for next stage or iteration
    sender_done_cont_work.store(false, std::memory_order_release);

    // 5. combine results if returning type
    if (std::holds_alternative<ReturningPureContinuation>(cont) &&
        cont_details.reducer != nullptr) {
        // combine receiver and sender's values -- recv_ret and
        // sender_cont_work_ret_val (written by sender)
        return cont_details.reducer(sender_cont_work_ret_v, recv_ret);

    } else {
        return PureContRetT{};
    }
}
#endif

///////////////////////////////////////////////////////////////////////////////////
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL

// called by RECEIVER to start doing a dequeue continuation. sets arguments
// to allow others to start stealing work if they have time to do so
void PurePipeline::StartStealableWorkContinuation() {
    // -1. reset the return value from the previous call. note that during a
    // Wait, we are using the deq_pipeline_atomic_r potentially
    // multiple times, once for each stage of the dequeue pipeline. Then, at the
    // end of the wait call, we are returning the value of it, which should be
    // the proper last value as determined by proper pipeline construction.
    const PureContDetails& cont_details = pipeline[active_pipeline_stage];

    // HACK: we only currently support doubles
    if (std::holds_alternative<ReturningPureContinuation>(cont_details.cont)) {
        check(std::holds_alternative<double>(cont_details.reducer_init_val),
              "Currently only reduer_init_vals of doubles are supported. See "
              "notes in pure_pipeline.h regarding this if this doesn't work.");
        deq_pipeline_atomic_ret_v.store(
                std::get<double>(cont_details.reducer_init_val),
                std::memory_order_relaxed);
    }

    // 0. reset num chunks done for this pipeline stage
    active_stage_num_chunks_done.store(0, std::memory_order_relaxed);

    // this is what actually kicks off workers to start working on this pipeline
    // stage, so we do it just before the receiver gets started on work itself
    dequeue_cont_curr_chunk.store(WORK_AVAIL_INIT_CHUNK,
                                  std::memory_order_release);

    // printf("dequeue_cont_curr_chunk is %d -- and WORK_AVAIL_INIT_CHUNK is %d
    // "
    //        "before "
    //        "ExecuteDequeueContinuationUntilDone\n",
    //        dequeue_cont_curr_chunk.load(), WORK_AVAIL_INIT_CHUNK);

// TODO: possibly only set this if it isn't already this, which is may be if
// the previous pipeline stage was this PC
#if PURE_PIPELINE_DIRECT_STEAL == 0
    owning_recv_thread->SetActiveVictimPipeline(this);
#endif

    // 1. do the work myself, too
    const auto work_until_done = true;
    ExecuteDequeueContinuationUntilDone(owning_recv_thread, work_until_done);
    // 2. wait until all the work is actually done -- unless I did the last
    // chunk
    // chunk_id_t chunks_done_upon_getting_here =
    //         active_stage_num_chunks_done.load(std::memory_order_acquire);
    PURE_START_TIMER(owning_recv_thread,
                     pp_deq_wait_collab_cont_for_helpers_to_finish);
    while (active_stage_num_chunks_done.load(std::memory_order_acquire) <
           PCV_4_NUM_CONT_CHUNKS)
        ;
    assert(active_stage_num_chunks_done.load() == PCV_4_NUM_CONT_CHUNKS);
    PURE_PAUSE_TIMER(owning_recv_thread,
                     pp_deq_wait_collab_cont_for_helpers_to_finish, -1, this);

// it will usually be the case that the active process channel was
// cleared (to nullptr) in ExecuteDequeueContinuationUntilDone by the
// thread that did the last chunk. However, it's possible that another
// thread may have already cleared it when some other thread set it as
// an active channel even though all work was already done. See comment
// in PureThread::ClearActiveVictimProcessChannel.
#if PURE_PIPELINE_DIRECT_STEAL == 0
    if (owning_recv_thread->GetActiveVictimPipeline() != nullptr) {
        // TODO: possibly only call this if the next pipeline stage isn't
        // collaborative
        owning_recv_thread->ClearActiveVictimPipeline();
    }
#endif
    assert(active_stage_num_chunks_done.load() == PCV_4_NUM_CONT_CHUNKS);
    // at this point, the return value for this stage is
    // deq_pipeline_atomic_ret_v.
}

#if PCV4_OPTION_WS_SINGLE_CHUNK
bool PurePipeline::ExecuteDequeueContinuationUntilDone(
        PureThread* const running_pure_thread, bool work_until_done) {

    assert(NumStages() > 0);

    // with approach 1, the fundamental atomic "start work" condition is
    // defined by PurePipeline::MaybeHasActiveWorkToSteal, where the
    // dequeue_cont_curr_chunk value is the key notion of work status. This
    // is also a necessary condition for Approach 2, although approach 2
    // adds an additional constraint, which is that this ProcessChannel must
    // still be actively doing work.

    // experiment: possibly don't do this at all - just try to increment it and
    // see what happens

    const auto curr_chunk =
            dequeue_cont_curr_chunk.load(std::memory_order_acquire);
    if (curr_chunk >= PCV_4_NUM_CONT_CHUNKS) {
        // early exit -- all work is already done by the time the thief got
        // here. It happens to the best of them.
        if (PCV4_APPR_2_DEBUG) {
            char tname[16];
            pthread_name(tname);
            fprintf(stderr,
                    "#### [%s] exiting early from "
                    "ExecuteDequeueContinuationUntilDone as curr chunk is "
                    "already %d\n",
                    tname, curr_chunk);
        }
        return false;
    }

    check(dequeue_cont_curr_chunk.load() >= WORK_AVAIL_INIT_CHUNK,
          "  dequeue_cont_curr_chunk = %d has been allocated to me but "
          "should be greater than WORK_AVAIL_INIT_CHUNK=%d\n",
          dequeue_cont_curr_chunk.load(), WORK_AVAIL_INIT_CHUNK);
    int                    local_num_chunks_done = 0;
    const PureContDetails& cont_details      = pipeline[active_pipeline_stage];
    PureContRetT           local_reduced_val = cont_details.reducer_init_val;
    constexpr auto         last_chunk        = PCV_4_NUM_CONT_CHUNKS - 1;

    // TODO: probably don't calculate this here as sometimes the chunk lengths
    // are not a function of payload_count, but something else. Probably just
    // compute this in PipelineStageIndexRangeForChunks.

    PURE_START_TIMER(running_pure_thread, pp_execute_cont);
    do {
        // auto const my_chunk = ++dequeue_cont_curr_chunk; // atomic increment
        auto const my_chunk = dequeue_cont_curr_chunk.fetch_add(
                                      1, std::memory_order_release) +
                              1;

        if (PCV4_APPR_2_DEBUG) {
            char tname[16];
            pthread_name(tname);
            fprintf(stderr,
                    KLT_PUR "    [%s] trying to execute chunk %d for PC %p. "
                            "last_chunk = %d\n" KRESET,
                    tname, my_chunk, this, last_chunk);
        }

        assert(active_stage_num_chunks_done >= 0);
        assert(active_stage_num_chunks_done <= PCV_4_NUM_CONT_CHUNKS);
        check(my_chunk > WORK_AVAIL_INIT_CHUNK,
              "  my_chunk = %d has been allocated to me but should be "
              "greater than WORK_AVAIL_INIT_CHUNK=%d\n",
              my_chunk, WORK_AVAIL_INIT_CHUNK);

        if (my_chunk <= last_chunk) {
            // there's still more work to do. do it.
            check(my_chunk >= 0,
                  "  Expected my_chunk, which is %d, to be non-negative",
                  my_chunk);

            if (my_chunk == last_chunk) {
                // the thread that gets the last chunk should signal that
                // this ProcessChannel is no longer active. Note that the
                // calling thread may be different from the receiving thread
                // for this ProcessChannel (that thread may be off doing
                // something else or already done).
                if (PCV4_APPR_2_DEBUG) {
                    char tname[16];
                    pthread_name(tname);
                    fprintf(stderr,
                            KUNDER "    [%s] LAST CHUNK -- calling clear "
                                   "active PC on "
                                   "recv_pure_thread %p\n" KRESET,
                            tname, owning_recv_thread);
                }

                // TODO: possibly only call this if the next pipeline stage
                // isn't collaborative
                owning_recv_thread->ClearActiveVictimPipeline();
            }
            // execute my chunk
            // PERF TODO: this lambda isn't inlined, it seems. how can we
            // make that happen?

            // only populate cont_details if you are actually going to execute
            // the continuation
            if (std::holds_alternative<ReturningPureContinuation>(
                        cont_details.cont)) {
                // reduce locally and then write it at the end
                PureContRetT this_ret_val = std::get<ReturningPureContinuation>(
                        cont_details.cont)(recv_buf_ptr_for_collab_cont,
                                           recv_payload_cnt_for_collab_cont,
                                           PureRTContExeMode::CHUNK, my_chunk,
                                           my_chunk, extra_cont_params);
                // accumulate into local_reduced_val
                local_reduced_val =
                        cont_details.reducer(this_ret_val, local_reduced_val);
            } else {
                // no return value continuation
                std::get<VoidPureContinuation>(cont_details.cont)(
                        recv_buf_ptr_for_collab_cont,
                        recv_payload_cnt_for_collab_cont,
                        PureRTContExeMode::CHUNK, my_chunk, my_chunk,
                        extra_cont_params);
            }
            ++local_num_chunks_done;
            if (my_chunk == last_chunk) {
                // early exit condition #1 (accessing only thread-local
                // stack variable)
                break;
            }
        } else {
            // else, the last chunk was already done by the time the atomic
            // increment completed.
            // all work is done here so clean up and return
            break;
        }
    } while (work_until_done); // ends do/while

    // reduce local return value into shared return value
    if (std::holds_alternative<ReturningPureContinuation>(cont_details.cont) &&
        local_num_chunks_done > 0) {
        // atomically update the shared value
        while (1) {
            // HACK: we are currently only supporting doubles
            double curr_shared_val =
                    deq_pipeline_atomic_ret_v.load(std::memory_order_acquire);
            double new_shared_val = std::get<double>(
                    cont_details.reducer(local_reduced_val, curr_shared_val));
            if (deq_pipeline_atomic_ret_v.compare_exchange_weak(
                        curr_shared_val, new_shared_val)) {
                // update succeeded; exit the loop
                break;
            }
        }
    }

    // the object that we will use here is the PurePipeline being stolen
    // from (this)
    PURE_PAUSE_TIMER(running_pure_thread, pp_execute_cont, -1, this);

    if (local_num_chunks_done > 0) {
        // update how many chunks each thread did for this process channel
        active_stage_num_chunks_done += local_num_chunks_done;
#if PRINT_PROCESS_CHANNEL_STATS
        running_pure_thread->IncrementProcessChannelChunkCountDoneByMe(
                this, local_num_chunks_done);
#endif
    }

    const auto work_actually_done = (local_num_chunks_done > 0);
    return work_actually_done;
}
#endif // PCV4_OPTION_WS_SINGLE_CHUNK

#if PCV4_OPTION_WS_GSS // Guided Self-Scheduling from Polychronopoulos et al.
bool PurePipeline::ExecuteDequeueContinuationUntilDone(
        PureThread* const running_pure_thread, bool work_until_done) {

    assert(NumStages() > 0);

    // with approach 1, the fundamental atomic "start work" condition is
    // defined by PurePipeline::MaybeHasActiveWorkToSteal, where the
    // dequeue_cont_curr_chunk value is the key notion of work status. This
    // is also a necessary condition for Approach 2, although approach 2
    // adds an additional constraint, which is that this ProcessChannel must
    // still be actively doing work.

    // TODO: possibly remove this
    // EXPENSIVE #3!!!
    const auto curr_chunk =
            dequeue_cont_curr_chunk.load(std::memory_order_acquire);
    if (curr_chunk >= PCV_4_NUM_CONT_CHUNKS) {
        return false;
    }

    check(dequeue_cont_curr_chunk.load() >= WORK_AVAIL_INIT_CHUNK,
          "  dequeue_cont_curr_chunk = %d has been allocated to me but "
          "should be greater than WORK_AVAIL_INIT_CHUNK=%d\n",
          dequeue_cont_curr_chunk.load(), WORK_AVAIL_INIT_CHUNK);

    int                   local_num_chunks_done = 0;
    const PureContDetails cont_details      = pipeline[active_pipeline_stage];
    PureContRetT          local_reduced_val = cont_details.reducer_init_val;

    // PureContRetT local_reduced_val     = reducer_init_val;
    constexpr int last_chunk      = PCV_4_NUM_CONT_CHUNKS - 1;
    bool          did_last_chunk  = false;
    const bool thread_is_receiver = (running_pure_thread == owning_recv_thread);

    PURE_START_TIMER(running_pure_thread, pp_execute_cont);
    //////////////////////////////////////////////////////////////////////
    do {
        // another approach to try is compare-exchange
        // EXPENSEIVE #1!
        const auto dequeue_cont_curr_chunk_snapshot =
                dequeue_cont_curr_chunk.load(std::memory_order_acquire);
        if (dequeue_cont_curr_chunk_snapshot >= PCV_4_NUM_CONT_CHUNKS) {
            break;
        }

        const auto remaining_chunks =
                PCV_4_NUM_CONT_CHUNKS - dequeue_cont_curr_chunk_snapshot;
        // OPT: may want something less than num_threads_this_process
        // (possibly another parameter)

        // OPT: no cast or ceiling. just floor it.
        uint_fast16_t chunks_for_me =
                ceil(static_cast<float>(remaining_chunks) /
                     static_cast<float>(num_threads_this_process));
        if (thread_is_receiver) {
            chunks_for_me *= RECEIVER_CHUNK_ALLOC_WEIGHT;
        }
        if (chunks_for_me == 0) {
            chunks_for_me = 1;
        }
        // TODO: do this with memory order specifier
        // EXPENSIVE #1!!!
        // auto end_chunk = (dequeue_cont_curr_chunk += chunks_for_me) - 1;
        auto end_chunk = (dequeue_cont_curr_chunk += chunks_for_me) - 1;
        // note: we may fix up end_chunk later if its off the end
        auto const start_chunk = end_chunk - chunks_for_me + 1;
        check(start_chunk >= 0,
              "start_chunk = %d should be >= 0. end_chunk = "
              "%d\tchunks_for_me "
              "= %d",
              start_chunk, end_chunk, chunks_for_me);

        if (PCV4_APPR_2_DEBUG) {
            char tname[16];
            pthread_name(tname);
            fprintf(stderr,
                    KLT_PUR "    [%s] trying to execute %d chunk(s) (%d - "
                            "%d) for PC "
                            "%p\tchunks_for_me=%d  chunks_remaining=%d  "
                            "num_threads_this_process=%d\n" KRESET,
                    tname, chunks_for_me, start_chunk, end_chunk, this,
                    chunks_for_me, remaining_chunks, num_threads_this_process);
        }

        assert(active_stage_num_chunks_done.load() >= 0);
        assert(active_stage_num_chunks_done.load() <= PCV_4_NUM_CONT_CHUNKS);
        check(start_chunk >= WORK_AVAIL_INIT_CHUNK,
              "  start_chunk = %d has been allocated to me but should be "
              "greater than WORK_AVAIL_INIT_CHUNK=%d\n",
              start_chunk, WORK_AVAIL_INIT_CHUNK);

        if (start_chunk <= last_chunk) {
            // there's still more work to do. do it.
            check(start_chunk >= 0,
                  "  Expected start_chunk, which is %d, to be non-negative",
                  start_chunk);

            // start chunk is always that, even if dequeue_cont_curr_chunk
            // went above last_chunk N.B. the atomic dequeue_cont_curr_chunk
            // may have changed since we read it above. So, we must account
            // for doing too many chunks by only doing up to last_chunk
            // (inclusively) possibly move this after the if statement below
            if (end_chunk > last_chunk) {
                end_chunk = last_chunk;
            }
            assert(end_chunk >= 0);
            assert(end_chunk <= last_chunk);
            assert(static_cast<int>(end_chunk) -
                           static_cast<int>(start_chunk) >=
                   0);

            if (end_chunk == last_chunk) {
                // the thread that gets the last chunk should signal that
                // this ProcessChannel is no longer active. Note that the
                // calling thread may be different from the receiving thread
                // for this ProcessChannel (that thread may be off doing
                // something else or already done).
                if (PCV4_APPR_2_DEBUG) {
                    char tname[16];
                    pthread_name(tname);
                    fprintf(stderr,
                            KUNDER "    [%s] LAST CHUNK -- calling clear "
                                   "active PC on "
                                   "receiver pure_thread %p\n" KRESET,
                            tname, owning_recv_thread);
                }
                owning_recv_thread->ClearActiveVictimPipeline();
            }
            // execute my chunks
            // PERF TODO: this lambda isn't inlined, it seems. how can we
            // make that happen? does attribute flatten help?
            if (std::holds_alternative<ReturningPureContinuation>(
                        cont_details.cont)) {
                // reduce locally and then write it at the end
                PureContRetT this_ret_val =
                        std::get<ReturningPureContinuation>(cont_details.cont)(
                                recv_buf_ptr_for_collab_cont,
                                recv_payload_cnt_for_collab_cont,
                                PureRTContExeMode::CHUNK, start_chunk,
                                end_chunk, extra_cont_params);
                // accumulate into local_reduced_val
                local_reduced_val =
                        cont_details.reducer(this_ret_val, local_reduced_val);
            } else {
                // no return value continuation
                std::get<VoidPureContinuation>(cont_details.cont)(
                        recv_buf_ptr_for_collab_cont,
                        recv_payload_cnt_for_collab_cont,
                        PureRTContExeMode::CHUNK, start_chunk, end_chunk,
                        extra_cont_params);
            }

            // we have to do the math again here because we may have reduced
            // end_chunk above
            local_num_chunks_done += end_chunk - start_chunk + 1;
            // printf("d %d\n", local_num_chunks_done);

            if (end_chunk == last_chunk) {
                // early exit condition #1 (accessing only thread-local
                // stack variable)
                did_last_chunk = true;
                break;
            }

            // testing removing of this. this seems to be largely
            // net-negative as it resynchronizes this variable, which is
            // heavily changing. if (dequeue_cont_curr_chunk.load() >=
            // last_chunk) {
            //     // early exit condition #2: the shared atomic counter may
            //     have
            //     // been incremented by another thread and all work may be
            //     done
            //     break;
            // }
        } else {
            // else, the last chunk was already done by the time the atomic
            // increment completed.
            // all work is done here so clean up and return
            break;
        }
    } while (work_until_done);

// original one - before making this only work with a double reducer value
#if 0
    // reduce local return value into shared return value
    if (std::holds_alternative<ReturningPureContinuation>(cont_details.cont) &&
        local_num_chunks_done > 0) {
        // atomically update the shared value
        while (1) {
            PureContRetT curr_shared_val = deq_pipeline_atomic_ret_v;
            PureContRetT new_shared_val =
                    cont_details.reducer(local_reduced_val, curr_shared_val);
            if (deq_pipeline_atomic_ret_v.compare_exchange_weak(
                        curr_shared_val, new_shared_val)) {
                // update succeeded. return.
                break;
            }
        }
    }
#endif

    // reduce local return value into shared return value
    if (std::holds_alternative<ReturningPureContinuation>(cont_details.cont) &&
        local_num_chunks_done > 0) {
        // atomically update the shared value
        while (1) {
            // HACK: we are currently only supporting doubles
            double curr_shared_val =
                    deq_pipeline_atomic_ret_v.load(std::memory_order_acquire);
            double new_shared_val = std::get<double>(
                    cont_details.reducer(local_reduced_val, curr_shared_val));
            if (deq_pipeline_atomic_ret_v.compare_exchange_weak(
                        curr_shared_val, new_shared_val)) {
                // update succeeded; exit the loop
                break;
            }
        }
    }

    // the object that we will use here is the ProcessChannel being stolen
    // from (this)
    PURE_PAUSE_TIMER(running_pure_thread, pp_execute_cont, -1, this);

    if (local_num_chunks_done > 0) {
        // update how many chunks each thread did for this process channel
        // TODO: memory order
        // EXPENSIVE #2!!!
        active_stage_num_chunks_done.fetch_add(local_num_chunks_done,
                                               std::memory_order_release);
#if PRINT_PROCESS_CHANNEL_STATS
        running_pure_thread->IncrementProcessChannelChunkCountDoneByMe(
                this, local_num_chunks_done);
#endif

        const auto work_actually_done = (local_num_chunks_done > 0);
        return work_actually_done;
    }
}
#endif // PCV4_OPTION_WS_GSS
#endif // PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
