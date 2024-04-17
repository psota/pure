// Author: James Psota
// File:   mpi_comm_split_manager.h 

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

#ifndef MPI_COMM_SPLIT_MANAGER_H
#define MPI_COMM_SPLIT_MANAGER_H

#include "mpi.h"
#include "pure/transport/mpi_pure_helpers.h"
#include <boost/functional/hash.hpp>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

class PureThread;

struct MpiCommManagerMetadata {

    std::set<int> ranks; // ordered
    MPI_Comm      origin_comm;

    MpiCommManagerMetadata(const std::vector<int>& ranks_vec,
                           MPI_Comm                origin_comm_arg)
        : ranks(ranks_vec.begin(), ranks_vec.end()),
          origin_comm(origin_comm_arg) {}

    bool operator==(const struct MpiCommManagerMetadata& other) const {
        return (other.origin_comm == origin_comm) && other.ranks == ranks;
        // return (other.origin_comm == origin_comm);
    }
};

struct MpiCommManagerMetadataHasher {
    // http://stackoverflow.com/questions/17016175/c-unordered-map-using-a-custom-class-type-as-the-key
    size_t operator()(const MpiCommManagerMetadata& k) const {
        using boost::hash_combine;
        using boost::hash_value;

        std::size_t seed = 0;
        hash_combine(seed, hash_value(k.origin_comm));

        for (auto r : k.ranks) {
            hash_combine(seed, hash_value(r));
        }
        return seed;
    }
};

// Manages MPI_Comms within a PureProcess so they are not duplicated
class MpiCommSplitManager {

  public:
    ~MpiCommSplitManager() {
        fprintf(stderr,
                KRED "MpiCommSplitManager dcon: created %lu MPI comms\n" KRESET,
                existing_comms.size());
    }

    // probbaly move the tag into this structure
    MPI_Comm GetOrCreateCommFromRanks(PureThread*             pt,
                                      const std::vector<int>& ranks,
                                      MPI_Comm                origin_comm);

  private:
    std::mutex process_lock;
    std::unordered_map<MpiCommManagerMetadata, MPI_Comm,
                       MpiCommManagerMetadataHasher>
            existing_comms;

    int mpi_comm_creation_tag_for_mpi_process = 0;
};

#endif