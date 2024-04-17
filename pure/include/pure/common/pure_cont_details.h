// Author: James Psota
// File:   pure_cont_details.h 

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

#ifndef PURE_CONT_DETAILS_H
#define PURE_CONT_DETAILS_H

//#include "pure/support/zed_debug.h"
#include <assert.h>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdint.h>
#include <variant>
#include <vector>

// we have to deal with this override this here, as the user application and
// also below uses PCV_4_NUM_CONT_CHUNKS directly.
#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
// special case - override back to 2 if it happened to be set in the application
// makefile
#define PCV_4_NUM_CONT_CHUNKS 2
#endif
#define PCV_4_LAST_CHUNK (PCV_4_NUM_CONT_CHUNKS - 1)

// this goes in the application at the end of the program (e.g., just before
// end_to_end_timer.pause() )
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL && \
        DO_PURE_EXHAUST_REMAINING_WORK
#define PURE_EXHAUST_REMAINING_WORK pure_thread_global->ExhaustRemainingWork()
#else
#define PURE_EXHAUST_REMAINING_WORK
#endif

// USAGE:
// use PureContinuation for a continuation that doesn't return a value
// use ReturningPureContinuation that returns a value

enum class PureRTContExeMode { CHUNK, RECEIVER_FULL };

// If you want to add a non-default-constructable type to the variant, add
// std::monostate
// TODO: extend this over time as needed for different applications

using PureContRetT =
        std::variant<std::monostate, bool, unsigned int, int, double, void*>;
using PureContRetArr = std::vector<PureContRetT>;

using chunk_id_t = int_fast16_t;

// TODO in the future: consider passing in return value of previous stage to the
// next stage of the pipeline
using VoidPureContinuation =
        std::function<void(void*, int, PureRTContExeMode, chunk_id_t,
                           chunk_id_t, std::optional<void*>)>;

// reminder: return a value from a ReturningPureContinuation wrapped in a
// variant, such as  "return PureContRetT{csum};"
using ReturningPureContinuation =
        std::function<PureContRetT(void*, int, PureRTContExeMode, chunk_id_t,
                                   chunk_id_t, std::optional<void*>)>;

// this was is only to be used in the runtime system.
// TODO: move this to pure_process.h or similar
using PureContinuation = std::variant<std::monostate, VoidPureContinuation,
                                      ReturningPureContinuation>;
using ReducingPureContinuation =
        std::function<PureContRetT(PureContRetT, PureContRetT)>;

struct PureContDetails {

    PureContinuation         cont;
    ReducingPureContinuation reducer;
    PureContRetT             reducer_init_val;
    bool                     collab;

    PureContDetails(PureContinuation&& cont_, bool collab_,
                    ReducingPureContinuation&& reducer_ = nullptr,
                    PureContRetT reducer_init_val_      = PureContRetT{})
        : cont(std::move(cont_)), reducer(std::move(reducer_)),
          reducer_init_val(reducer_init_val_), collab(collab_) {
#if DEBUG_CHECK
        if (std::holds_alternative<ReturningPureContinuation>(cont) && collab) {
            if (reducer == nullptr) {
                fprintf(stderr, "If the continuation is declared a"
                                "ReturningPureContinuation "
                                "and is collaborative, the"
                                "associated reducer must be non-null");
                std::abort();
            }
        }
#endif
    };

    PureContDetails(const PureContDetails& other) {
        // fprintf(stderr,
        //          KRED "WARNING: PureContDetails copy constructor called\n"
        //          KRESET);
        cont             = other.cont;
        reducer          = other.reducer;
        reducer_init_val = other.reducer_init_val;
        collab           = other.collab;
    }

    PureContDetails& operator=(const PureContDetails& other) {
        // fprintf(stderr,
        //         KRED "WARNING: PureContDetails copy assignment called\n"
        //         KRESET);
        cont             = other.cont;
        reducer          = other.reducer;
        reducer_init_val = other.reducer_init_val;
        collab           = other.collab;
        return *this;
    }

    PureContDetails& operator=(PureContDetails&& other) = delete;
    inline void      AssertNonmonotoneCont() const { assert(cont.index() > 0); }
};

#endif