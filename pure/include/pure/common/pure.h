// Author: James Psota
// File:   pure.h 

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

#ifndef PURE_COMMON_PURE_H
#define PURE_COMMON_PURE_H

#ifndef __cplusplus
#error "Must only include this file in from a C++ file"
#endif

#pragma once

#include "mpi.h"
#include <csignal>
#include <stdexcept>

#include "pure/common/pure_cont_details.h"
#include "pure/common/pure_pipeline.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/runtime/pure_comm.h"
#include "pure/runtime/pure_thread.h"
#include "pure/transport/bcast_channel.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/reduce_channel.h"
#include "pure/transport/send_channel.h"

// forward declarations
class BundledMessage;
class BundleChannel;
class MPIMessageMetadata;
class PureThread;

// Global pure_thread variable for application to optionally set in orig_main
extern thread_local PureThread* pure_thread_global;

#define PURE_RANK (pure_thread_global->Get_rank())
#define PURE_SIZE (pure_thread_global->Get_size())
#define PURE_COMM_WORLD (pure_thread_global->GetPureCommWorld())

// note: these are for "comm world"
#define PURE_BARRIER (pure_thread_global->Barrier(PURE_COMM_WORLD, true))
#define PURE_BARRIER_WORLD (pure_thread_global->Barrier(PURE_COMM_WORLD, true))
#define PURE_BARRIER_NO_WS (pure_thread_global->Barrier(PURE_COMM_WORLD, false))

// prettier aliases for thesis
using PureTask           = VoidPureContinuation;
using ReturningPureTask  = ReturningPureContinuation;
using ReducingPureTask   = ReducingPureContinuation;
using PureTaskReturnType = PureContRetT;
using PureTaskReturnArr  = PureContRetArr;

// debugging -- call this and then write to file handle "of". Then fclose(of)
// when done. make sure to clear out the "dmc_logs" dir before running as this
// appends strings.

#define WRITE_DMC_LOG 0

#define GET_DMC_LOG                                                     \
    mkdir_p("dmc_logs");                                                \
    const auto __tmap = strlen(std::getenv("THREAD_MAP_FILENAME")) > 0; \
    char       __fname[64];                                             \
    sprintf(__fname, "dmc_logs/r%.4d_dmc_%s.log", PURE_RANK,            \
            (__tmap ? "tmap" : "no_tmap"));                             \
    FILE* of = fopen(__fname, "a");                                     \
    check(of != nullptr,                                                \
          "Tried to open DMC logfile, but we didn't get a file handle.");

#define GET_DMC_LOG_SAME_SCOPE \
    assert(of == nullptr);     \
    of = fopen(__fname, "a");  \
    check(of != nullptr,       \
          "Tried to open DMC logfile, but we didn't get a file handle.");

#define DMC_CLOSE_LOG \
    fclose(of);       \
    of = nullptr;

// styleguide: https://google-styleguide.googlecode.com/svn/trunk/cppguide.html

namespace PureRT {

class Pure {
  private:
    inline explicit Pure() = default;
    inline ~Pure()         = default;

  public:
    static int Run(int /*argc*/, char const** /*argv*/);
}; // class Pure

}; // namespace PureRT

#endif
