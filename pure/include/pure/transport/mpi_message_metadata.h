// Author: James Psota
// File:   mpi_message_metadata.h 

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

#ifndef PURE_TRANSPORT_MPI_MESSAGE_METADATA_H
#define PURE_TRANSPORT_MPI_MESSAGE_METADATA_H

#pragma once

#include "mpi.h"
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sstream>
#include <string>

//#include "pure/common/pure.h"
#include "pure/common/pure_rt_enums.h"
//#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/mpi_message_metadata.h"
#include "pure/transport/recv_channel.h"

#define CHECK_ARGS 1

#define METADATA_STRING_FMT "%d"
#define METADATA_COUNT_WIDTH 5
#define METADATA_COUNT_BYTES_WIDTH 5
#define METADATA_COUNT_DATATYPE_WIDTH 10
#define METADATA_COUNT_PURE_DEST_WIDTH 5
#define METADATA_COUNT_TAG_WIDTH 5
#define METADATA_COUNT_PURE_SENDER_WIDTH 5
#define METADATA_COUNT_SEQ_ID_WIDTH 5

using std::string;

// forward declarations
class BundleChannelEndpoint;
class RecvChannel;

class MPIMessageMetadata {
  private:
    void*                          orig_buf;
    void**                         user_buf_ptr;
    int                            count;
    int                            count_bytes;
    MPI_Datatype                   datatype;
    int                            pure_dest;
    int                            tag;
    int                            pure_sender;
    int                            seq_id;
    PureRT::BufferAllocationScheme buffer_allocation_scheme; // only used for
                                                             // receiver record
    // keeping; not sent
    RecvChannel* recv_channel;

  public:
    MPIMessageMetadata(
            void* /*buf_arg*/, void** /*user_buf_ptr_arg*/, int /*count_arg*/,
            MPI_Datatype /*datatype_arg*/, int /*pure_dest_arg*/,
            int /*tag_arg*/, int /*pure_sender_arg*/, int /*seq_id_arg*/,
            RecvChannel*                   recv_channel = nullptr,
            PureRT::BufferAllocationScheme buffer_allocation_scheme_arg =
                    PureRT::BufferAllocationScheme::invalid_scheme);

    ~MPIMessageMetadata() = default;

    inline void* GetOrigBuf() {
        assert(buffer_allocation_scheme ==
               PureRT::BufferAllocationScheme::user_buffer);
        return orig_buf;
    }

    inline void** GetUserBufPtr() {
        assert(buffer_allocation_scheme ==
               PureRT::BufferAllocationScheme::runtime_buffer);
        return user_buf_ptr;
    }

    inline int          GetCount() { return count; }
    inline int          GetCountBytes() { return count_bytes; }
    inline MPI_Datatype GetDatatype() { return datatype; }
    inline int          GetPureDest() { return pure_dest; }
    inline int          GetTag() { return tag; }
    inline int          GetPureSender() { return pure_sender; }
    inline int          GetSeqId() { return seq_id; }
    inline PureRT::BufferAllocationScheme GetBufferAllocationScheme() {
        return buffer_allocation_scheme;
    }
    inline RecvChannel* GetRecvChannel() { return recv_channel; }

    static std::string SerializationFormat();
    static int         MetadataNumChars;
    static int         MetadataBufLen;
    static const char* MetadataFormatString();
    static void Parse(const char* md_string, int* count_buf,
                      int* count_buf_bytes, unsigned int* datatype_buf,
                      int* dest_buf, int* tag_buf, int* sender_buf,
                      int* seq_id_buf);
    static void BufCounts(const char* md_string, int* count_buf,
                          int* count_buf_bytes);
    static void Serialize(const BundleChannelEndpoint& /*bce*/, void* /*buf*/);

    string ToString();
};
#endif
