// Author: James Psota
// File:   mpi_pure_helpers.h 

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

#ifndef MPI_PURE_HELPERS_H
#define MPI_PURE_HELPERS_H

#include "mpi.h"
#include "pure/runtime/pure_thread.h"
#include <type_traits>

// Global pure_thread variable for application to optionally set in orig_main

// oct 2022 -- removing this to deal with linkage issue. include pure.h if
// necessary.
// extern thread_local PureThread* pure_thread_global;

namespace PureRT {

// Waits for an MPI request to complete, doing some work stealing while it's
// waiting if work stealing is enabled.
void do_mpi_wait_with_work_steal(PureThread* const pt, MPI_Request* const req,
                                 MPI_Status* status);

MPI_Comm create_mpi_comm_tag(const std::vector<int>& mpi_ranks,
                             MPI_Comm origin_comm, int unique_comm_tag);

MPI_Comm create_mpi_comm(const std::vector<int>& mpi_ranks,
                         MPI_Comm                origin_comm);

template <typename T>
MPI_Datatype MPI_Datatype_from_typename() {
    MPI_Datatype d;

    if (std::is_same<T, double>())
        return MPI_DOUBLE;
    if (std::is_same<T, int>())
        return MPI_INT;
    if (std::is_same<T, long long int>())
        return MPI_LONG_LONG_INT;

    sentinel("Failed to match type. Add to MPI_Datatype_from_typename");
}

}; // namespace PureRT

#endif
