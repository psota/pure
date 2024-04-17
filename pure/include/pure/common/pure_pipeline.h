// Author: James Psota
// File:   pure_pipeline.h 

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

#ifndef PURE_PIPELINE_H
#define PURE_PIPELINE_H

#include "pure/common/pure_cont_details.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/helpers.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <variant>

// forward decls
class PureThread;

using namespace PureRT;
#define RECEIVER_CHUNK_ALLOC_WEIGHT 1

// using pure_chunk_range_index_t = uint_fast32_t;
// trying this to get rid of advisor warning
// using pure_chunk_range_index_t = int;
using pure_chunk_range_index_t = int64_t;
struct pure_chunk_range_t {
    pure_chunk_range_index_t min_idx;
    pure_chunk_range_index_t max_idx;
};

static const PureContRetArr static_empty_pure_cont_ret_arr =
        std::vector<PureContRetT>{};

// debugging helpers
#ifndef DO_PRINT_CONT_DEBUG_INFO
#define DO_PRINT_CONT_DEBUG_INFO 1
#endif

#if (DEBUG_CHECK || PROFILE_MODE) && DO_PRINT_CONT_DEBUG_INFO
// TSV
#define PRINT_CONT_DEBUG_INFO(name)                                            \
    {                                                                          \
        const auto __logfile = pure_thread_global->GetContDebugFile();         \
        fprintf(__logfile, "%d\t%s\t%d\t%s\t%s\t%hi\t%hi\t%ld\t%ld\t",         \
                pure_thread_global->Get_rank(),                                \
                (owning_rank == PURE_RANK) ? "OWNER" : "THIEF", owning_rank,   \
                name, (m == PureRTContExeMode::CHUNK ? "CHUNK" : "recv_full"), \
                start_chunk, end_chunk, range.min_idx, range.max_idx);         \
        print_pure_backtrace(__logfile, '\n');                                 \
        fprintf(__logfile, "\n");                                              \
    }

#define PRINT_CONT_DEBUG_INFO_COLORED(name)                                 \
    {                                                                       \
        const auto __logfile = pure_thread_global->GetContDebugFile();      \
        fprintf(__logfile,                                                  \
                KUNDER "%s"                                                 \
                       "[r%d] {owner: r%d} %s: mode: %s\tchunk "            \
                       "range: [%hi - "                                     \
                       "%hi]\tindex range: [%ld - %ld]\t%s\n" KRESET,       \
                ((owning_rank == pure_thread_global->Get_rank()) ? KRED     \
                                                                 : KGRN),   \
                pure_thread_global->Get_rank(), owning_rank, name,          \
                (m == PureRTContExeMode::CHUNK ? "CHUNK" : "recv_full"),    \
                start_chunk, end_chunk, range.min_idx, range.max_idx,       \
                (owning_rank == pure_thread_global->Get_rank()) ? "OWNER"   \
                                                                : "THIEF"); \
        print_pure_backtrace(__logfile, '\n');                              \
        fprintf(__logfile, "\n");                                           \
    }

#else
#define PRINT_CONT_DEBUG_INFO(name)
#define PRINT_CONT_DEBUG_INFO_COLORED(name)
#endif

class PurePipeline {
  public:
    // we make this directly accessible so we can use vector
    // methods (e.g., iterators) directly
    std::vector<PureContDetails> pipeline;

  private:
    PureContRetArr stage_ret_vals;

    // SENDER-WRITTEN FIELDS
    // //////////////////////////////////////////////////
#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
    PureContRetT          sender_cont_work_ret_v{};
    std::atomic<unsigned> active_pipeline_stage = 0;
#endif
    // RECEIVER-WRITTEN FIELDS
    // /////////////////////////////////////////////////
    // written once by the PurePipeline constructor
    // any user of the standard constructor is responsible
    // for filling in this field.
    PureThread* owning_recv_thread = nullptr;

    ////////////////////////////////////////////////////////////////////////
    // bad ping pong sharing CACHE LINE OF DEATH, used only
    // with conttinuation
    alignas(CACHE_LINE_BYTES) buffer_t recv_buf_ptr_for_collab_cont = nullptr;
    std::optional<void*> extra_cont_params                = std::nullopt;
    payload_count_type   recv_payload_cnt_for_collab_cont = -1;

    // make sure to call FinalizeConstruction before using
    // this field
    // TODO: set this in finalize construction and then use
    // it in ExecuteUntildone
    int num_threads_this_process = -1;

    uint_fast8_t num_collab_stages = 0;
    bool         is_independent    = false; // not connected to a ProcessChannel

#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
    // deprecated option
    std::atomic<bool> collab_work_ready_for_sender;
    std::atomic<bool> sender_done_cont_work;
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // this is the FINAL return value of the pipeline that
    // gets returned via Wait

    // TODO: change name
    // std::atomic<PureContRetT> deq_pipeline_atomic_ret_v;
    // HACK: based on challenges with the Intel compiler not
    // allowing atomic variants, and we only use doubles (so
    // far) in the program, just use a double here and assert
    // it. IF needed, extend this later with a std::mutex and a
    // variant.
    alignas(CACHE_LINE_BYTES) std::atomic<double> deq_pipeline_atomic_ret_v;

    // TODO: analyze with perf c2c and vtune - may be more
    // to do here
    // TODO: change dequeue_cont_curr_chunk name
    alignas(CACHE_LINE_BYTES) std::atomic<chunk_id_t> dequeue_cont_curr_chunk;
    alignas(CACHE_LINE_BYTES) std::atomic<unsigned> active_pipeline_stage = 0;

    // avoid false-sharing, even though there's probably
    // only a short interval of time where these two should
    // interfere with each other towards the end of a work
    // loop
    alignas(CACHE_LINE_BYTES)
            std::atomic<chunk_id_t> active_stage_num_chunks_done;
    char pad1[CACHE_LINE_BYTES - sizeof(std::atomic<chunk_id_t>)];
#endif

    /////////////////////////////////////////////////////////////
  public:
    PurePipeline(int num_expected_stages = 0);
    PurePipeline(const PurePipeline& other) = delete;

    // IMPORTANT: keep move constructor and assignment
    // operator up to date as fields change and are added.
    // make sure to keep preprocessor directives.
    PurePipeline(PurePipeline&&);
    PurePipeline& operator=(PurePipeline&&);

    ~PurePipeline();

    // work-stealing-related functions
    // ///////////////////////////
    PureContRetArr&
                    Execute(std::optional<void*> extra_cont_params = std::nullopt);
    PureContRetArr& ExecutePCPipeline(buffer_t           buf_ptr,
                                      payload_count_type payload_count);

#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB // 42
    void         MaybeHelpReceiverWithContinuation(PureThread* send_thread);
    PureContRetT StartSenderRecieverCollabContinuation();
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    void StartStealableWorkContinuation();
    bool ExecuteDequeueContinuationUntilDone(PureThread* const running_thread,
                                             bool);
#endif
    //////////////////////////////////////////////////////////////

    // General helpers for pipeline

    inline PureContDetails&      operator[](int idx) { return pipeline[idx]; }
    inline const PureContRetArr& GetReturnVals() const {
        return stage_ret_vals;
    }

    inline bool HasAnyCollabStage() const {
        auto ret = std::any_of(
                pipeline.begin(), pipeline.end(),
                [](const PureContDetails& pcd) { return pcd.collab; });
        return ret;
    }
    inline PureThread*  OwningThread() const { return owning_recv_thread; }
    inline bool         IsIndependent() const { return is_independent; }
    inline uint_fast8_t NumStages() const { return pipeline.size(); }
    inline uint_fast8_t NumCollabStages() const { return num_collab_stages; }
    inline uint_fast8_t Empty() const { return NumStages() == 0; }
    void                FinalizeConstruction(PureThread* const pt,
                                             bool              is_independent = false);

    // total_range_len is the number of indicies in the same
    // domain as the ultimately returned pure_chunk_range_t
    // note: this recalcuates default_chunk_len; TODO:
    // consider removing from the continuation call return
    // values is usually min and max indicies, such as
    // "min_j" and "max_j" in a loop
    template <typename array_type>
    inline static pure_chunk_range_t PipelineStageCachelineAlignedIndexRange(
            int total_range_len, chunk_id_t start_chunk, chunk_id_t end_chunk) {

        constexpr chunk_id_t last_chunk = PCV_4_NUM_CONT_CHUNKS - 1;
        assert(end_chunk <= last_chunk);

        // Approach: first compute everything based on cache
        // lines, and then convert to indicies at the end.
        constexpr auto elements_per_cacheline =
                CACHE_LINE_BYTES / sizeof(array_type);
        // const int default_chunk_len = total_range_len /
        // PCV_4_NUM_CONT_CHUNKS;
        const int cachelines_per_chunk = total_range_len /
                                         elements_per_cacheline /
                                         PCV_4_NUM_CONT_CHUNKS;
        check(cachelines_per_chunk > 0,
              "The number of cachelines per chunk is less "
              "than 1, which means "
              "PCV_4_NUM_CONT_CHUNKS is set too high. Try "
              "lowering it (it's "
              "currently %d). total_range_len = %d, "
              "elements_per_cacheline = "
              "%d",
              PCV_4_NUM_CONT_CHUNKS, total_range_len, elements_per_cacheline);
        uint_fast32_t total_cachelines_to_do;
        uint_fast32_t extra_indicies_to_do = 0; // only used for last chunk

        if (end_chunk == last_chunk) {
            // account for last chunk differently
            // first, add up everything but the last chunk
            total_cachelines_to_do =
                    (end_chunk - start_chunk) * cachelines_per_chunk;

            // now, add in the last chunk specially
            // total_cachelines_to_do +=
            //         total_range_len - (last_chunk *
            //         cachelines_per_chunk);
            extra_indicies_to_do =
                    total_range_len - (last_chunk * cachelines_per_chunk *
                                       elements_per_cacheline);
        } else {
            // vanilla accounting
            total_cachelines_to_do =
                    (end_chunk - start_chunk + 1) * cachelines_per_chunk;
        }

        pure_chunk_range_t range;
        range.min_idx =
                start_chunk * cachelines_per_chunk * elements_per_cacheline;
        range.max_idx = range.min_idx +
                        (total_cachelines_to_do * elements_per_cacheline) - 1 +
                        extra_indicies_to_do;

        assert(range.min_idx >= 0);
        assert(range.max_idx <= total_range_len);
        return range;
    }

    // NOT cacheline-ailgned -- prefer
    // PipelineStageCachelineAlignedIndexRange when possible
    // use offset if, for example, the original index of the array starts at 1
    inline static pure_chunk_range_t PipelineStageUnalignedIndexRange(
            int total_range_len, chunk_id_t start_chunk, chunk_id_t end_chunk,
            unsigned int offset = 0) {

        pure_chunk_range_t range;

        constexpr chunk_id_t last_chunk = PCV_4_NUM_CONT_CHUNKS - 1;
        assert(end_chunk <= last_chunk);

        const int default_chunk_len = total_range_len / PCV_4_NUM_CONT_CHUNKS;
        check(default_chunk_len > 0,
              "PCV_4_NUM_CONT_CHUNKS (%d) seems to be too "
              "high given that the "
              "total range length is only %d. Consider "
              "lowering "
              "PCV_4_NUM_CONT_CHUNKS.",
              PCV_4_NUM_CONT_CHUNKS, total_range_len);
        uint_fast32_t total_iters_to_do;

        if (end_chunk == last_chunk) {
            // account for last chunk differently
            // first, add up everything but the last chunk
            total_iters_to_do = (end_chunk - start_chunk) * default_chunk_len;

            // now, add in the last chunk specially
            total_iters_to_do +=
                    total_range_len - (last_chunk * default_chunk_len);
        } else {
            // vanilla accounting
            total_iters_to_do =
                    (end_chunk - start_chunk + 1) * default_chunk_len;
        }

        range.min_idx = start_chunk * default_chunk_len + offset;
        range.max_idx = range.min_idx + total_iters_to_do - 1;

        assert(range.min_idx >= offset);
        assert(range.max_idx <= total_range_len + offset);

        return range;
    }
};

#endif