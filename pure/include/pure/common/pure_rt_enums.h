// Author: James Psota
// File:   pure_rt_enums.h 

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

#ifndef PURE_COMMON_PURE_RT_ENUMS_H
#define PURE_COMMON_PURE_RT_ENUMS_H

#pragma once

#include <functional>
#include <string>

// work stealing helpers
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
#define MAYBE_WORK_STEAL(thread) \
    thread->MaybeExecuteStolenWorkOfRandomReceiver()
// predicate version
#define MAYBE_WORK_STEAL_COND(thread, predicate)          \
    if (predicate) {                                      \
        thread->MaybeExecuteStolenWorkOfRandomReceiver(); \
    }
#else
#define MAYBE_WORK_STEAL(thread)                 // do nothing
#define MAYBE_WORK_STEAL_COND(thread, predicate) // do nothing
#endif

namespace PureRT {

// Pure types
using channel_endpoint_tag_t = unsigned int;
using bundle_size_t          = size_t;

static const int global_max_num_ranks = 16384;
static const int PURE_UNUSED_RANK     = -1001;
static const int PURE_UNUSED_TAG      = -2001;

// PURE_USER_MAX_TAG is the max tag a user can use. We use tags above
// PURE_USER_MAX_TAG internally with wrapper channels such as ScanChannel. This
// is mainly limited by Intel MPI and how we use upper bits in the tag for
// thread rank disambiguation.
static const int PURE_USER_MAX_TAG            = 498;
static const int PURE_DIRECT_CHAN_BATCHER_TAG = 499;
static const int PURE_INTERNAL_FIRST_RESERVED_TAG =
        PURE_DIRECT_CHAN_BATCHER_TAG + 1;

static const int                    PURE_UNUSED_BUNDLE_SIZE          = -6001;
static const channel_endpoint_tag_t CHANNEL_ENDPOINT_TAG_DEFAULT     = 8888888;
static const channel_endpoint_tag_t CHANNEL_ENDPOINT_TAG_SCAN_CHAN   = 8888880;
static const bundle_size_t          USER_SPECIFIED_BUNDLE_SZ_DEFAULT = -1;

// initializer function must deal with casting buffer
using buffer_init_func_t = std::function<void(void* buf)>;
using buffer_t           = void*;
using const_buffer_t     = const void* const;
using payload_count_type = uint32_t;
using seq_type           = std::uint_fast32_t;

// Allreduce small payload channel: works for ints and doubles; can make big
// later.

#define MAX_COUNT_SMALL_PAYLOAD_AR_CHAN 131
// #define MAX_COUNT_SMALL_PAYLOAD_AR_CHAN 1024

enum class BundleMode {
    none,
    one_msg_per_sender_thread,
    specified_channel_tag
};
enum class BufferAllocationScheme {
    invalid_scheme,
    user_buffer,
    runtime_buffer
};
enum class EndpointType { UNINITIALIZED, SENDER, RECEIVER, REDUCE, BCAST };

} // namespace PureRT
#endif
