// Author: James Psota
// File:   bundle_channel_endpoint.h 

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

#ifndef PURE_TRANSPORT_BUNDLE_CHANNEL_ENDPOINT_H
#define PURE_TRANSPORT_BUNDLE_CHANNEL_ENDPOINT_H

#pragma once

#include "mpi.h"
#include <sstream>
#include <type_traits>
#include <typeinfo>

#include <boost/functional/hash.hpp>

#include "pure/common/pure_rt_enums.h"
#include "pure/common/pure_user.h"
#include "pure/support/helpers.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/process_channel.h"

// forward declarations
class BundleChannel;
class DirectMPIChannel;
class RdmaMPIChannel;
class ProcessChannel;
class PureThread;
class PureCommProcessDetail;
class PureComm;
class SendToSelfChannel;

using std::string;
using namespace PureRT;

namespace PureRT {
enum class ChannelEndpointType {
    NOT_INITIALIZED, // 0
    PROCESS,         // 1
    BUNDLE,          // 2
    DIRECT_MPI,      // 3
    DIRECT_BATCHER,
    FLAT_BATCHER,
    RDMA_MPI,
    SEND_TO_SELF
};
} // namespace PureRT

struct BundleChannelEndpointMetadata {
    PureCommProcessDetail* const pure_comm_process_detail_ptr;
    const int                    count;
    const MPI_Datatype           datatype;
    const int                    sender_pure_rank;
    const int                    dest_pure_rank;
    const int                    tag;

    BundleChannelEndpointMetadata(PureComm* const pure_comm, int count_arg,
                                  MPI_Datatype datatype_arg,
                                  int          sender_pure_rank_arg,
                                  int dest_pure_rank_arg, int tag_arg);
    bool        operator==(const BundleChannelEndpointMetadata& other) const;
    std::string ToString() const;
};

struct BundleChannelEndpointMetadataHasher {
    // N.B. inspired by
    // http://stackoverflow.com/questions/17016175/c-unordered-map-using-a-custom-class-type-as-the-key
    size_t operator()(const BundleChannelEndpointMetadata& bcem) const {
        using boost::hash_combine;
        using boost::hash_value;

        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Modify 'seed' by XORing and bit-shifting in
        // one member of 'ProcessChannelMetadata' after the other:
        hash_combine(seed, hash_value(bcem.pure_comm_process_detail_ptr));
        hash_combine(seed, hash_value(bcem.count));
        hash_combine(seed,
                     hash_value(static_cast<unsigned int>(bcem.datatype)));
        hash_combine(seed, hash_value(bcem.sender_pure_rank));
        hash_combine(seed, hash_value(bcem.dest_pure_rank));
        hash_combine(seed, hash_value(bcem.tag));

        return seed;

    } // function operator
};

// TODO: templetize this class, and make the buffers of the templetized type.
// Then the GetSendChannelBuf
// will be typed cleanly and there will be no need for a static cast.
class BundleChannelEndpoint {

    // hmm not sure if this is the best way to do this.
    //  friend class process_channel_v4::ProcessChannel;

  public:
    static std::string
            ChannelEndpointTypeName(PureRT::ChannelEndpointType /*ct*/);
    static size_t
                TotalEndpointsPayloadBytes(BundleChannelEndpoint** /* endpoint_array */,
                                           int /*num_endpoints*/);
    static bool ValidEndpointType(PureRT::EndpointType /*endpoint_type*/);

    inline BundleChannel* GetBundleChannel() { return bundle_channel; }

    void SetBundleChannel(BundleChannel* const /*bc*/);
    void SetProcessChannel(ProcessChannel* const /*pc*/);
    void SetDirectMPIChannel(DirectMPIChannel* const /*dmc*/);
    void SetRdmaMPIChannel(RdmaMPIChannel* const /*dmc*/);
    void SetSendToSelfChannel(SendToSelfChannel* const);

    virtual void WaitForInitialization() const;

    // GETTERS -- these are set to whatever purecomm was used upon channel
    // initialization!
    inline int GetSenderPureRank() const { return sender_pure_rank; }
    inline int GetDestPureRank() const { return dest_pure_rank; }

    virtual inline void* GetSendBuf() const { return send_buf; }
    virtual inline void* GetRecvBuf() const { return recv_buf; }

    // TODO: change these to cleaner definitions after moving to C++14
    template <typename BufType>
    auto inline GetSendChannelBuf() const -> BufType {
        check(send_channel_buf != nullptr,
              "ERROR: You called GetSendChannelBuf() before the buffer was "
              "ready. For "
              "SendChannels, "
              "it's ready for use after SendChannel::WaitForInitialization "
              "returns when using the runtime buffer. If you passed the "
              "using_runtime_buffer=false flag, "
              "then simply do not call GetSendChannelBuf because it wasn't "
              "allocated "
              "For ReduceChannel, it's ready after "
              "ReduceChannel::InitReduceChannel returns (TODO: "
              "fix this to be consistent)");
        // TODO: templetize this class, and make the buffers of the templetized
        // type. Then the GetSendChannelBuf
        // will be typed cleanly and there will be no need for a static cast.
        AssertConsistentDatatype<BufType>();
        return static_cast<BufType>(send_channel_buf);
    }

    template <typename BufType>
    auto inline GetRecvChannelBuf() const -> BufType {
        check(recv_channel_buf != nullptr,
              "ERROR: You called GetRecvChannelBuf() before the buffer was "
              "ready. "
              "For RecvChannel, it's ready after "
              "RecvChannel::WaitForInitialization returns; for "
              "BcastChannel, it's ready after "
              "BcastChannel::WaitForInitialization is called; "
              "For ReduceChannel, it's ready after "
              "ReduceChannel::WaitForInitialization is first "
              "called.");
        // TODO: templetize this class, and make the buffers of the templetized
        // type. Then the GetSendChannelBuf
        // will be typed cleanly and there will be no need for a static cast.
        AssertConsistentDatatype<BufType>();
        return static_cast<BufType>(recv_channel_buf);
    }

    inline int          GetCount() const { return count; }
    inline int          GetTag() const { return tag; }
    inline MPI_Datatype GetDatatype() const { return datatype; }
    inline int          GetDatatypeBytes() const { return datatype_bytes; }
    int                 NumPayloadBytes() const;

    inline PureRT::ChannelEndpointType GetChannelType() const {
        return channel_type;
    }
    inline std::string GetChannelTypeName() const {
        return ChannelEndpointTypeName(channel_type);
    }
    inline channel_endpoint_tag_t GetChannelEndpointTag() const {
        return channel_endpoint_tag;
    }

    // SETTERS
    // THESE ARE FOR CHANGING THE USER'S BUFFER. DEPRECATED FOR USE WITH RUNTIME
    // BUFFER.
    virtual inline void SetSendBuf(void* new_send_buf) {
        send_buf = new_send_buf;
    }
    virtual inline void SetRecvBuf(void* new_recv_buf) {
        recv_buf = new_recv_buf;
    }

    // THESE ARE FOR INTERNAL USE (not by the user) FOR THE RUNTIME TO SET THE
    // CHANNEL BUFFER FOR USE BY THE USER WHICH IS LATER GOTTEN BY
    // GetSendChannelBuf and GetRecvChannelBuf
    virtual inline void SetSendChannelBuf(void* new_send_channel_buf) {
        ts_log_info("Setting send_channel_buf to %p", new_send_channel_buf);
        send_channel_buf = new_send_channel_buf;
    }
    virtual inline void SetRecvChannelBuf(void* new_recv_channel_buf) {
        ts_log_info("Setting recv_channel_buf to %p", new_recv_channel_buf);
        recv_channel_buf = new_recv_channel_buf;
    }
    inline bool IsSendToSelfChannel() {
        return (sender_pure_rank == dest_pure_rank);
    }

    void   CheckInvariants() const;
    string TransportChannelToString() const;
    virtual ~BundleChannelEndpoint();

    // TODO: fixme: put the above raw getters back into protected section
    // TODO: fixme: put the above raw getters back into protected section

    inline void* GetRawSendChannelBuf() const { return send_channel_buf; }
    inline void* GetRawRecvChannelBuf() const { return recv_channel_buf; }

    // DEBUGGING HELPERS
    inline void PrintSimple(char* const label) {
        fprintf(stderr, "[%s]\t%d -> %d\tcount: %d\ttag: %d\tcet: %d\n", label,
                sender_pure_rank, dest_pure_rank, count, tag,
                channel_endpoint_tag);
    }

    inline PureThread* GetPureThread() { return pure_thread; }

  protected:
    PureThread* pure_thread; // back pointer to the pure thread that has this
                             // endpoint
    // TODO: rip this out
    // TODO: consider changing these to a variant -- only should have one
    BundleChannel*              bundle_channel;
    ProcessChannel*             process_channel;
    DirectMPIChannel*           direct_mpi_channel;
    RdmaMPIChannel*             rdma_mpi_channel;
    SendToSelfChannel*          send_to_self_channel;
    void*                       send_buf; // user's buffer, deprecated
    void*                       recv_buf; // user's buffer, deprecated
    void*                       send_channel_buf = nullptr;
    void*                       recv_channel_buf = nullptr;
    const int                   count;
    const MPI_Datatype          datatype;
    int                         datatype_bytes;
    const int                   tag;
    const int                   sender_pure_rank;
    const int                   dest_pure_rank;
    const unsigned int          channel_endpoint_tag;
    PureRT::ChannelEndpointType channel_type;

    BundleChannelEndpoint(void* const /*send_buf_arg*/,
                          void* const /*recv_buf_arg*/, int /*count_arg*/,
                          const MPI_Datatype& /*datatype_arg*/,
                          int /*sender_pure_rank_arg*/, int /*tag_arg*/,
                          int /*dest_pure_rank_arg*/,
                          unsigned int /*channel_endpoint_tag_arg*/,
                          PureThread* /* pt */);

    // These are for internal runtime use only; access via parent class or
    // friend class.
    // inline void* const GetRawSendChannelBuf() const { return
    // send_channel_buf; }
    // inline void* const GetRawRecvChannelBuf() const { return
    // recv_channel_buf; }

  private:
    template <typename BufType>
    void AssertConsistentDatatype() const {

#if DEBUG_CHECK
        check(std::is_pointer<BufType>::value,
              "Expected BufType to be a pointer type, but it is %s .",
              typeid(BufType).name());

        bool        is_int, is_float, is_double, is_char, is_long, is_uint64;
        const char* buf_type_name = demangle(typeid(BufType).name()).c_str();

        switch (datatype) {
        case MPI_INT:
            is_int = std::is_same<BufType, int*>::value;
            check(is_int,
                  "Expected BufType to be int*, but is %s. Channel "
                  "datatype is %u",
                  buf_type_name, static_cast<unsigned int>(datatype));
            break;
        case MPI_FLOAT:
            is_float = std::is_same<BufType, float*>::value;
            check(is_float,
                  "Expected BufType to be float*, but is %s. Channel "
                  "datatype is %u",
                  buf_type_name, static_cast<unsigned int>(datatype));
            break;
        case MPI_DOUBLE:
            is_double = std::is_same<BufType, double*>::value;
            check(is_double,
                  "Expected BufType to be double*, but is %s. "
                  "Channel datatype is %u",
                  buf_type_name, static_cast<unsigned int>(datatype));
            break;
        case MPI_LONG:
            is_long = std::is_same<BufType, long*>::value;
            check(is_long,
                  "Expected BufType to be long*, but is %s. "
                  "Channel datatype is %u",
                  buf_type_name, static_cast<unsigned int>(datatype));
            break;
        case MPI_UINT64_T:
            is_uint64 = std::is_same<BufType, uint64_t*>::value;
            check(is_uint64,
                  "Expected BufType to be uint64_t*, but is %s. "
                  "Channel datatype is %u",
                  buf_type_name, static_cast<unsigned int>(datatype));
            break;
        case MPI_CHAR:
            is_char = std::is_same<BufType, char*>::value;
            check(is_char,
                  "Expected BufType to be char*, but is %s. Channel "
                  "datatype is %u",
                  buf_type_name, static_cast<unsigned int>(datatype));
            break;
        case MPI_BYTE:
            // use a char* for MPI_BYTE
            is_char = std::is_same<BufType, char*>::value;
            check(is_char,
                  "Expected BufType to be char*, but is %s. Channel "
                  "datatype is %u",
                  buf_type_name, static_cast<unsigned int>(datatype));
            break;
        default:
            sentinel("Unsupported MPI datatype %#x",
                     static_cast<unsigned int>(datatype));
        }
#endif
    }
};
#endif
