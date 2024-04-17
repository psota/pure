// Author: James Psota
// File:   allreduce_sum_one_double_iter_v4.h 

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

#ifndef ALL_REDUCE_SUM_ONE_DOUBLE_PROCESS_v4_H
#define ALL_REDUCE_SUM_ONE_DOUBLE_PROCESS_v4_H

#define REDUCE_DEBUG_PRINT 0

#ifndef REDUCE_PROCESS_IN_ORDER_V4
#define REDUCE_PROCESS_IN_ORDER_V4 1
#endif

#include "pure/support/helpers.h"
#include "pure/transport/mpi_pure_helpers.h"
#include <atomic>

using std::atomic;

using seq_type = std::uint_fast32_t;

template <typename T>
struct alignas(CACHE_LINE_BYTES) SequencedReduceThreadDataV4 {

    // line written by non leader
    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_elt_seq;
    atomic<T*>           thread_elt;
    atomic<unsigned int> done_copying_result_flag = 0;
    char                 _pad0[CACHE_LINE_BYTES - sizeof(atomic<seq_type>) -
               sizeof(atomic<T*>) - sizeof(atomic<unsigned int>)];

    // line written by leader
    alignas(CACHE_LINE_BYTES) atomic<seq_type> result_ptr_seq;
    atomic<T*> result_ptr;
    char       _pad1[CACHE_LINE_BYTES - sizeof(atomic<seq_type>) -
               sizeof(atomic<T*>)];
};

// There is one of these per Process, not thread!
template <typename T>
class AllReduceProcessV4 {

  public:
    AllReduceProcessV4() {}

    ~AllReduceProcessV4() {
        free(thread_data);
        free(process_reduce_buf_internal);
    }

    // every thread call this
    void Init(PureComm const* const pure_comm, size_t max_num_elts_arg,
              seq_type& thread_seq) {
        if (pure_comm->GetThreadNumInCommInProcess() == 0) {
            const auto num_threads_in_pure_comm =
                    pure_comm->GetNumPureRanksThisProcess();
            thread_data = jp_memory_alloc<1, CACHE_LINE_BYTES, false,
                                          SequencedReduceThreadDataV4<T>>(
                    sizeof(SequencedReduceThreadDataV4<T>) *
                    num_threads_in_pure_comm);
            memset(thread_data, 0,
                   num_threads_in_pure_comm *
                           sizeof(SequencedReduceThreadDataV4<T>));

            process_reduce_buf_internal =
                    jp_memory_alloc<1, CACHE_LINE_BYTES, false, T>(
                            max_num_elts_arg * sizeof(T));

            max_num_elts = max_num_elts_arg;

            datatype_bytes = sizeof(T);

            // this actually releases everyone else
            num_threads_done_computing.store(0, std::memory_order_release);
        } else {
            // non-leader -- wait until the above flag is set properly -- we are
            // dual-using this field
            while (num_threads_done_computing.load(std::memory_order_acquire) ==
                   -1) {
                x86_pause_instruction();
            }
        }

        thread_seq = 1;
    }

    // public wrappers
    void Reduce(PureThread* const pure_thread, PureComm const* const pure_comm,
                int pure_comm_root, T* const __restrict input_buf,
                T* const __restrict output_buf, unsigned int count_arg,
                seq_type& thread_seq) {
        const auto do_allreduce = false;
        ReduceImpl(pure_thread, pure_comm, pure_comm_root, input_buf,
                   output_buf, count_arg, thread_seq, do_allreduce);
    }

    void AllReduce(PureThread* const     pure_thread,
                   PureComm const* const pure_comm,
                   T* const __restrict input_buf,
                   T* const __restrict output_buf, unsigned int count_arg,
                   seq_type& thread_seq) {
        const auto do_allreduce = true;
        const auto fake_root    = 0;
        ReduceImpl(pure_thread, pure_comm, fake_root, input_buf, output_buf,
                   count_arg, thread_seq, do_allreduce);
    }

  private:
    //////////////// functions
    void ReduceImpl(PureThread* const     pure_thread,
                    PureComm const* const pure_comm, int pure_comm_root,
                    T* const __restrict input_buf,
                    T* const __restrict output_buf, unsigned int actual_count,
                    seq_type& thread_seq, bool do_allreduce) {

        // clean these up
        const int thread_num_in_pure_comm =
                pure_comm->GetThreadNumInCommInProcess();
        const int pure_comm_size_this_process =
                pure_comm->GetNumPureRanksThisProcess();

        int cache_lines = (datatype_bytes * actual_count) / CACHE_LINE_BYTES;
        if ((datatype_bytes * actual_count) % CACHE_LINE_BYTES != 0) {
            cache_lines += 1;
        }

        auto lines_per_thread =
                cache_lines / pure_comm->GetNumPureRanksThisProcess();
        if (lines_per_thread == 0) {
            // fix truncation
            lines_per_thread = 1;
        }
        const auto num_threads_computing =
                std::min(cache_lines, pure_comm->GetNumPureRanksThisProcess());

        assert(thread_num_in_pure_comm < pure_comm_size_this_process);

        assert(pure_comm_root == 0); // hack for now
        const auto root_thread_num = 0;
        const auto is_root = (thread_num_in_pure_comm == root_thread_num);

        // STEP 1
        ++thread_seq;

        // STEP: post my source buffer for computing threads to use
        thread_data[thread_num_in_pure_comm].thread_elt.store(
                input_buf, std::memory_order_relaxed);
        thread_data[thread_num_in_pure_comm].put_elt_seq.store(
                thread_seq, std::memory_order_release);

        // STEP: all threads reduce their chunks
        ReduceMyChunks(pure_thread, thread_num_in_pure_comm,
                       pure_comm_size_this_process, thread_seq, input_buf,
                       actual_count, cache_lines, lines_per_thread,
                       num_threads_computing);

        // todo = fix root
        if (is_root) {
            // STEP: leader waits for all chunks to be computed
            assert(thread_num_in_pure_comm == root_thread_num);

            // wait for all computing threads to be done
            // don't count leader, which is a computer, though
            if (num_threads_computing > 1) {
                // if more than just me computing
                while (num_threads_done_computing.load(
                               std::memory_order_acquire) !=
                       num_threads_computing - 1) {
                    x86_pause_instruction();
                    MAYBE_WORK_STEAL(pure_thread);
                }
            }

            // STEP: The reduction inside my process and pure comm is done,
            // but now do the intra-process reduction
#if MPI_ENABLED
            if (do_allreduce) {
                assert(output_buf != nullptr);
                const auto ret = MPI_Allreduce(
                        process_reduce_buf_internal, output_buf, actual_count,
                        PureRT::MPI_Datatype_from_typename<T>(), MPI_SUM,
                        pure_comm->GetMpiComm());
                assert(ret == MPI_SUCCESS);
            } else {
                const auto ret = MPI_Reduce(
                        process_reduce_buf_internal, output_buf, actual_count,
                        PureRT::MPI_Datatype_from_typename<T>(), MPI_SUM, 0,
                        pure_comm->GetMpiComm());
                assert(ret == MPI_SUCCESS);
            }
#else
            // if mpi didn't do the reduction (into the root's output buf), do
            // it here
            assert(pure_comm->CommRank() == pure_comm_root);
            std::memcpy(output_buf, process_reduce_buf_internal,
                        actual_count * sizeof(T));
#endif
            // upon leaving, the root has the result in output_buf
            ///////////////////////////////////////
            // reset for next time
            num_threads_done_computing.store(0, std::memory_order_release);

            // intra-process broadcast if necessary
            if (do_allreduce) {
                // 1. put pointer to reduced buffer
                reduce_result_ptr.store(output_buf, std::memory_order_release);

                // 2. update seq
                reduce_done_flag.store(thread_seq, std::memory_order_release);

                // 3. wait until all have consumed
#if COUNTER_VERSION
                const auto non_leader_threads = pure_comm_size_this_process - 1;
                while (num_threads_done_allreduce_bcast.load(
                               std::memory_order_acquire) !=
                       non_leader_threads) {
                    x86_pause_instruction();
                    MAYBE_WORK_STEAL(pure_thread);
                }
#else
                for (auto t = 1; t < pure_comm_size_this_process; ++t) {
                    while (thread_data[t].done_copying_result_flag.load(
                                   std::memory_order_acquire) !=
                           done_definition) {
                        x86_pause_instruction();
                        MAYBE_WORK_STEAL(pure_thread);
                    }
                }

                // flip for next use of this channel
                done_definition ^= 1;
#endif
                // reset
                num_threads_done_allreduce_bcast.store(
                        0, std::memory_order_release);

            } else {
                // just update seq to indicate that the reduce is done
                reduce_done_flag.store(thread_seq, std::memory_order_release);
            }

        } else {
            // not root ///////////////////////////
            // STEP: non-leader waits for leader to indicate completion

            while (reduce_done_flag.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
            }

            if (do_allreduce) {
                // 1. do copy out
                std::memcpy(output_buf,
                            reduce_result_ptr.load(std::memory_order_acquire),
                            actual_count * datatype_bytes);

                // 2. indicate done doing copy out

#if COUNTER_VERSION
                ++num_threads_done_allreduce_bcast;
#else
                // dropbox version
                thread_data[thread_num_in_pure_comm]
                        .done_copying_result_flag.fetch_xor(
                                1, std::memory_order_acq_rel);
#endif
            }
        }
    } // ReduceImpl function

    void ReduceMyChunks(PureThread* const pure_thread,
                        int               thread_num_in_process_in_comm,
                        int pure_ranks_this_comm_this_process, int thread_seq,
                        T* __restrict src_buf, unsigned int        actual_count,
                        unsigned int cache_lines, unsigned int lines_per_thread,
                        unsigned int num_threads_computing) {

        if (thread_num_in_process_in_comm >= num_threads_computing) {
            // STEP 2: exit early if I don't own any chunks (because the
            // buffer is small) there's no work for me to do for example, if
            // there are 4 cache lines worth of work, and 8 worker threads,
            // ranks 0-3 each do a single cache line and ranks 4-7 do none
#if REDUCE_DEBUG_PRINT
            fprintf(stderr, KYEL "\tthread %d not computing anything\n" KRESET,
                    thread_num_in_process_in_comm);
#endif
            return;
        }

        // STEP 3: calculate the indicies for this chunk, assuming the
        // buffer is aligned
        int low_idx = (thread_num_in_process_in_comm * lines_per_thread) *
                      CACHE_LINE_BYTES / datatype_bytes;

        // high_idx is inclusive
        int high_idx;

        // the last *active* thread should go all the way until the end
        if (((cache_lines < pure_ranks_this_comm_this_process) &&
             (thread_num_in_process_in_comm == cache_lines - 1)) ||
            (thread_num_in_process_in_comm ==
             pure_ranks_this_comm_this_process - 1)) {
            high_idx = actual_count - 1;
        } else {
            high_idx =
                    (((thread_num_in_process_in_comm + 1) * lines_per_thread) *
                     CACHE_LINE_BYTES / datatype_bytes) -
                    1;
        }
        ///////////////////////////

        T* const __restrict reduce_buf_start =
                &static_cast<T*>(process_reduce_buf_internal)[low_idx];
        assert_cacheline_aligned(reduce_buf_start);

        const T* const __restrict my_buf_start = &src_buf[low_idx];

        // STEP 3: copy in my data for this chunk, which also serves as a
        // starting point
        const int num_chunk_elts = high_idx - low_idx + 1;
        std::memcpy(reduce_buf_start, my_buf_start,
                    num_chunk_elts * datatype_bytes);

#if REDUCE_DEBUG_PRINT
        fprintf(stderr,
                "thread %d doing reduction [%d-%d]. num_chunk_elts=%d "
                "cache_lines "
                "= "
                "%d "
                " lines_per_thread = %d process_reduce_buf_internal %p  "
                "reduction_buf[%d] = "
                "%f\n",
                thread_num_in_process_in_comm, low_idx, high_idx,
                num_chunk_elts, cache_lines, lines_per_thread,
                process_reduce_buf_internal, low_idx,
                process_reduce_buf_internal[low_idx]);
#endif

        // STEP 4: loop through all the other threads in this process and
        // reduce them

// probably just hack in a threshold on this.
#if REDUCE_PROCESS_IN_ORDER_V4
        for (auto t = 0; t < pure_ranks_this_comm_this_process; ++t) {

            if (t == thread_num_in_process_in_comm) {
                // already put my data in above via memcpy
                continue;
            }

            while (thread_data[t].put_elt_seq.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
                MAYBE_WORK_STEAL(pure_thread);
            }

            T* const __restrict thread_buf =
                    thread_data[t].thread_elt.load(std::memory_order_acquire);
            for (auto i = low_idx; i <= high_idx; ++i) {
                process_reduce_buf_internal[i] += thread_buf[i];
            }
        }
#else
        // we want static initialization, so just make a bigger array than
        // we may need
        assert(pure_ranks_this_comm_this_process <= 64);
        bool thread_done[64] = {false};
        // bool thread_done[pure_ranks_this_comm_this_process] = {false};
        // std::memset(thread_done, 0,
        //             pure_ranks_this_comm_this_process * sizeof(bool));
        thread_done[thread_num_in_process_in_comm] = true;
        int threads_remain = pure_ranks_this_comm_this_process - 1;

        while (threads_remain > 0) {
            // approach: keep looping through all of the attached threads
            for (auto i = 0; i < pure_ranks_this_comm_this_process; ++i) {

                if (thread_done[i]) {
                    continue;
                }

                const auto other_thread_seq = thread_data[i].put_elt_seq.load(
                        std::memory_order_acquire);
                if (other_thread_seq == thread_seq) {
                    // buffer from thread i has been updated
                    const T* __restrict other_src_buf = static_cast<const T*>(
                            thread_data[i].thread_elt.load(
                                    std::memory_order_acquire));
                    assert(other_src_buf != nullptr);
                    ReduceChunk(other_src_buf, low_idx, high_idx);
                    thread_done[i] = true;
                    --threads_remain;
                }

            } // for
        }     // while
#endif

        // all computation by this thread is done, so indicate as such to
        // the leader thread
        if (thread_num_in_process_in_comm != 0) {
            // hack for 0 root only
            ++num_threads_done_computing; // the leader is watching this
        }
    }

    void ReduceChunk(const T* __restrict src_buf, int low_idx, int high_idx) {

        // hack fixme
        MPI_Op             op       = MPI_SUM;
        const MPI_Datatype datatype = PureRT::MPI_Datatype_from_typename<T>();

        T* const __restrict prb = static_cast<T*>(__builtin_assume_aligned(
                process_reduce_buf_internal, CACHE_LINE_BYTES));

        switch (op) {
        case MPI_SUM: {
            for (auto i = low_idx; i <= high_idx; ++i) {
                prb[i] += src_buf[i];
            }
            break;
        }
        case MPI_MAX: {
            assert(datatype == MPI_INT ||
                   datatype == MPI_DOUBLE); // only this is supported
                                            // currently given how
                                            // InitializeBuffer works
            for (auto i = low_idx; i <= high_idx; ++i) {
                if (src_buf[i] > prb[i]) {
                    prb[i] = src_buf[i];
                }
            }
            break;
        }
        case MPI_MIN: {
            assert(datatype == MPI_INT ||
                   datatype == MPI_DOUBLE); // only this is supported
                                            // currently given how
                                            // InitializeBuffer works
            for (auto i = low_idx; i <= high_idx; ++i) {
                if (src_buf[i] < prb[i]) {
                    prb[i] = src_buf[i];
                }
            }
            break;
        }
        default: {
            sentinel("MPI_Op %d not currently supported in "
                     "ReduceProcessChannel.",
                     op);
        }
        }
    }

    ////////////////////////////////////
    SequencedReduceThreadDataV4<T>* thread_data;
    T*                              process_reduce_buf_internal;
    size_t                          max_num_elts, datatype_bytes;

    //
    // int cache_lines, lines_per_thread, num_threads_computing;

    alignas(CACHE_LINE_BYTES) atomic<unsigned int> num_threads_done_computing =
            -1;
    alignas(CACHE_LINE_BYTES) atomic<seq_type> reduce_done_flag = -1;
    alignas(CACHE_LINE_BYTES) atomic<T*> reduce_result_ptr;
    alignas(CACHE_LINE_BYTES) int done_definition = 1;
    alignas(CACHE_LINE_BYTES)
            atomic<unsigned int> num_threads_done_allreduce_bcast = 0;
};

#endif