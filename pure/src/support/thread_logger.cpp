// Author: James Psota
// File:   thread_logger.cpp 

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

/*
 * TODO

    The mutex in log can get removed if all threads wait via barrier (or condition variable)
    until all other threads have initialized. Just pass in the total number of threads
    per process. The last one in signals to the others to go. Then, remove the locking in the
    ThreadLogger::log.
 */

#include "pure/support/thread_logger.h"
#if RUNTIME_IS_PURE
// this file is used in non-pure contexts, so only conditionally include pure_thread.h
#include "pure/runtime/pure_thread.h"

// constructor for struct defined in header
_ThreadLogMetadata::_ThreadLogMetadata(PureThread* pure_thread_) : pure_thread(pure_thread_) {

    rank = pure_thread->Get_rank();
    stringstream fname;
    fname << "logs/";
    fname << "rank_" << rank << ".log.ansi";
    filename = fname.str();
    stream.open(filename, std::ofstream::out);

    if (stream.fail()) {
        std::cerr << "ERROR: thread output file " << filename << " failed to open for writing. Exiting." << std::endl
                  << std::endl;
        exit(-202);
    }
}

// static variable definition
unordered_map<string, ThreadLogMetadata&> ThreadLogger::thread_log_map;
mutex ThreadLogger::thread_log_map_mutex;

ThreadLogger::ThreadLogger()  = default; // ctor
ThreadLogger::~ThreadLogger() = default; // dtor

// static function
void ThreadLogger::initialize(PureThread* pure_thread) {

#if ENABLE_THREAD_LOGGER
    {
        std::lock_guard<mutex> thread_log_map_lock(thread_log_map_mutex);
        string                 name = process_thread_name();

        auto search_output = thread_log_map.find(name);
        if (search_output == thread_log_map.end()) {

            auto md = new ThreadLogMetadata(pure_thread);

            // insert new entry
            auto ret = thread_log_map.insert({name, *md});
            check(ret.second, "insertion into thread_log_map for thread %s failed", name.c_str());
        } else {
            fprintf(stderr, "ERROR: this thread (id %s) was already initialized with the "
                            "ThreadLogger system. You must call ThreadLogger::iniitialize exactly "
                            "once per program run.\n",
                    name.c_str());
            exit(-200);
        }
    } // ends critical section
#endif
} // function initialize

// static function
ThreadLogMetadata& ThreadLogger::get_metadata() {
    {
        // FIXME: remove this lock as it shouldn't be necessary!
        // FIXME: remove this lock as it shouldn't be necessary!
        // FIXME: remove this lock as it shouldn't be necessary!
        // FIXME: remove this lock as it shouldn't be necessary!
        std::lock_guard<mutex> thread_log_map_lock(thread_log_map_mutex);

        string pt_name       = process_thread_name();
        auto   search_output = thread_log_map.find(pt_name);
        if (search_output == thread_log_map.end()) {

            fprintf(stderr, "ERROR: this thread (%s) not initialized with ThreadLogger system (map "
                            "size: %lu). You must call ThreadLogger::initialize with the rank to "
                            "initialize it before using it.\n\n",
                    pt_name.c_str(), thread_log_map.size());

            for (auto& i : thread_log_map) {
                if (i.first == pt_name) {
                    fprintf(stderr, "**");
                }
                fprintf(stderr, "\t%s -> %p\n", i.first.c_str(), &(i.second));
            }

            exit(-201);
        } else {
            ThreadLogMetadata& md = search_output->second;
            return md;
        }
    }
}

#if ENABLE_THREAD_LOGGER
// static function
PureThread* ThreadLogger::get_pure_thread() {
    ThreadLogMetadata& md = get_metadata();
    return md.pure_thread;
}
#endif

// static function
void ThreadLogger::log(char* msg, bool print_rank) {
#if ENABLE_THREAD_LOGGER

    ThreadLogMetadata& md = get_metadata();

    // write to both logfile and stderr. (and, force flushing so log files are updated even
    // if program is prematurely preempted).
    if (print_rank) {
        stringstream header;
        header << KLT_GREY << "[r" << md.rank << "] " << KRESET;
        string hdr_str = header.str();
        fprintf(stderr, "%s", hdr_str.c_str());
        md.stream << hdr_str;
    }
    fprintf(stderr, "%s", msg);
    fflush(stderr);
    md.stream << msg;
    md.stream.flush();

#endif
} // function log

// static function
void ThreadLogger::finalize() {
#if ENABLE_THREAD_LOGGER
    {
        std::lock_guard<mutex> thread_log_map_lock(thread_log_map_mutex);
        // close file handle and destroy this entry from the name map
        auto search_output = thread_log_map.find(process_thread_name());
        if (search_output == thread_log_map.end()) {
            fprintf(stderr, "ERROR: unable to find thread_log_map entry %s for finalization\n",
                    process_thread_name().c_str());
            exit(-201);
        } else {
            ThreadLogMetadata& md = search_output->second;
            md.stream.close();
            delete (&md);
            thread_log_map.erase(search_output);
        }
    } // ends critical section
#endif
}

// only currently allow this to work for the Pure runtime. TODO: generalize.
#endif
