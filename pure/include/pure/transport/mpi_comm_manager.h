// Author: James Psota
// File:   mpi_comm_manager.h 

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

#ifndef MPI_COMM_MANAGER_H
#define MPI_COMM_MANAGER_H

#include "assert.h"
#include "mpi.h"
#include <cstring>

#define MPI_COMM_MANAGER_ENABLED (MPI_ENABLED && USE_RDMA_MPI_CHANNEL)

class MPICommManager {

  public:
    MPICommManager(int nprocs) : nprocs_(nprocs) {

#if MPI_COMM_MANAGER_ENABLED
        MPI_Group world_group;
        MPI_Comm_group(MPI_COMM_WORLD, &world_group);

        // num_pairs_ = nprocs * (nprocs - 1) / 2;
        // store comms_ as a 1d array
        const auto num_pairs = nprocs_ * nprocs_;
        comms_               = new MPI_Comm[num_pairs];
        std::memset(comms_, 0, num_pairs * sizeof(MPI_Comm));

        // remove
        int rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        int created = 0;

        // populate it, creating a new MPI_Comm on each iteration
        for (auto i = 0; i < nprocs_; ++i) {
            for (auto j = i + 1; j < nprocs_; ++j) {

                // we always access the array with a <lower_rank, higher_rank>
                // order, so we only populate those entries as well
                assert(j > i);

                // first create the group.
                MPI_Group pair_group;
                const int ranks[2] = {i, j};
                MPI_Group_incl(world_group, 2, ranks, &pair_group);

                MPI_Comm   new_comm;
                const auto ret =
                        MPI_Comm_create(MPI_COMM_WORLD, pair_group, &new_comm);
                assert(ret == MPI_SUCCESS);
                ++created;

                if (rank != i && rank != j) {
                    assert(new_comm == MPI_COMM_NULL);
                }

                // now, insert it into the repository
                const auto idx = IndexOrdered(i, j);
                assert(comms_[idx] == 0);
                comms_[idx] = new_comm;

                printf("[r%d] new comm for (%d, %d) -> %d   (idx: %d)\n", rank,
                       i, j, new_comm, idx);
            }
        }
        assert(created == nprocs_ * (nprocs_ - 1) / 2);
#endif
    }

    ~MPICommManager() {
#if MPI_COMM_MANAGER_ENABLED
        for (auto i = 0; i < nprocs_ * nprocs_; ++i) {
            if (comms_[i] != 0 && comms_[i] != MPI_COMM_NULL) {
                MPI_Comm_free(&comms_[i]);
            }
        }
        delete[] comms_;
#endif
    }

    MPI_Comm Get(int comm_world_rank1, int comm_world_rank2) const {
        assert(comm_world_rank1 != comm_world_rank2);
        return comms_[Index(comm_world_rank1, comm_world_rank2)];
    }

  private:
    MPI_Comm* comms_;
    int       nprocs_;

    int Index(int r1, int r2) const {
        // we always look up the "top half" of the matrix
        if (r1 < r2) {
            return IndexOrdered(r1, r2);
        } else {
            return IndexOrdered(r2, r1);
        }
    }

    int IndexOrdered(int lower, int higher) const {
        assert(lower < higher);
        return (lower * nprocs_) + higher;
    }
};

#endif
