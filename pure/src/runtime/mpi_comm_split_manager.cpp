// Author: James Psota
// File:   mpi_comm_split_manager.cpp 

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

#include "pure/runtime/mpi_comm_split_manager.h"
#include "pure/runtime/pure_thread.h"

// here "ranks" are MPI ranks.
MPI_Comm MpiCommSplitManager::GetOrCreateCommFromRanks(
        PureThread* pt, const std::vector<int>& ranks, MPI_Comm origin_comm) {

    fprintf(stderr, KRED "wr%d -- split manager %p. lock is %p\n" KRESET,
            pt->Get_rank(), this, &process_lock);

    // hack
    sleep(pt->GetThreadNumInProcess());

    std::lock_guard<std::mutex> lg(process_lock);
    //////////////////////////////////

    MpiCommManagerMetadata key(ranks, origin_comm);
    const auto             iter = existing_comms.find(key);

    MPI_Comm new_comm;
    if (iter == existing_comms.end()) {

        fprintf(stderr,
                KYEL
                ">> didn't find an existing comm with correct ranks and orign "
                "comm. trying to create a new one with tag %d...\n" KRESET,
                mpi_comm_creation_tag_for_mpi_process);

        new_comm = PureRT::create_mpi_comm_tag(
                ranks, origin_comm, mpi_comm_creation_tag_for_mpi_process);
        existing_comms.insert({key, new_comm});

#define DEBUG_PRINT_MPI_COMM_SPLIT_MGR 1
#if DEBUG_PRINT_MPI_COMM_SPLIT_MGR
        fprintf(stderr,
                KCYN "wr%d  [currently %d comms] created new MPI Comm from "
                     "origin comm %d with tag "
                     "%d and ranks [",
                pt->Get_rank(), existing_comms.size(), origin_comm,
                mpi_comm_creation_tag_for_mpi_process);
        for (auto r : ranks) {
            fprintf(stderr, "%d ", r);
        }
        fprintf(stderr, "]\n" KRESET);
#endif

        ++mpi_comm_creation_tag_for_mpi_process;
    } else {
        new_comm = iter->second;
    }

    return new_comm;
}
