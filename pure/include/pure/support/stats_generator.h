// Author: James Psota
// File:   stats_generator.h 

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

#ifndef PURE_SUPPORT_STATS_GENERATOR_H
#define PURE_SUPPORT_STATS_GENERATOR_H

#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>

//#include "pure/3rd_party/jsoncpp/json.h"

#include "pure/support/benchmark_timer.h"
#include "pure/support/helpers.h"
#include "json/json.h"

namespace PureRT {

std::string
generate_stats(int pure_thread_rank, const BenchmarkTimer& end_to_end_timer,
               const std::vector<BenchmarkTimer*>& user_custom_timers =
                       std::vector<BenchmarkTimer*>(),
               const std::vector<BenchmarkTimer*>& runtime_custom_timers =
                       std::vector<BenchmarkTimer*>());

#if COLLECT_THREAD_TIMELINE_DETAIL
void TimerIntervalDetailsToCsv(
        int rank, BenchmarkTimer& end_to_end_timer,
        const std::vector<BenchmarkTimer*>& user_custom_timers,
        const std::vector<BenchmarkTimer*>& runtime_custom_timers =
                std::vector<BenchmarkTimer*>());
#endif

} // namespace PureRT
#endif
