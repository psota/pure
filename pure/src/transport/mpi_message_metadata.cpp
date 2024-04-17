// Author: James Psota
// File:   mpi_message_metadata.cpp 

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

#include "pure/transport/mpi_message_metadata.h"
#include "pure/support/zed_debug.h"

// TODO(jim): change to a const
#define METADATA_FORMAT_STRING "%06u%07u%010u%05u%05d%05u%05u"
int MPIMessageMetadata::MetadataNumChars = (6 + 7 + 10 + 5 + 5 + 5 + 5);
int MPIMessageMetadata::MetadataBufLen =
        MPIMessageMetadata::MetadataNumChars + 1; // add null terminator

MPIMessageMetadata::MPIMessageMetadata(
        void* buf_arg, void** user_buf_ptr_arg, int count_arg,
        MPI_Datatype datatype_arg, int pure_dest_arg, int tag_arg,
        int pure_sender_arg, int seq_id_arg, RecvChannel* recv_channel_arg,
        PureRT::BufferAllocationScheme buffer_allocation_scheme_arg)
    : count(count_arg), datatype(datatype_arg), pure_dest(pure_dest_arg),
      tag(tag_arg), pure_sender(pure_sender_arg), seq_id(seq_id_arg),
      buffer_allocation_scheme(buffer_allocation_scheme_arg),
      recv_channel(recv_channel_arg) {

    // TODO: clean out this user buffer section.
    if (buffer_allocation_scheme_arg ==
        PureRT::BufferAllocationScheme::user_buffer) {
        orig_buf = buf_arg;
    } else {
        orig_buf = nullptr;
    }
    // TODO(jim): error checking on this?
    // TODO: clean out this user buffer section.
    user_buf_ptr = user_buf_ptr_arg;

    int dt_bytes;
    int ret     = MPI_Type_size(datatype, &dt_bytes);
    count_bytes = dt_bytes * count;

#if DEBUG_CHECK

    // NOTE: IMPORTANT: these checks make sure the character widths for the MPI
    // Message Header are
    // wide enough.
    // change constant METADATA_FORMAT_STRING above to address

    assert(count >= 0);
    check(count <= 999999,
          "desired count = %d, but header not wide enough to fit that", count);

    assert(count_bytes >= 0);
    check(count_bytes <= 9999999,
          "desired count_bytes = %d, but header not wide enough to fit that",
          count_bytes);

    check((count_bytes % count == 0), "count_bytes=%d, count=%d", count_bytes,
          count);

    assert((unsigned int)datatype > 0);
    assert((unsigned int)datatype <= UINT_MAX); // seems like the type system
                                                // will automatically handle
                                                // this?

    assert(pure_dest >= 0);
    assert(pure_dest <= 99999);

    assert(tag >= 0);

    assert(pure_sender >= 0);

    assert(seq_id >= 0);
    assert(seq_id <= 99999);

    assert(pure_dest >= 0);
    assert(pure_dest <= 99999);

    assert(tag >= 0);
    assert(tag <= 99999);

    assert(pure_sender >= 0);
    assert(pure_sender <= 99999);

#endif
}

std::string MPIMessageMetadata::SerializationFormat() {

    std::string fmt = "<count[6] eg: 000012><count_bytes[7] eg: "
                      "0000112><datatype[10] eg: "
                      "1275070475><pure_dest[5] eg: 00002><tag[5] eg: "
                      "00004><pure_sender[5] eg: "
                      "00014><seq_id[5] eg: 00120>";
    return fmt;
}

void MPIMessageMetadata::Serialize(const BundleChannelEndpoint& bce,
                                   void*                        buf) {

    snprintf(static_cast<char*>(buf), MPIMessageMetadata::MetadataBufLen,
             METADATA_FORMAT_STRING, bce.GetCount(), bce.NumPayloadBytes(),
             static_cast<unsigned int>(bce.GetDatatype()),
             bce.GetDestPureRank(), bce.GetTag(), bce.GetSenderPureRank(),
             -1000);
}

// static function
void MPIMessageMetadata::Parse(const char* md_string, int* count_buf,
                               int* count_buf_bytes, unsigned int* datatype_buf,
                               int* dest_buf, int* tag_buf, int* sender_buf,
                               int* seq_id_buf) {
    sscanf(md_string, METADATA_FORMAT_STRING, count_buf, count_buf_bytes,
           datatype_buf, dest_buf, tag_buf, sender_buf, seq_id_buf);

    check((*count_buf_bytes % *count_buf == 0),
          "count_buf_bytes=%d, count_buf=%d", *count_buf_bytes, *count_buf);
}

// static function
void MPIMessageMetadata::BufCounts(const char* md_string, int* count_buf,
                                   int* count_buf_bytes) {
    int          dummy;
    unsigned int udummy;
    MPIMessageMetadata::Parse(md_string, count_buf, count_buf_bytes, &udummy,
                              &dummy, &dummy, &dummy, &dummy);
}

string MPIMessageMetadata::ToString() {
    stringstream ss;

    ss << "MPIMessageMetadata at " << this << ": ";
    ss << "(" << pure_sender << "->" << pure_dest << ") ";
    ss << "orig_buf: " << orig_buf << ", ";

    if (user_buf_ptr != nullptr) {
        ss << "user_buf_ptr: " << *user_buf_ptr << ", ";
    }
    ss << "count: " << count << ", ";
    ss << "datatype: " << static_cast<unsigned int>(datatype) << ", ";
    ss << "tag: " << tag;

    return ss.str();

} // function ToString
