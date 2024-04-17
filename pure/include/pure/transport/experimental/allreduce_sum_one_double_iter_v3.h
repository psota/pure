// Author: James Psota
// File:   allreduce_sum_one_double_iter_v3.h 

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

#ifndef ALL_REDUCE_SUM_ONE_DOUBLE_PROCESS_H
#define ALL_REDUCE_SUM_ONE_DOUBLE_PROCESS_H

#define REDUCE_DEBUG_PRINT 0
#define DO_REDUCE_TIMING 0

/* How to generalize to all applications:
** this version incorporates chunk-based concurrent computation

  1. add support for more datatypes -- templatize
  2. use in ReduceChannel
  5. add support for more operations

DONE
  3. add support for arbitrary sizes via constructor
  4. point to buffer, not a dropbox apporach
  6. support for concurrent reductions

  */

#include "pure/support/helpers.h"
#include <atomic>

using std::atomic;

using seq_type = std::uint_fast32_t;

struct alignas(CACHE_LINE_BYTES) SequencedReduceThreadData {

    // line written by non leader
    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_elt_seq;
    atomic<double*>      thread_elt;
    atomic<unsigned int> done_copying_result_flag = 0;
    char                 _pad0[CACHE_LINE_BYTES - sizeof(atomic<seq_type>) -
               sizeof(atomic<double*>) - sizeof(atomic<unsigned int>)];

    // line written by leader
    alignas(CACHE_LINE_BYTES) atomic<seq_type> result_ptr_seq;
    atomic<double*> result_ptr;
    char            _pad1[CACHE_LINE_BYTES - sizeof(atomic<seq_type>) -
               sizeof(atomic<double*>)];
};

// There is one of these per Process, not thread!
// New Name: AllReduceSumOneDoubleProcessIter
class AllReduceSumOneDoubleProcessIter {

  public:
    AllReduceSumOneDoubleProcessIter()
        : wait_for_data("wait_for_data"), compute("compute"),
          store_flag("store_flag"), wait_for_consumption(""),
          t1_copy_and_incr(""), t1_inc_done_compute("") {}

    ~AllReduceSumOneDoubleProcessIter() {
        printf("reduce timers:\nwait:   \t%lld\ncompute:\t%lld\nstore:  "
               "\t%lld\nwait for "
               "consumption:\t%lld\nt1_copy_and_incr:\t%lld\nt1 "
               "incr done compute timer: %lld\n",
               wait_for_data.elapsed_cycles(), compute.elapsed_cycles(),
               store_flag.elapsed_cycles(),
               wait_for_consumption.elapsed_cycles(),
               t1_copy_and_incr.elapsed_cycles(),
               t1_inc_done_compute.elapsed_cycles());

        free(thread_data);
        free(process_reduce_buf_internal);
    }

    // every thread call this``
    template <typename T>
    void Init(PureComm const* const pure_comm, size_t num_elts_arg,
              seq_type& thread_seq) {
        if (pure_comm->GetThreadNumInCommInProcess() == 0) {
            const auto num_threads_in_pure_comm =
                    pure_comm->GetNumPureRanksThisProcess();
            thread_data = jp_memory_alloc<1, CACHE_LINE_BYTES, false,
                                          SequencedReduceThreadData>(
                    sizeof(SequencedReduceThreadData) *
                    num_threads_in_pure_comm);
            memset(thread_data, 0,
                   num_threads_in_pure_comm *
                           sizeof(SequencedReduceThreadData));

            process_reduce_buf_internal =
                    jp_memory_alloc<1, CACHE_LINE_BYTES, false, double>(
                            num_elts_arg * sizeof(double));

            num_elts = num_elts_arg;

            datatype_bytes = sizeof(T);
            cache_lines    = (datatype_bytes * num_elts) / CACHE_LINE_BYTES;
            if ((datatype_bytes * num_elts) % CACHE_LINE_BYTES != 0) {
                cache_lines += 1;
            }

            lines_per_thread =
                    cache_lines / pure_comm->GetNumPureRanksThisProcess();
            if (lines_per_thread == 0) {
                // fix truncation
                lines_per_thread = 1;
            }
            num_threads_computing = std::min(
                    cache_lines, pure_comm->GetNumPureRanksThisProcess());

            // this actually releases everyone else
            num_threads_done_computing.store(0, std::memory_order_release);
        } else {
            // non-leader -- wait until the above flag is set properly -- we are
            // dual-using the field
            while (num_threads_done_computing.load(std::memory_order_acquire) ==
                   -1) {
                x86_pause_instruction();
            }
        }

        thread_seq = 1;
    }

    // public wrappers
    void Reduce(PureThread* const pure_thread, PureComm const* const pure_comm,
                int pure_comm_root, double* const __restrict input_buf,
                double* const __restrict output_buf, seq_type& thread_seq) {
        const auto do_allreduce = false;
        ReduceImpl(pure_thread, pure_comm, pure_comm_root, input_buf,
                   output_buf, thread_seq, do_allreduce);
    }

    void AllReduce(PureThread* const     pure_thread,
                   PureComm const* const pure_comm,
                   double* const __restrict input_buf,
                   double* const __restrict output_buf, seq_type& thread_seq) {
        const auto do_allreduce = true;
        const auto fake_root    = 0;
        ReduceImpl(pure_thread, pure_comm, fake_root, input_buf, output_buf,
                   thread_seq, do_allreduce);
    }

  private:
    //////////////// functions

    void ReduceImpl(PureThread* const     pure_thread,
                    PureComm const* const pure_comm, int pure_comm_root,
                    double* const __restrict input_buf,
                    double* const __restrict output_buf, seq_type& thread_seq,
                    bool do_allreduce) {

        // clean these up
        const int thread_num_in_pure_comm =
                pure_comm->GetThreadNumInCommInProcess();
        const int pure_comm_size_this_process =
                pure_comm->GetNumPureRanksThisProcess();

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
        ReduceMyChunks<double>(pure_thread, thread_num_in_pure_comm,
                               pure_comm_size_this_process, thread_seq,
                               input_buf);

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
                        process_reduce_buf_internal, output_buf, num_elts,
                        MPI_DOUBLE, MPI_SUM, pure_comm->GetMpiComm());
                assert(ret == MPI_SUCCESS);
            } else {
                const auto ret = MPI_Reduce(
                        process_reduce_buf_internal, output_buf, num_elts,
                        MPI_DOUBLE, MPI_SUM, 0, pure_comm->GetMpiComm());
                assert(ret == MPI_SUCCESS);
            }
#else
            // if mpi didn't do the reduction (into the root's output buf), do
            // it here
            assert(pure_comm->CommRank() == pure_comm_root);
            std::memcpy(output_buf, process_reduce_buf_internal,
                        num_elts * sizeof(double));
#endif
            // upon leaving, the root has the result in output_buf
            ///////////////////////////////////////

            if (DO_REDUCE_TIMING)
                store_flag.start();
            // reset for next time
            num_threads_done_computing.store(0, std::memory_order_release);

            // intra-process broadcast if necessary
            if (do_allreduce) {
                // 1. put pointer to reduced buffer
                reduce_result_ptr.store(output_buf, std::memory_order_release);

                // 2. update seq
                reduce_done_flag.store(thread_seq, std::memory_order_release);

                if (DO_REDUCE_TIMING)
                    wait_for_consumption.start();
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
                if (DO_REDUCE_TIMING)
                    wait_for_consumption.pause();

                // reset
                num_threads_done_allreduce_bcast.store(
                        0, std::memory_order_release);

            } else {
                // just update seq to indicate that the reduce is done
                reduce_done_flag.store(thread_seq, std::memory_order_release);
            }

            if (DO_REDUCE_TIMING)
                store_flag.pause();

        } else {
            // not root ///////////////////////////
            // STEP: non-leader waits for leader to indicate completion

            while (reduce_done_flag.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
            }

            if (do_allreduce) {
                // 1. do copy out
                if (DO_REDUCE_TIMING && thread_num_in_pure_comm == 1)
                    t1_copy_and_incr.start();
                std::memcpy(output_buf,
                            reduce_result_ptr.load(std::memory_order_acquire),
                            num_elts * sizeof(double));

                // 2. indicate done doing copy out

#if COUNTER_VERSION
                ++num_threads_done_allreduce_bcast;
#else
                // dropbox version
                thread_data[thread_num_in_pure_comm]
                        .done_copying_result_flag.fetch_xor(
                                1, std::memory_order_acq_rel);
#endif

                if (DO_REDUCE_TIMING && thread_num_in_pure_comm == 1)
                    t1_copy_and_incr.pause();
            }
        }
    } // ReduceImpl function

    template <typename T>
    void ReduceMyChunks(PureThread* const pure_thread,
                        int               thread_num_in_process_in_comm,
                        int pure_ranks_this_comm_this_process, int thread_seq,
                        T* __restrict src_buf) {

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
            high_idx = num_elts - 1;
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
#if REDUCE_PROCESS_IN_ORDER
        for (auto t = 0; t < pure_ranks_this_comm_this_process; ++t) {

            if (t == thread_num_in_process_in_comm) {
                // already put my data in above via memcpy
                continue;
            }

            if (DO_REDUCE_TIMING && thread_num_in_process_in_comm == 0)
                wait_for_data.start();

            while (thread_data[t].put_elt_seq.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
                MAYBE_WORK_STEAL(pure_thread);
            }

            if (DO_REDUCE_TIMING && thread_num_in_process_in_comm == 0)
                wait_for_data.pause();

            if (DO_REDUCE_TIMING && thread_num_in_process_in_comm == 0)
                compute.start();

            double* const __restrict thread_buf =
                    thread_data[t].thread_elt.load(std::memory_order_acquire);

            // if (thread_num_in_process_in_comm == 1)
            //     fprintf(stderr, "r%d before: elt is %f\n",
            //             thread_num_in_process_in_comm,
            //             process_reduce_buf_internal[8]);

            for (auto i = low_idx; i <= high_idx; ++i) {
                process_reduce_buf_internal[i] += thread_buf[i];
            }

            // if (thread_num_in_process_in_comm == 1)
            //     fprintf(stderr,
            //             "r%d after: elt is %f after adding in %f (%p)\n",
            //             thread_num_in_process_in_comm,
            //             process_reduce_buf_internal[8], thread_buf[8],
            //             &thread_buf[8]);
            if (DO_REDUCE_TIMING && thread_num_in_process_in_comm == 0)
                compute.pause();
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
                    ReduceChunk<T>(other_src_buf, low_idx, high_idx);
                    thread_done[i] = true;
                    --threads_remain;
                }

            } // for
        }     // while
#endif

        // all computation by this thread is done, so indicate as such to
        // the leader thread
        if (DO_REDUCE_TIMING && thread_num_in_process_in_comm == 1)
            t1_inc_done_compute.start();

        if (thread_num_in_process_in_comm != 0) {
            // hack for 0 root only
            ++num_threads_done_computing; // the leader is watching this
        }

        if (DO_REDUCE_TIMING && thread_num_in_process_in_comm == 1)
            t1_inc_done_compute.pause();
    }

    template <typename T>
    void ReduceChunk(const T* __restrict src_buf, int low_idx, int high_idx) {

        // hack fixme
        MPI_Datatype datatype = MPI_DOUBLE;
        MPI_Op       op       = MPI_SUM;

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
    SequencedReduceThreadData* thread_data;
    double*                    process_reduce_buf_internal;
    size_t                     num_elts, datatype_bytes;
    int cache_lines, lines_per_thread, num_threads_computing;

    alignas(CACHE_LINE_BYTES) atomic<unsigned int> num_threads_done_computing =
            -1;
    alignas(CACHE_LINE_BYTES) atomic<seq_type> reduce_done_flag = -1;
    alignas(CACHE_LINE_BYTES) atomic<double*> reduce_result_ptr;
    alignas(CACHE_LINE_BYTES) int done_definition = 1;
    alignas(CACHE_LINE_BYTES)
            atomic<unsigned int> num_threads_done_allreduce_bcast = 0;

    // hack remove
    alignas(CACHE_LINE_BYTES) BenchmarkTimer wait_for_data, compute, store_flag,
            wait_for_consumption;

    alignas(CACHE_LINE_BYTES) BenchmarkTimer t1_copy_and_incr,
            t1_inc_done_compute;
};

#endif