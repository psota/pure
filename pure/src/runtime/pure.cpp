// Author: James Psota
// File:   pure.cpp 

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

#include "pure/common/pure.h"
#include "pure/runtime/pure_process.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/mpi_message_metadata.h"
#include <boost/preprocessor/stringize.hpp>
#include <cassert>
#include <fstream>
#include <sys/types.h>
#include <unistd.h>

// static function
int PureRT::Pure::Run(int argc, char const* argv[]) {

    // Get threads per proc from environment (not a preprocessor variable so it
    // doesn't have to be redefined)
    int threads_per_proc = -1;
    if (const char* env_t = std::getenv("PURE_RT_NUM_THREADS")) {
        threads_per_proc = atoi(env_t);
        check_always(threads_per_proc > 0, "threads_per_proc is %d",
                     threads_per_proc);
        check_always(threads_per_proc <= 272, "too many threads per proc (%d)",
                     threads_per_proc); // sanity check -- current max is KNL
                                        // nodes on Cori
    } else {
        sentinel("PURE_RT_NUM_THREADS is not in the environment but must be");
    }

    // Get thread to proc mapping from environment
    char* thread_map_filename;
    if ((thread_map_filename = std::getenv("THREAD_MAP_FILENAME"))) {
        if (strlen(thread_map_filename) <= 0) {
            // we treat an empty filename the same as no filename
            thread_map_filename = nullptr;
        } else {
            // make sure the file exists
            std::ifstream f(thread_map_filename);
            check(f.good(), "Unable to open thread map file %s",
                  thread_map_filename);
        }
    }

    int mpi_size = -1;
    int rank     = -1;

    // fprintf(stderr, "MPI_ENABLED is %d\n", MPI_ENABLED);

#if MPI_ENABLED
    // initialize thread-safe version of MPI. N.B.
    // http://www.mpich.org/static/docs/v3.1/www3/MPI_Init_thread.html
    // Optimization possibility: do not use thread-safe version of MPI and
    // instead use our own synchronization.
    int init_ret;
    int init_provided_thread_support =
            -1; // verify that this gets overridden by MPI_Init_thread
    bool mt_mode;
    if (threads_per_proc == 1 || FORCE_MPI_THREAD_SINGLE) {
        // no thread support -- although this doesn't allow the progress engine!
        // beware.
        init_ret = MPI_Init_thread(&argc, const_cast<char***>(&argv),
                                   MPI_THREAD_SINGLE,
                                   &init_provided_thread_support);
        assert(init_provided_thread_support == MPI_THREAD_SINGLE);
        mt_mode = false;
    } else {
        init_ret = MPI_Init_thread(&argc, const_cast<char***>(&argv),
                                   MPI_THREAD_MULTIPLE,
                                   &init_provided_thread_support);
        assert(init_provided_thread_support == MPI_THREAD_MULTIPLE);
        mt_mode = true;
    }
    assert(init_ret == MPI_SUCCESS);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank == 0) {
        if (mt_mode) {
            printf(KCYN "MPI running in multi-threaded mode.\n" KRESET);
        } else {
            printf(KLT_RED "NO MPI multi-threaded support!\n" KRESET);
        }
    }

#if ENABLE_MPI_ERROR_RETURN
    MPI_Errhandler_set(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
#endif

    MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    check(mpi_size > 1,
          "MPI_ENABLED is set to true, but MPI_Comm_size "
          "returned %d and MPI should only be enabled if running "
          "with more than one process.",
          mpi_size);

#else
    mpi_size = 1;
    rank     = 0;
#endif

#if PAUSE_FOR_DEBUGGER_ATTACH
    if (rank <= 3) {
        const auto sleep_sec = 14;
        if (rank == 0) {
            fprintf(stderr,
                    KYEL "\aPausing for %d seconds to attach a "
                         "debugger...\n\n" KRESET,
                    sleep_sec);
        }

        pid_t pid = getpid();
        fprintf(stderr,
                "  MPI PROCESS %d on %s:     gdb -p %d    or     "
                "lldb "
                "-p %d\n\n",
                rank, PureRT::hostname_string().c_str(), static_cast<int>(pid),
                static_cast<int>(pid));
        sleep(sleep_sec);
    }
#if MPI_ENABLED
    MPI_Barrier(MPI_COMM_WORLD);
#endif
#endif

#if FORCE_MPI_THREAD_SINGLE
    if (rank == 0) {
        fprintf(stderr,
                KRED "WARNING: FORCE_MPI_THREAD_SINGLE is enabled\n" KRESET);
    }
#endif

    int total_threads;
    if (const char* env_t = std::getenv("TOTAL_THREADS")) {
        total_threads = atoi(env_t);
    }
    assert(total_threads > 0);
    assert(total_threads <= 32768); // sanity check

    // builds singleton PureProcess (which, in turn, builds PureThreads)
    auto pure_process_singleton =
            new PureProcess(argc, argv, mpi_size, threads_per_proc,
                            total_threads, thread_map_filename);

#ifdef CORI_ARCH
    if (rank == 0) {
        fprintf(stderr, "CORI_ARCH: %s\n", BOOST_PP_STRINGIZE(CORI_ARCH));
    }
#endif
    // now actually run the program
    int ret = pure_process_singleton->RunPureThreads();

    // program is done running; clean up
    delete (pure_process_singleton);

    // special handling for cori and jemalloc and mpi

#ifdef CORI_ARCH
#if USE_JEMALLOC != 1 && MPI_ENABLED
    if (rank == 0) {
        fprintf(stderr, "Calling MPI Finalize -- call site 1.\n");
    }
    MPI_Finalize();
#endif
#else
    // not cori
#if MPI_ENABLED
    if (rank == 0) {
        fprintf(stderr, "Calling MPI Finalize -- call site 2.\n");
    }
    MPI_Finalize();
#endif
#endif

    return ret;
}

#ifdef DEBUG_CHECK

// approach: write some global static vars and make sure they jive with the env
// vars
constexpr static int __STATIC_PROCESS_CHANNEL_VERSION = PROCESS_CHANNEL_VERSION;
constexpr static int __STATIC_PROCESS_CHANNEL_MAJOR_VERSION =
        PROCESS_CHANNEL_MAJOR_VERSION;
constexpr static int __STATIC_PROCESS_CHANNEL_BUFFERED_MSG_SIZE =
        PROCESS_CHANNEL_BUFFERED_MSG_SIZE;
constexpr static int __STATIC_PRINT_PROCESS_CHANNEL_STATS =
        PRINT_PROCESS_CHANNEL_STATS;
constexpr static int __STATIC_COLLECT_THREAD_TIMELINE_DETAIL =
        COLLECT_THREAD_TIMELINE_DETAIL;
constexpr static int __STATIC_BUFFERED_CHAN_MAX_PAYLOAD_BYTES =
        BUFFERED_CHAN_MAX_PAYLOAD_BYTES;

void runtime_verify_env_vars(char** envp) {

    assert(__STATIC_PROCESS_CHANNEL_VERSION == PROCESS_CHANNEL_VERSION);
    assert_env_var_value<int>("PROCESS_CHANNEL_VERSION",
                              __STATIC_PROCESS_CHANNEL_VERSION);

    assert(__STATIC_PROCESS_CHANNEL_MAJOR_VERSION ==
           PROCESS_CHANNEL_MAJOR_VERSION);
    assert_env_var_value<int>("PROCESS_CHANNEL_MAJOR_VERSION",
                              __STATIC_PROCESS_CHANNEL_MAJOR_VERSION);

    assert(__STATIC_PROCESS_CHANNEL_BUFFERED_MSG_SIZE ==
           PROCESS_CHANNEL_BUFFERED_MSG_SIZE);
    assert_env_var_value<int>("PROCESS_CHANNEL_BUFFERED_MSG_SIZE",
                              __STATIC_PROCESS_CHANNEL_BUFFERED_MSG_SIZE);

    assert(__STATIC_PRINT_PROCESS_CHANNEL_STATS == PRINT_PROCESS_CHANNEL_STATS);
    assert_env_var_value<int>("PRINT_PROCESS_CHANNEL_STATS",
                              __STATIC_PRINT_PROCESS_CHANNEL_STATS);

    assert(__STATIC_COLLECT_THREAD_TIMELINE_DETAIL ==
           COLLECT_THREAD_TIMELINE_DETAIL);
    assert_env_var_value<int>("COLLECT_THREAD_TIMELINE_DETAIL",
                              __STATIC_COLLECT_THREAD_TIMELINE_DETAIL);

    assert(__STATIC_BUFFERED_CHAN_MAX_PAYLOAD_BYTES ==
           BUFFERED_CHAN_MAX_PAYLOAD_BYTES);
    assert_env_var_value<int>("BUFFERED_CHAN_MAX_PAYLOAD_BYTES",
                              __STATIC_BUFFERED_CHAN_MAX_PAYLOAD_BYTES);

#ifdef PCV_4_NUM_CONT_CHUNKS
    assert(PCV_4_NUM_CONT_CHUNKS > 0);
#endif
}

#endif

// this is the actual main that gets run
int main(int argc, char const* argv[], char** envp) {

#ifdef DEBUG_CHECK
    runtime_verify_env_vars(envp);
#endif

    int ret = PureRT::Pure::Run(argc, argv);
    if (ret != EXIT_SUCCESS) {
        fprintf(stderr, KRED "FAILURE: Return value from main: %d\n" KRESET,
                ret);
    }
    return ret;
}
