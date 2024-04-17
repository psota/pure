// Author: James Psota
// File:   bundle_channel_endpoint.cpp 

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

#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/runtime/pure_comm.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/bundle_channel.h"
#include "pure/transport/process_channel.h"

BundleChannelEndpointMetadata::BundleChannelEndpointMetadata(
        PureComm* const pure_comm, int count_arg, MPI_Datatype datatype_arg,
        int sender_pure_rank_arg, int dest_pure_rank_arg, int tag_arg)
    : pure_comm_process_detail_ptr(pure_comm->GetProcessDetails()),
      count(count_arg), datatype(datatype_arg),
      sender_pure_rank(sender_pure_rank_arg),
      dest_pure_rank(dest_pure_rank_arg), tag(tag_arg) {}

bool BundleChannelEndpointMetadata::
     operator==(const BundleChannelEndpointMetadata& other) const {
    return (pure_comm_process_detail_ptr ==
                    other.pure_comm_process_detail_ptr &&
            count == other.count && datatype == other.datatype &&
            sender_pure_rank == other.sender_pure_rank &&
            dest_pure_rank == other.dest_pure_rank && tag == other.tag);
}

std::string BundleChannelEndpointMetadata::ToString() const {
    std::stringstream ss;
    ss << "Pure comm process detail pointer: " << pure_comm_process_detail_ptr
       << std::endl;
    ss << "COUNT: " << count << std::endl;
    ss << "DATATYPE: " << static_cast<unsigned int>(datatype) << std::endl;
    ss << "SENDER PURE RANK: " << sender_pure_rank << std::endl;
    ss << "DEST PURE RANK: " << dest_pure_rank << std::endl;
    ss << "TAG: " << tag << std::endl;
    return ss.str();
}

//////////////////////////////////////////////////////////
// static function
std::string
BundleChannelEndpoint::ChannelEndpointTypeName(PureRT::ChannelEndpointType ct) {

    switch (ct) {
    // We put BUNDLE first as that's probably the common case
    case PureRT::ChannelEndpointType::BUNDLE:
        return "BUNDLE";
    case PureRT::ChannelEndpointType::PROCESS:
        return "PROCESS";
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        return "DIRECT";
    default:
        sentinel("Invalid channel type %d", ct);
    }
} // static function ChannelEndpointTypeName

// static function
size_t BundleChannelEndpoint::TotalEndpointsPayloadBytes(
        BundleChannelEndpoint** endpoint_array, int num_endpoints) {
    size_t total_payload_bytes = 0;
    for (auto i = 0; i < num_endpoints; ++i) {
        total_payload_bytes += endpoint_array[i]->NumPayloadBytes();
    }
    return total_payload_bytes;

} // function TotalEndpointsPayloadBytes

bool BundleChannelEndpoint::ValidEndpointType(
        PureRT::EndpointType endpoint_type) {
    return endpoint_type == PureRT::EndpointType::SENDER ||
           endpoint_type == PureRT::EndpointType::RECEIVER ||
           endpoint_type == PureRT::EndpointType::REDUCE ||
           endpoint_type == PureRT::EndpointType::BCAST;
}

BundleChannelEndpoint::BundleChannelEndpoint(
        void* const send_buf_arg, void* const recv_buf_arg, int count_arg,
        const MPI_Datatype& datatype_arg, int sender_pure_rank_arg, int tag_arg,
        int dest_pure_rank_arg, unsigned int channel_endpoint_tag_arg,
        PureThread* pure_thread_arg)
    : send_buf(send_buf_arg), recv_buf(recv_buf_arg), count(count_arg),
      datatype(datatype_arg), tag(tag_arg),
      sender_pure_rank(sender_pure_rank_arg),
      dest_pure_rank(dest_pure_rank_arg),
      channel_endpoint_tag(channel_endpoint_tag_arg),
      pure_thread(pure_thread_arg),
      channel_type(PureRT::ChannelEndpointType::NOT_INITIALIZED),
      bundle_channel(nullptr), process_channel(nullptr),
      direct_mpi_channel(nullptr), rdma_mpi_channel(nullptr),
      send_to_self_channel(nullptr) {

    switch (datatype) {
    case MPI_DOUBLE:
        datatype_bytes = sizeof(double);
        break;
    case MPI_INT:
        datatype_bytes = sizeof(int);
        break;
    case MPI_FLOAT:
        datatype_bytes = sizeof(float);
        break;
    case MPI_LONG:
        datatype_bytes = sizeof(long);
        break;
    case MPI_LONG_LONG_INT:
        datatype_bytes = sizeof(long long int);
        break;
    case MPI_UINT64_T:
        datatype_bytes = sizeof(uint64_t);
        break;
    case MPI_CHAR:
        datatype_bytes = sizeof(char);
        break;
    case MPI_BYTE:
        datatype_bytes = 1;
        break;
    default:
        sentinel("Invalid or unimplemented MPI_Datatype %d. If you need to add "
                 "more datatypes, you must do so in both reduce_channel.cpp as "
                 "well as bundle_channel_endpoint.cpp",
                 static_cast<unsigned int>(datatype));
    } // switch on reduce channel's datatype
}

BundleChannelEndpoint::~BundleChannelEndpoint() {
    // TODO(jim): NYI
}

void BundleChannelEndpoint::CheckInvariants() const {

    const size_t max_sender = 16384;
    check((int)datatype > 0, "Datatype %d must be greater than zero",
          (int)datatype);
    check(sender_pure_rank < max_sender ||
                  sender_pure_rank == PureRT::PURE_UNUSED_RANK,
          "sender pure rank (%d) must be less than %zu (note: this is "
          "arbitrary can can be increased)",
          sender_pure_rank, max_sender);
    check(dest_pure_rank < max_sender ||
                  dest_pure_rank == PureRT::PURE_UNUSED_RANK,
          "dest pure rank (%d) must be less than %zu (note: this is arbitrary "
          "can can be increased)",
          dest_pure_rank, max_sender);
    check(sender_pure_rank >= 0 || sender_pure_rank == PureRT::PURE_UNUSED_RANK,
          "sender_pure_rank (%d) must be greater than zero", sender_pure_rank);
    check(dest_pure_rank >= 0 || dest_pure_rank == PureRT::PURE_UNUSED_RANK,
          "dest_pure_rank (%d) must be greater than zero", dest_pure_rank);
    check(tag >= 0 || tag == PureRT::PURE_UNUSED_TAG,
          "tag (%d) must be greater than zero", tag);
    check(count >= 0, "count (%d) must be greater than zero", count);

    // DT has large counts so this is no longer helpful
    // const int max_count = 8388608; // 2**23  28303360
    // check(count < max_count, "count (%d) must be less than %d", count,
    // max_count);

} // function CheckInvariants

void BundleChannelEndpoint::SetBundleChannel(BundleChannel* const bc) {
    assert(bundle_channel == nullptr);
    assert(bc != nullptr);
    bundle_channel = bc;

    ts_log_info(KCYN
                "Set bundle_channel for BundleChannelEndpoint %p to %p" KRESET,
                this, bundle_channel);
    channel_type = PureRT::ChannelEndpointType::BUNDLE;
}

void BundleChannelEndpoint::SetProcessChannel(ProcessChannel* const pc) {
    assert(process_channel == nullptr);
    assert(pc != nullptr);
    process_channel = pc;
    channel_type    = PureRT::ChannelEndpointType::PROCESS;
}

void BundleChannelEndpoint::SetDirectMPIChannel(DirectMPIChannel* const dmc) {
    assert(direct_mpi_channel == nullptr);
    assert(dmc != nullptr);
    direct_mpi_channel = dmc;
    channel_type       = PureRT::ChannelEndpointType::DIRECT_MPI;
}

void BundleChannelEndpoint::SetRdmaMPIChannel(RdmaMPIChannel* const rmc) {
    assert(rdma_mpi_channel == nullptr);
    assert(rmc != nullptr);
    rdma_mpi_channel = rmc;
    channel_type     = PureRT::ChannelEndpointType::RDMA_MPI;
}

void BundleChannelEndpoint::SetSendToSelfChannel(
        SendToSelfChannel* const stsc) {
    assert(send_to_self_channel == nullptr);
    assert(stsc != nullptr);
    send_to_self_channel = stsc;
    channel_type         = PureRT::ChannelEndpointType::SEND_TO_SELF;
}

void BundleChannelEndpoint::WaitForInitialization() const {
    switch (channel_type) {
    case PureRT::ChannelEndpointType::PROCESS:
        break;
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        check(direct_mpi_channel != nullptr, "direct_mpi_channel must be "
                                             "non-nullptr when calling "
                                             "WaitForInitialization");
        break;
    case PureRT::ChannelEndpointType::DIRECT_BATCHER:
        break;
    case PureRT::ChannelEndpointType::RDMA_MPI:
        check(rdma_mpi_channel != nullptr, "rdma_mpi_channel must be "
                                           "non-nullptr when calling "
                                           "WaitForInitialization");
        break;
    default:
        sentinel("Not implemented for unknown transport channel type %d (%p)",
                 channel_type, &channel_type);
    }
}

int BundleChannelEndpoint::NumPayloadBytes() const {
    ts_log_info("[CET%d](%d->%d) NumPayloadBytes with datatype %d and count %d",
                channel_endpoint_tag, sender_pure_rank, dest_pure_rank,
                (int)datatype, count);
    return (GetDatatypeBytes() * count);
}

string BundleChannelEndpoint::TransportChannelToString() const {

    // WARNING: do not call ts_log_info or related, as this function is often
    // used as arguments to
    // those functions, and it will deadlock.
    switch (channel_type) {
    case PureRT::ChannelEndpointType::PROCESS:
        sentinel(
                "TransportChannelToString: Not implemented for ProcessChannel");
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        sentinel("TransportChannelToString: Not implemented for "
                 "DirectMPIChannel");
    default:
        sentinel("Not implemented for unknown transport channel type %d (%p)",
                 channel_type, &channel_type);
    } // switch on channel_type
}
