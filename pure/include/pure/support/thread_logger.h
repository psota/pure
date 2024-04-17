// Author: James Psota
// File:   thread_logger.h 

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

#ifndef PURE_SUPPORT_THREAD_LOGGER_H
#define PURE_SUPPORT_THREAD_LOGGER_H

#pragma once

#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

using std::string;

#include "pure/support/helpers.h"
//#include "pure/runtime/pure_thread.h"

// forward declarations
class PureThread;

using std::mutex;
using std::ofstream;
using std::string;
using std::stringstream;
using std::thread;
using std::unordered_map;

typedef struct _ThreadLogMetadata {
    int         rank;
    PureThread* pure_thread;
    string      filename;
    ofstream    stream;

    explicit _ThreadLogMetadata(PureThread* /*pure_thread_*/);

} ThreadLogMetadata;

class ThreadLogger {

  public:
    ThreadLogger();
    virtual ~ThreadLogger();

    static void log(char* /*msg*/, bool print_rank = true);
    static void initialize(PureThread* /*pure_thread*/);
    static void finalize();
#if ENABLE_THREAD_LOGGER
    static PureThread* get_pure_thread();
#endif
    static inline string process_thread_name() {
        /* TODO: this should only need the thread not the
           process name. Simply this code by putting the call to
           get_id() in the constructor directly.
          */
        thread::id   this_id = std::this_thread::get_id();
        stringstream fname;
        // TODO(jim): directory structure
        fname << "p" << getpid() << "_t" << this_id;
        return fname.str();
    }

  private:
    static mutex                                     thread_log_map_mutex;
    static unordered_map<string, ThreadLogMetadata&> thread_log_map;
    static ThreadLogMetadata&                        get_metadata();

}; // class ThreadLogger

#endif
