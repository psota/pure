// Author: James Psota
// File:   direct_mpi_channel.cpp 

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


#include "pure/transport/direct_mpi_channel.h"
#include "pure/common/pure_pipeline.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/helpers.h"
#include "pure/transport/mpi_pure_helpers.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/send_channel.h"
#include <optional>

#include "pure.h"

#if PRINT_PROCESS_CHANNEL_STATS
#define INC_DC_COUNTER(c) ++c;
#else
#define INC_DC_COUNTER(c)
#endif

#if COLLECT_THREAD_TIMELINE_DETAIL

#define DC_START_TIMER(t) _pure_thread->t.start();
#define DC_PAUSE_TIMER(t) _pure_thread->t.pause(-1);
#else
#define DC_START_TIMER(t)
#define DC_PAUSE_TIMER(t)
#endif

DirectMPIChannel::DirectMPIChannel(BundleChannelEndpoint* const bce,
                                   int mate_mpi_rank_in_comm, MPI_Comm mpi_comm,
                                   int sender_thread_num_within_world_comm,
                                   int receiver_thread_num_within_world_comm,
                                   PureRT::EndpointType endpoint_type,
                                   bool                 using_rt_send_buf_hint,
                                   bool           using_sized_enqueues_hint,
                                   bool           using_rt_recv_buf,
                                   PurePipeline&& deq_pipe)
    : _mpi_comm(mpi_comm), _pure_thread(bce->GetPureThread()),
      _endpoint_type(endpoint_type), _count(bce->GetCount()),
      _datatype(bce->GetDatatype()),
      _mate_mpi_rank_in_comm(mate_mpi_rank_in_comm),
      _using_rt_send_buf_hint(using_rt_send_buf_hint),

      _using_sized_enqueues_hint(using_sized_enqueues_hint),
      _using_rt_recv_buf(using_rt_recv_buf),
      _sender_thread_num_within_world_comm(sender_thread_num_within_world_comm),
      _receiver_thread_num_within_world_comm(
              receiver_thread_num_within_world_comm),
      _dequeue_pipeline(std::move(deq_pipe)) {

    _encoded_tag = ConstructTagForMPI(sender_thread_num_within_world_comm,
                                      receiver_thread_num_within_world_comm,
                                      bce->GetTag(), _mpi_comm);

    if (endpoint_type == PureRT::EndpointType::SENDER) {
        if (using_rt_send_buf_hint) {

            // 1. make this buffer cache-line-aligned
            // 2. make this buffer a multiple of cache lines
            // ORIGINAL: clean this out: _rt_buf = new
            // char[bce->NumPayloadBytes()];
            allocated_size_bytes = jp_memory_alloc<1, CACHE_LINE_BYTES, true>(
                    &_rt_buf, bce->NumPayloadBytes());
            assert_cacheline_aligned(_rt_buf);
            assert(allocated_size_bytes % CACHE_LINE_BYTES == 0);
        } else {
            _rt_buf              = nullptr;
            allocated_size_bytes = 0;
        }

        bce->SetSendChannelBuf(_rt_buf);

        // initialize right in constructor, as no WaitForInitialization is
        // necessary for DirectMPIChannel
        SendChannel* sc = dynamic_cast<SendChannel*>(bce);

        if (_using_rt_send_buf_hint && !_using_sized_enqueues_hint) {
            // set up persistent send request if using runtime buffer
            // NOTE: TODO: have to only allow this if the count is going to be
            // consistent throughout the lifetime of this DirectMPIChannel.
            // also can only do this if not using runtime-specified sized
            // enqueues via EnqueueUserBuf
            int ret = MPI_Send_init(_rt_buf, _count,
                                    static_cast<unsigned int>(_datatype),
                                    _mate_mpi_rank_in_comm, _encoded_tag,
                                    _mpi_comm, &enqueue_request);
            assert(ret == MPI_SUCCESS);
        }

        sc->SetInitializedState();
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {

        if (_using_rt_recv_buf) {
            assert(_using_rt_recv_buf == using_rt_recv_buf);
            _rt_buf = jp_memory_alloc<1, CACHE_LINE_BYTES, true>(
                    bce->NumPayloadBytes(), &allocated_size_bytes);

            if (reinterpret_cast<uintptr_t>(_rt_buf) % CACHE_LINE_BYTES != 0) {
                fprintf(stderr,
                        "tried to allocate aligned %d bytes but it's not "
                        "aligned (buf: %p)\n",
                        bce->NumPayloadBytes(), _rt_buf);
            }

            assert_cacheline_aligned(_rt_buf);
            assert(allocated_size_bytes % CACHE_LINE_BYTES == 0);

            // the rt buf is only relevant when using the rt recv buf
            bce->SetRecvChannelBuf(_rt_buf);
            dequeue_requests.set_capacity(1);
        } else {
            dequeue_requests.set_capacity(
                    CACHE_LINE_BYTES / sizeof(MPI_Request) *
                    INITIAL_OUTSTANDING_CHANNEL_DEQ_REQ_CLS);
        }

        // can use persistent endpoint for receiver even if sender isn't using
        // it.
        // int ret = MPI_Recv_init(
        //         _rt_buf, _count, static_cast<unsigned int>(_datatype),
        //         _mate_mpi_rank_in_comm, _encoded_tag, _mpi_comm,
        //         &dequeue_request);
        // assert(ret == MPI_SUCCESS);

        // initialize right in constructor, as no WaitForInitialization is
        // necessary for DirectMPIChannel
        _recv_channel = dynamic_cast<RecvChannel*>(bce);
        _recv_channel->SetInitializedState();
    } else {
        sentinel("Invalid endpoint type %d", endpoint_type);
    }

#if PRINT_PROCESS_CHANNEL_STATS
    // initialize the description, which doesn't change throughout the
    // program
    int sender_mpi_rank, receiver_mpi_rank;
    if (endpoint_type == PureRT::EndpointType::SENDER) {
        MPI_Comm_rank(_mpi_comm, &sender_mpi_rank);
        receiver_mpi_rank = mate_mpi_rank_in_comm;
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        sender_mpi_rank = mate_mpi_rank_in_comm;
        MPI_Comm_rank(_mpi_comm, &receiver_mpi_rank);
    } else {
        sentinel("Invalid endpoint type.");
    }
    // if (DEBUG_CHECK == 0) {
    //     sentinel("Must run in debug mode when PRINT_PROCESS_CHANNEL_STATS is
    //     "
    //              "enabled.");
    // }
    sprintf(description_, "%s, %d, %d, %d, %d",
            endpoint_type_name(endpoint_type).c_str(), sender_mpi_rank,
            receiver_mpi_rank, bce->GetSenderPureRank(),
            bce->GetDestPureRank());
#endif
}

DirectMPIChannel::~DirectMPIChannel() {
    if (_rt_buf != nullptr) {
        // for the sender-side endpoint type, it's possible the rt_buf is not
        // used based on the value of
        // using_rt_send_buf_hint that's passed in upon construction.
        free(_rt_buf);
    }
    if (_using_rt_send_buf_hint && !_using_sized_enqueues_hint &&
        _endpoint_type == PureRT::EndpointType::SENDER) {
        const int ret = MPI_Request_free(&enqueue_request);
        assert(ret == MPI_SUCCESS);
#if PRINT_PROCESS_CHANNEL_STATS
        printf("DMC: %s\t%llu sends\n", description_, num_mpi_sends);
#endif
    }
    if (_endpoint_type == PureRT::EndpointType::RECEIVER) {
#if PRINT_PROCESS_CHANNEL_STATS
        printf("DMC: %s\t%llu recvs  \t%d max outstanding deqs  \t%d deq req "
               "resizes\t%llu work "
               "steal attempts\n",
               description_, num_mpi_recvs, max_outstanding_deq_reqs,
               num_deq_req_resizes, num_work_steal_attempts);
#endif
    }

#if PRINT_PROCESS_CHANNEL_STATS
    assert(num_mpi_sends == num_mpi_enq_waits);
    assert(num_mpi_recvs == num_mpi_deq_waits);
#endif
}

void DirectMPIChannel::Enqueue(const size_t count_to_send) {
    EnqueueImpl(_rt_buf, count_to_send);
}

// uses user buffer (buffer allocated in user application)
void DirectMPIChannel::EnqueueUserBuf(const buffer_t user_buf,
                                      const size_t   count_to_send) {
#if DEBUG_CHECK
    if (_using_rt_send_buf_hint) {
        sentinel("EnqueueUserBuf called for DirectMPIChannel initialized with "
                 "_using_rt_send_buf_hint but you are calling EnqueueUserBuf, "
                 "which means both a user send buffer and runtime send "
                 "buffer were allocated. To fix, pass using_rt_send_buf_hint "
                 "when initializing the SendChannel in the user program.");
    }
#endif
    EnqueueImpl(user_buf, count_to_send);
}

void DirectMPIChannel::EnqueueImpl(const buffer_t sender_buf,
                                   const size_t   count_to_send) {

    assert(_recv_channel == nullptr);
    assert(count_to_send <= _count);

#if DEBUG_CHECK && WRITE_DMC_LOG
    GET_DMC_LOG;
    fprintf(of,
            "\tDMC: %p\tMPI_Isend\t%p\tcount: %d\tdatatype: "
            "%d\t_sender_thread_num_within_world_comm: "
            "%d\t_receiver_thread_num_within_world_comm: %d\tmate mpi "
            "rank: "
            "%d\ttag: "
            "%d\treq: %d starting",
            this, sender_buf, count_to_send, _datatype,
            _sender_thread_num_within_world_comm,
            _receiver_thread_num_within_world_comm, _mate_mpi_rank_in_comm,
            _encoded_tag, enqueue_request);
    DMC_CLOSE_LOG;
#endif

    last_enqueue_request = enqueue_request;

    // TODO: different send modes
    // TODO: allow multiple outstanding sends
    DC_START_TIMER(dc_mpi_isend);
    int ret;
    if (_using_rt_send_buf_hint && !_using_sized_enqueues_hint) {
        assert(count_to_send == _count);
        ret = MPI_Start(&enqueue_request);
        // assert(false); // don't think this is supported now.
    } else {
        ret = MPI_Isend(sender_buf, count_to_send,
                        static_cast<unsigned int>(_datatype),
                        _mate_mpi_rank_in_comm, _encoded_tag, _mpi_comm,
                        &enqueue_request);
        assert(enqueue_request != last_enqueue_request);
    }

    assert(ret == MPI_SUCCESS);
    DC_PAUSE_TIMER(dc_mpi_isend);
    INC_DC_COUNTER(num_mpi_sends);

#if DEBUG_CHECK && WRITE_DMC_LOG
    GET_DMC_LOG_SAME_SCOPE
    fprintf(of, "\tCOMPLETE\n");
    DMC_CLOSE_LOG;
#endif
}

void DirectMPIChannel::EnqueueBlocking(const size_t count_to_send) {
    DC_START_TIMER(dc_mpi_isend);
    assert(_using_rt_send_buf_hint);
    EnqueueBlockingImpl(_rt_buf, count_to_send);
    DC_PAUSE_TIMER(dc_mpi_isend);
}

void DirectMPIChannel::EnqueueBlockingUserBuf(const buffer_t user_send_buf,
                                              const size_t   count_to_send) {
    assert(!_using_rt_send_buf_hint);
    EnqueueBlockingImpl(user_send_buf, count_to_send);
}

void DirectMPIChannel::EnqueueBlockingImpl(const buffer_t sender_buf,
                                           const size_t   count_to_send) {

    // TODO: consider moving to MPI_Ssend or MPI_Issend
    // N.B. https://www.mcs.anl.gov/research/projects/mpi/sendmode.html
    const auto ret = MPI_Send(sender_buf, count_to_send,
                              static_cast<unsigned int>(_datatype),
                              _mate_mpi_rank_in_comm, _encoded_tag, _mpi_comm);
    assert(ret == MPI_SUCCESS);
}

// #if PURE_DMC_TEST_WAIT
void DirectMPIChannel::EnqueueWait(bool                 actually_blocking_wait,
                                   std::optional<bool*> enqueue_succeeded) {

#if DEBUG_CHECK && WRITE_DMC_LOG
    GET_DMC_LOG;
    fprintf(of, "\tDMC: %p\tSEND MPI_Wait starting", this);
    DMC_CLOSE_LOG;
#endif

    int        completed;
    MPI_Status status;
    DC_START_TIMER(dc_mpi_send_wait);

    if (actually_blocking_wait) {
        PureRT::do_mpi_wait_with_work_steal(_pure_thread, &enqueue_request,
                                            &status);
        completed = 1; // always
    } else {
        const auto ret =
                MPI_Test(&enqueue_request, &completed, MPI_STATUS_IGNORE);
        assert(ret == MPI_SUCCESS);
        if (enqueue_succeeded.has_value()) {
            *(enqueue_succeeded.value()) = completed;
        }
    }

    DC_PAUSE_TIMER(dc_mpi_send_wait);
    INC_DC_COUNTER(num_mpi_enq_waits);

#if DEBUG_CHECK && WRITE_DMC_LOG
    if (completed) {
        GET_DMC_LOG_SAME_SCOPE;
        fprintf(of, "\tCOMPLETE\n");
        DMC_CLOSE_LOG;
    }
#endif
}

// #else

// #include <chrono>
// #include <thread>
// void DirectMPIChannel::EnqueueWait() {
//     while (true) {
//         int req_done;
//         sentinel("DequeueWait tried to wait with super slow MPI_Test
//         approach. "
//                  "Configure away from this or rewrite this.");
//         const auto ret_success =
//                 MPI_Test(&enqueue_request, &req_done, MPI_STATUS_IGNORE);
//         assert(ret_success == MPI_SUCCESS);
//         if (req_done) {
//             break;
//         } else {
//             x86_pause_instruction();
//             std::this_thread::sleep_for(
//                     std::chrono::microseconds(PURE_DMC_TEST_WAIT_DELAY_USECS));
//         }
//     }
// }

// #endif

static thread_local bool __debug_warned_about_dequeue_user_buf = false;

// Oct 11, 2020: allowing multiple outstanding receives using the runtime
// buffer. We used to not allow this, but I'm not exactly sure why -- subsequent
// requests can just keep receiving into the
void DirectMPIChannel::Dequeue(const buffer_t user_buf) {
    assert(this != nullptr);
    check(_endpoint_type == PureRT::EndpointType::RECEIVER,
          "endpiont type is %d", _endpoint_type);
    assert(_recv_channel != nullptr);

#if DEBUG_CHECK && WRITE_DMC_LOG
    GET_DMC_LOG;
    fprintf(of,
            "\tDMC: %p\tRECV\tcount: %d  datatype: %d   "
            "_sender_thread_num_within_world_comm: "
            "%d\treceiver_thread_num_within_world_comm: %d\nmate mpi "
            "rank: %d "
            "  encoded tag: %d starting",
            this, _count, _datatype, _sender_thread_num_within_world_comm,
            _receiver_thread_num_within_world_comm, _mate_mpi_rank_in_comm,
            _encoded_tag);
    DMC_CLOSE_LOG;
#endif

    if (dequeue_requests.capacity() - dequeue_requests.size() == 1) {
        dequeue_requests.set_capacity(dequeue_requests.capacity() * 2);
        INC_DC_COUNTER(num_deq_req_resizes);
    }
    assert(dequeue_requests.capacity() - dequeue_requests.size() > 0);

    // enforce that multiple outstanding requests in DirectMPIChannel can only
    // be used if using the user_buf (not runtime buf)
    if (_using_rt_recv_buf) {
// assert(user_buf == nullptr);
#if DEBUG_CHECK
        int rank;
        MPI_Comm_rank(_mpi_comm, &rank);
        if (user_buf != nullptr && rank == 0 &&
            !__debug_warned_about_dequeue_user_buf) {
            fprintf(stderr,
                    KYEL
                    "WARNING: DirectMPIChannel::Dequeue: called with a "
                    "userbuf, but _using_rt_recv_buf is true, which means a "
                    "runtime buffer was allocated (wastefully). You may want "
                    "to experiment with different settings of "
                    "BUFFERED_CHAN_MAX_PAYLOAD_BYTES, which is currently "
                    "%d\n" KRESET,
                    BUFFERED_CHAN_MAX_PAYLOAD_BYTES);
            __debug_warned_about_dequeue_user_buf = true;

            if (user_buf == nullptr && _using_rt_recv_buf == false) {
                sentinel("user_buf is nullptr, but no runtime receive buffer "
                         "was allocated. Either use a non-null user buffer, or "
                         "raise the BUFFERED_CHAN_MAX_PAYLOAD_BYTES threshold "
                         "to at least %d.",
                         _recv_channel->NumPayloadBytes());
            }
        }
#endif
        // check(dequeue_requests.capacity() == 1,
        //       "Expected dequeue requests capacity to be 1, but it is %lu",
        //       dequeue_requests.capacity());
        // assert(dequeue_requests.size() == 0); // no current outstanding
    }

    // if user_buf is nullptr, that means we should use the runtime buffer.
    // otherwise, we should receive directly into the user buf
    DC_START_TIMER(dc_mpi_irecv);

    // I think there's a bug here. When we want to use the user buffer but the
    // system has allocated an rt_buf, you still have to use what the user
    // wants. The rt_buf is simply wasted in that case.
    const buffer_t dest_buf = (user_buf == nullptr) ? _rt_buf : user_buf;
    check(dest_buf != nullptr,
          "dest_buf is nullptr. _using_rt_recv_buf is %d. Reminder that you "
          "must use a user receive buffer if it's large (more than "
          "BUFFERED_CHAN_MAX_PAYLOAD_BYTES = %d)",
          _using_rt_recv_buf, BUFFERED_CHAN_MAX_PAYLOAD_BYTES);

    MPI_Request req;
    const int   ret =
            MPI_Irecv(dest_buf, _count, static_cast<unsigned int>(_datatype),
                      _mate_mpi_rank_in_comm, _encoded_tag, _mpi_comm, &req);
    assert(ret == MPI_SUCCESS);

    // add to the back, wait from the front
    dequeue_requests.push_back(req);

    DC_PAUSE_TIMER(dc_mpi_irecv);
    assert(dequeue_requests.size() > 0);

#if PRINT_PROCESS_CHANNEL_STATS
    if (max_outstanding_deq_reqs < dequeue_requests.size()) {
        max_outstanding_deq_reqs = dequeue_requests.size();
    }
#endif
    INC_DC_COUNTER(num_mpi_recvs);

#if DEBUG_CHECK && WRITE_DMC_LOG
    GET_DMC_LOG_SAME_SCOPE
    fprintf(of, "\tCOMPLETE\n");
    DMC_CLOSE_LOG;
#endif

} // function Dequeue

#if 0
void                     DirectMPIChannel::Dequeue(const buffer_t user_buf) {
    assert(_recv_channel != nullptr);
    if (!_using_rt_recv_buf &&
        dequeue_requests.capacity() - dequeue_requests.size() == 1) {
        // resize dequeue_requests, assuming not using runtime buffer
        dequeue_requests.set_capacity(dequeue_requests.capacity() * 2);
        INC_DC_COUNTER(num_deq_req_resizes);
    }

    // debugging
    // assert(_using_rt_recv_buf);
    // fprintf(stderr, "[%d] size %lu   capacity: %lu\n", PURE_RANK,
    //         dequeue_requests.size(), dequeue_requests.capacity());

    if (dequeue_requests.capacity() - dequeue_requests.size() == 0) {
        print_pure_backtrace(stderr, '\n');
    }

    assert(dequeue_requests.capacity() - dequeue_requests.size() > 0);

    // enforce that multiple outstanding requests in DirectMPIChannel can only
    // be used if using the user_buf (not runtime buf)
    if (_using_rt_recv_buf) {
// assert(user_buf == nullptr);
#if DEBUG_CHECK
        int rank;
        MPI_Comm_rank(_mpi_comm, &rank);
        if (user_buf != nullptr && rank == 0 &&
            !__debug_warned_about_dequeue_user_buf) {
            fprintf(stderr,
                    KYEL
                    "WARNING: DirectMPIChannel::Dequeue: called with a "
                    "userbuf, but _using_rt_recv_buf is true, which means a "
                    "runtime buffer was allocated (wastefully). You may want "
                    "to experiment with different settings of "
                    "BUFFERED_CHAN_MAX_PAYLOAD_BYTES, which is currently "
                    "%d\n" KRESET,
                    BUFFERED_CHAN_MAX_PAYLOAD_BYTES);
            __debug_warned_about_dequeue_user_buf = true;

            if (user_buf == nullptr && _using_rt_recv_buf == false) {
                sentinel("user_buf is nullptr, but no runtime receive buffer "
                         "was allocated. Either use a non-null user buffer, or "
                         "raise the BUFFERED_CHAN_MAX_PAYLOAD_BYTES threshold "
                         "to at least %d.",
                         _recv_channel->NumPayloadBytes());
            }
        }
#endif
        check(dequeue_requests.capacity() == 1,
              "Expected dequeue requests capacity to be 1, but it is %lu",
              dequeue_requests.capacity());
        assert(dequeue_requests.size() == 0); // no current outstanding
    }

    // if user_buf is nullptr, that means we should use the runtime buffer.
    // otherwise, we should receive directly into the user buf
    DC_START_TIMER(dc_mpi_irecv);

    // I think there's a bug here. When we want to use the user buffer but the
    // system has allocated an rt_buf, you still have to use what the user
    // wants. The rt_buf is simply wasted in that case.
    const buffer_t dest_buf = (user_buf == nullptr) ? _rt_buf : user_buf;
    check(dest_buf != nullptr,
          "dest_buf is nullptr. _using_rt_recv_buf is %d. Reminder that you "
          "must use a user receive buffer if it's large (more than "
          "BUFFERED_CHAN_MAX_PAYLOAD_BYTES = %d)",
          _using_rt_recv_buf, BUFFERED_CHAN_MAX_PAYLOAD_BYTES);
    MPI_Request req;

    const int ret =
            MPI_Irecv(dest_buf, _count, static_cast<unsigned int>(_datatype),
                      _mate_mpi_rank_in_comm, _encoded_tag, _mpi_comm, &req);
    assert(ret == MPI_SUCCESS);

    // add to the back, wait from the front
    dequeue_requests.push_back(req);

    DC_PAUSE_TIMER(dc_mpi_irecv);
    assert(dequeue_requests.size() > 0);

#if PRINT_PROCESS_CHANNEL_STATS
    if (max_outstanding_deq_reqs < dequeue_requests.size()) {
        max_outstanding_deq_reqs = dequeue_requests.size();
    }
#endif
    INC_DC_COUNTER(num_mpi_recvs);
} // function Dequeue
#endif

#if 0
const PureContRetArr& DirectMPIChannel::DequeueWait() {
    assert(dequeue_requests.size() > 0);
    MPI_Status status;
    DC_START_TIMER(dc_mpi_recv_wait);

    MPI_Request& curr_deq_req = dequeue_requests.front();

    MPI_Request req_copy = curr_deq_req;
    PureRT::do_mpi_wait_with_work_steal(_pure_thread, &curr_deq_req, &status);

    // remove this request now that it's been received
    dequeue_requests.pop_front();
    DC_PAUSE_TIMER(dc_mpi_recv_wait);

    // figure out how much was actually received and set it for future use
    int received_count;
    int count_ret = MPI_Get_count(&status, _datatype, &received_count);
    assert(count_ret == MPI_SUCCESS);
    assert(received_count <= _count);
    _recv_channel->SetReceivedCount(received_count);

    // TODO: clean this out; only for debug mode
    INC_DC_COUNTER(num_mpi_deq_waits);
    // dequeue_in_progress = false;

#if PCV4_OPTION_EXECUTE_CONTINUATION
    if (_dequeue_pipeline.Empty()) {
        return static_empty_pure_cont_ret_arr;
    } else {
        assert(_dequeue_pipeline.NumStages() > 0);
        DC_START_TIMER(dc_deq_wait_continuation);

        check(_using_rt_recv_buf,
              "Currently, post-recv continuation-style pipelines are only "
              "supported when the runtime buffer is used. This is because not "
              "using the runtime buffer supports multiple outstanding "
              "requests, which means it supports multiple outstanding buffers. "
              "We are not currently storing the buffer locations for the "
              "multiple outstanding requests.");
        _dequeue_pipeline.ExecutePCPipeline(_rt_buf, received_count);
        DC_PAUSE_TIMER(dc_deq_wait_continuation);
        return this->_dequeue_pipeline.GetReturnVals();
    }
#else
    // returns a reference to a const empty vector
    return static_empty_pure_cont_ret_arr;
#endif // no continuation

} // function DequeueWait

#endif

const PureContRetArr&
DirectMPIChannel::DequeueWait(bool                 actually_blocking_wait,
                              std::optional<bool*> dequeue_succeeded_out) {

#if DEBUG_CHECK && WRITE_DMC_LOG
    GET_DMC_LOG;
    fprintf(of, "\tDMC: %p\tRECV WAIT starting ", this);
    DMC_CLOSE_LOG;
#endif

    check(dequeue_requests.size() > 0,
          "[wr%d] Dequeue requests is empty for DMC. Mate MPI rank is %d",
          _pure_thread->Get_rank(), _mate_mpi_rank_in_comm);
    MPI_Status status;

    DC_START_TIMER(dc_mpi_recv_wait);
    MPI_Request& curr_deq_req = dequeue_requests.front();

    bool dequeue_wait_succeeded = false;
    if (actually_blocking_wait) {
        // int rank;
        // MPI_Comm_rank(_mpi_comm, &rank);
        // fprintf(stderr,
        //         KYEL "r%d\tcount: %d  encoded tag: %d  [ m%d -> m%d ]\n"
        //         KRESET, PURE_RANK, _count, _encoded_tag,
        //         _mate_mpi_rank_in_comm, rank);

        // print_pure_backtrace(stderr, '\n');
        PureRT::do_mpi_wait_with_work_steal(_pure_thread, &curr_deq_req,
                                            &status);
        dequeue_wait_succeeded = true;
    } else {
        // sentinel("DequeueWait tried to wait with super slow MPI_Test
        // approach. "
        //          "Configure away from this or rewrite this.");
        int        test_succeeded;
        const auto ret = MPI_Test(&curr_deq_req, &test_succeeded, &status);
        assert(ret == MPI_SUCCESS);

        if (test_succeeded == 1) {
            dequeue_wait_succeeded = true;
        }

        if (dequeue_succeeded_out.has_value()) {
            *(dequeue_succeeded_out.value()) = dequeue_wait_succeeded;
        }

        if (dequeue_wait_succeeded == false) {
            // Recv not done, so the continuation couldn't have been run. Just
            // return. Also, don't set the count or do anything with the
            // dequeue_reqeusts list.
            return static_empty_pure_cont_ret_arr;
        }
    }

    assert(dequeue_wait_succeeded);
    // remove this request now that it's been received in the case where the
    // receive succeeded
    dequeue_requests.pop_front();
    DC_PAUSE_TIMER(dc_mpi_recv_wait);

    // figure out how much was actually received and set it for future use
    int received_count;
    int count_ret = MPI_Get_count(&status, _datatype, &received_count);
    assert(count_ret == MPI_SUCCESS);

#if DEBUG_CHECK && WRITE_DMC_LOG
    GET_DMC_LOG_SAME_SCOPE
    fprintf(of, "\tCOMPLETE\n");
    DMC_CLOSE_LOG;
#endif

#if DEBUG_CHECK
    if (received_count > _count) {
        print_pure_backtrace();
        sentinel("recieved message large");
    }
#endif
    assert(received_count <= _count);

    _recv_channel->SetReceivedCount(received_count);

    // TODO: clean this out; only for debug mode
    INC_DC_COUNTER(num_mpi_deq_waits);
    // dequeue_in_progress = false;

#if PCV4_OPTION_EXECUTE_CONTINUATION
    if (_dequeue_pipeline.Empty()) {
        return static_empty_pure_cont_ret_arr;
    } else {
        assert(_dequeue_pipeline.NumStages() > 0);
        DC_START_TIMER(dc_deq_wait_continuation);

        check(_using_rt_recv_buf,
              "Currently, post-recv continuation-style pipelines are only "
              "supported when the runtime buffer is used. This is because not "
              "using the runtime buffer supports multiple outstanding "
              "requests, which means it supports multiple outstanding buffers. "
              "We are not currently storing the buffer locations for the "
              "multiple outstanding requests.");
        _dequeue_pipeline.ExecutePCPipeline(_rt_buf, received_count);
        DC_PAUSE_TIMER(dc_deq_wait_continuation);
        return this->_dequeue_pipeline.GetReturnVals();
    }
#else
    // returns a reference to a const empty vector
    return static_empty_pure_cont_ret_arr;
#endif // no continuation

} // function DequeueWait

void DirectMPIChannel::DequeueBlocking(const buffer_t user_buf) {
    assert(_recv_channel != nullptr);

#if DEBUG_CHECK
    if (_using_rt_recv_buf) {
        assert(user_buf == nullptr);
    }
#endif

    // if user_buf is nullptr, that means we should use the runtime buffer.
    // otherwise, we should receive directly into the user buf
    DC_START_TIMER(dc_mpi_irecv);

    const buffer_t dest_buf = (user_buf == nullptr) ? _rt_buf : user_buf;
    assert(dest_buf != nullptr);
    const auto ret = MPI_Recv(
            dest_buf, _count, static_cast<unsigned int>(_datatype),
            _mate_mpi_rank_in_comm, _encoded_tag, _mpi_comm, MPI_STATUS_IGNORE);
    assert(ret == MPI_SUCCESS);
    DC_PAUSE_TIMER(dc_mpi_irecv);
    INC_DC_COUNTER(num_mpi_recvs);
} // function DequeueBlocking

#if PRINT_PROCESS_CHANNEL_STATS
// returns a string with contents for debugging purposes
// example:
char* DirectMPIChannel::GetDescription() { return description_; }
#endif

void DirectMPIChannel::Validate() const {
#if DEBUG_CHECK
    check(_endpoint_type == PureRT::EndpointType::SENDER ||
                  _endpoint_type == PureRT::EndpointType::RECEIVER,
          "Invalid endpint type");
#endif
}

int DirectMPIChannel::ConstructTagForMPI(
        int sender_thread_num_within_world_comm,
        int receiver_thread_num_within_world_comm, int application_tag,
        MPI_Comm mpi_comm) {

    const char* cray_env = std::getenv("CRAY_CPU_TARGET");
    if (cray_env != nullptr) {
        check(std::string(cray_env).compare(std::string("x86-milan")) == 0,
              "Only x86-milan is currently supported on Perlmutter but "
              "CRAY_CPU_TARGET is %s",
              cray_env);
    }

    // fix this to enable hyperthreads
    const auto max_thread_num = 255; // Perlmutter milan only
    // const auto max_thread_num = 127; // Perlmutter milan only

    check_always(sender_thread_num_within_world_comm >= 0 &&
                         sender_thread_num_within_world_comm <= max_thread_num,
                 "expected sender thread num %d to be within [0, %d]",
                 sender_thread_num_within_world_comm, max_thread_num);
    check_always(receiver_thread_num_within_world_comm >= 0 &&
                         receiver_thread_num_within_world_comm <=
                                 max_thread_num,
                 "expected receiver thread num %d to be within [0, %d]",
                 receiver_thread_num_within_world_comm, max_thread_num);

    // on Perlmutter, the largest tag as determined by MPI_TAG_UB the largest
    // possible tag is 1,483,131,296. log2(1,483,131,296) = 30.xxx so we max out
    // at 30 bits total for the tag.
    const auto total_tag_bits           = 30;
    const auto single_thread_width_bits = 8; // we need up to 255
    const auto thread_nums_width_bits =
            single_thread_width_bits + single_thread_width_bits;
    constexpr auto tag_width = total_tag_bits - thread_nums_width_bits;
    const int      max_tag   = std::pow(2, tag_width) - 1;

    assert(application_tag >= 0);
    check_always(application_tag <= max_tag,
                 "[r%d] DMC tag too large: must be <= %d but is %d", PURE_RANK,
                 max_tag, application_tag);

    unsigned long encoded_tag =
            (application_tag << thread_nums_width_bits) |
            (sender_thread_num_within_world_comm << single_thread_width_bits) |
            receiver_thread_num_within_world_comm;

    const auto max_perlmutter_tag = 33554431; // see [1] below
    check_always(encoded_tag < max_perlmutter_tag,
                 "The max Perlmutter tag is %d, but the encoded tag that we "
                 "came up with is %lu",
                 max_perlmutter_tag, encoded_tag);

    /*
Note 1:


    Max tag info on Perlmutter via Feb 2024 email with NERSC:

    $ man intro_mpi | grep
```
We received the following information
" It is important to note that the maximum tag value supported by HPE
Cray MPI varies depending on what network interconnect and libraries are
in use. These variations are dictated by underlying networking layers
and hardware. Please note that the MPI standard only requires that the
minimum maximum tag value be at least 32767 and that the tag value not
change during the execution of an MPI program. If you are using UCX
libraries the maximum tag value is always 16777215. If you are using OFI
libraries and do not use the interconnect (run on a UAN/UAI via ./<prog>
or run on a single node, even multiple ranks on a single node) the
maximum tag value will be 536870911.
1. Using industry standard NICs (SS-10) - maximum tag value is 268435455
2. Using the HPE Slingshot NIC (SS-11) - maximum tag value is 33554431"

As Perlmutter is using HPE Slingshot NICs (SS-11), the maximum tag value
is limited to 33554431. Please note that:
1. This is an OFI CXI limit and not that of the MPICH layer
2. Cray no longer ships a separate MPICH library with additional tag
value support thereby allowing us to provide the large tag option.
*/

    // MPI requires a signed int tag
    const int int_encoded_tag = static_cast<int>(encoded_tag);
    return int_encoded_tag;
}