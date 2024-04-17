// Author: James Psota
// File:   process_channel.h 

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

#ifndef PURE_TRANSPORT_PROCESS_CHANNEL_H
#define PURE_TRANSPORT_PROCESS_CHANNEL_H

#pragma once

#include <boost/preprocessor/stringize.hpp>

// we use PROCESS_CHANNEL_VERSION to choose the correct version. Make sure this
// is set in the environment. add additional options in
// $CPL/support/determine_preprocessor_vars.rb

#ifdef PROCESS_CHANNEL_VERSION
#if RUNTIME_IS_PURE
#if PROCESS_CHANNEL_MAJOR_VERSION == 1
#include "pure/transport/process_channel_v1.h"
using namespace process_channel_v1;
#elif PROCESS_CHANNEL_MAJOR_VERSION == 2
#include "pure/transport/process_channel_v2.h"
using namespace process_channel_v2;
#elif PROCESS_CHANNEL_MAJOR_VERSION == 3
#include "pure/transport/process_channel_v3.h"
using namespace process_channel_v3;
#elif PROCESS_CHANNEL_MAJOR_VERSION == 4
#include "pure/transport/process_channel_v4.h"
// using namespace process_channel_v4;
#else
#error "You must specify a PROCESS_CHANNEL_VERSION of 1, 2x, 3x, 4x in the environment."
#endif
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
#ifndef PCV_4_NUM_CONT_CHUNKS
#error PCV_4_NUM_CONT_CHUNKS must be defined if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL is defined
#endif
#endif

#endif
#endif