// Author: James Psota
// File:   channel_manager.h 

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

#ifndef CHANNEL_MANAGER_H
#define CHANNEL_MANAGER_H

#include <boost/functional/hash.hpp>

class PureComm;

struct ChannelMetadata {
    PureComm* const    pure_comm;
    const int          count;
    const MPI_Datatype datatype;
    const int          partner_pure_rank;
    const int          tag;
    const MPI_Op       op;

    ChannelMetadata(int count_arg, MPI_Datatype datatype_arg,
                    int partner_pure_rank_arg, int tag_arg, PureComm* const pc,
                    MPI_Op op_arg = MPI_OP_NULL)
        : pure_comm(pc), count(count_arg), datatype(datatype_arg),
          partner_pure_rank(partner_pure_rank_arg), tag(tag_arg), op(op_arg) {}

    bool operator==(const ChannelMetadata& other) const {
        return (count == other.count && datatype == other.datatype &&
                partner_pure_rank == other.partner_pure_rank &&
                tag == other.tag && pure_comm == other.pure_comm &&
                op == other.op);
    }
};

struct ChannelMetadataHasher {
    // N.B. inspired by
    // http://stackoverflow.com/questions/17016175/c-unordered-map-using-a-custom-class-type-as-the-key
    size_t operator()(const ChannelMetadata& cm) const {
        using boost::hash_combine;
        using boost::hash_value;

        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Modify 'seed' by XORing and bit-shifting in
        // one member of it after the other:
        hash_combine(seed, hash_value(cm.count));
        hash_combine(seed, hash_value(static_cast<unsigned int>(cm.datatype)));
        hash_combine(seed, hash_value(cm.partner_pure_rank));
        hash_combine(seed, hash_value(cm.tag));
        hash_combine(seed, hash_value(cm.op));
        hash_combine(seed, hash_value(cm.pure_comm));

        return seed;
    } // function operator
};

#endif // header guard
