// Author: James Psota
// File:   zed_debug.h 

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

#ifndef PURE_SUPPORT_ZED_DEBUG_H
#define PURE_SUPPORT_ZED_DEBUG_H

// currently only supported for C++ compilation units
// via http://c.learncodethehardway.org/book/ex20.html
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <mutex>
#include <thread>

#if MPI_ENABLED
#include "mpi.h"
#endif

extern void pthread_name(char* out_name);

namespace PureRT {

// note: some variables are defined in src/support/zed_debug.cpp

extern std::mutex __zed_debug_stderr_mutex; // for thread-safe locking of stderr

#define PRINT_FLAG_STATUS(name, value)                            \
    {                                                             \
        if (value) {                                              \
            fprintf(stderr, KGRN name ":\tENABLED   ☑\n" KRESET); \
        } else {                                                  \
            fprintf(stderr, KRED name ":\tDISABLED  ☐\n" KRESET); \
        }                                                         \
    }

#define PRINT_FLAG_VALUE(name, value) \
    { fprintf(stderr, KYEL name ":\t%d\n" KRESET, value); }

#define PRINT_FLAG_FLOAT_VALUE(name, value) \
    { fprintf(stderr, KYEL name ":\t%f\n" KRESET, value); }

#define asprintf_memory_error()                                            \
    {                                                                      \
        fprintf(stderr, "asprintf failed to allocate string. Exiting.\n"); \
        PURE_ABORT;                                                        \
        ;                                                                  \
    }

///// LOGGING FUNCTIONS /////

#ifdef ENABLE_THREAD_LOGGER

// assume ThreadLogger::initialize(...) has already been called
#define debug(M, ...)                                                        \
    {                                                                        \
        char* formatted;                                                     \
        if (asprintf(&formatted,                                             \
                     KBOLD KYEL "[DEBUG]" KNONBOLD   KGREY                   \
                                " %s:%s:%d: " KRESET M "\n",                 \
                     __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__) < 0) { \
            asprintf_memory_error();                                         \
        }                                                                    \
        ThreadLogger::log(formatted);                                        \
        free(formatted);                                                     \
    }

#define ts_debug(M, ...)                                                   \
    {                                                                      \
        std::lock_guard<std::mutex> stderr_lock(__zed_debug_stderr_mutex); \
        debug(M, ##__VA_ARGS__);                                           \
    }

#define log_err(M, ...)                                                      \
    {                                                                        \
        char* formatted;                                                     \
        if (asprintf(&formatted,                                             \
                     KBOLD KRED "[ERROR]" KDEFAULT " (%s:%s:%d) " KRESET M   \
                                "\n\n",                                      \
                     __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__) < 0) { \
            asprintf_memory_error();                                         \
        }                                                                    \
        ThreadLogger::log(formatted);                                        \
        free(formatted);                                                     \
    }
#define ts_log_err(M, ...)                                                 \
    {                                                                      \
        std::lock_guard<std::mutex> stderr_lock(__zed_debug_stderr_mutex); \
        log_err(M, ##__VA_ARGS__);                                         \
    }

#define log_warn(M, ...)                                                     \
    {                                                                        \
        char* formatted;                                                     \
        if (asprintf(&formatted,                                             \
                     KBOLD KPUR "[WARN]" KGREY " (%s:%s:%d) " KRESET M "\n", \
                     __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__) < 0) { \
            asprintf_memory_error();                                         \
        }                                                                    \
        ThreadLogger::log(formatted);                                        \
        free(formatted);                                                     \
    }
#define ts_log_warn(M, ...)                                                \
    {                                                                      \
        std::lock_guard<std::mutex> stderr_lock(__zed_debug_stderr_mutex); \
        log_warn(M, ##__VA_ARGS__);                                        \
    }

#define log_info(M, ...)                                                     \
    {                                                                        \
        char* formatted;                                                     \
        if (asprintf(&formatted, KGREY "[INFO] (%s:%s:%d) " KRESET M "\n",   \
                     __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__) < 0) { \
            asprintf_memory_error();                                         \
        }                                                                    \
        ThreadLogger::log(formatted);                                        \
        free(formatted);                                                     \
    }
#define ts_log_info(M, ...)                                                \
    {                                                                      \
        std::lock_guard<std::mutex> stderr_lock(__zed_debug_stderr_mutex); \
        log_info(M, ##__VA_ARGS__);                                        \
    }

#define log_direct(M, ...)                                \
    {                                                     \
        char* formatted;                                  \
        if (asprintf(&formatted, M, ##__VA_ARGS__) < 0) { \
            asprintf_memory_error();                      \
        }                                                 \
        bool print_header = false;                        \
        ThreadLogger::log(formatted, print_header);       \
        free(formatted);                                  \
    }
#define ts_log_direct(M, ...)                                              \
    {                                                                      \
        std::lock_guard<std::mutex> stderr_lock(__zed_debug_stderr_mutex); \
        log_direct(M, ##__VA_ARGS__);                                      \
    }

#define ts_print(M, ...)                                                   \
    {                                                                      \
        std::lock_guard<std::mutex> stderr_lock(__zed_debug_stderr_mutex); \
        char*                       formatted;                             \
        if (asprintf(&formatted, KRESET M "\n", ##__VA_ARGS__) < 0) {      \
            asprintf_memory_error();                                       \
        }                                                                  \
        ThreadLogger::log(formatted);                                      \
        free(formatted);                                                   \
    }

#else

#define debug(M, ...)
#define ts_debug(M, ...)
#define log_err(M, ...)
#define ts_log_err(M, ...)
#define log_warn(M, ...)
#define ts_log_warn(M, ...)
#define log_info(M, ...)
#define ts_log_info(M, ...)
#define ts_print(M, ...)
#define log_direct(M, ...)
#define ts_log_direct(M, ...)

#endif

///// OTHER DEBUGGING AND CHECKING FUNCTIONS /////

#define check_always(A, M, ...)                                              \
    {                                                                        \
        if (!(A)) {                                                          \
            fprintf(stderr,                                                  \
                    KBOLD KRED "\n[ERROR]" KDEFAULT " (%s:%s:%d)\n" KRESET M \
                               "\n\n",                                       \
                    __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__);        \
            log_err(M, ##__VA_ARGS__);                                       \
            PURE_ABORT;                                                      \
            ;                                                                \
        }                                                                    \
    }

#if DEBUG_CHECK

#define debug_print(M, ...) \
    { fprintf(stderr, KRED M KRESET, ##__VA_ARGS__); }

/* fprintf(stderr, KBOLD KRED "\n[ERROR]" KDEFAULT " (%s:%s:%d) " KRESET M
   "\n\n", __FILE__, __FUNCTION__, \
                   __LINE__, ##__VA_ARGS__);  */
#define clean_errno() (errno == 0 ? "None" : strerror(errno))
#define check_mem(A) check((A), "Out of memory.")
#define check(A, M, ...)                                                     \
    {                                                                        \
        if (!(A)) {                                                          \
            fprintf(stderr,                                                  \
                    KBOLD KRED "\n[ERROR]" KDEFAULT " (%s:%s:%d)\n" KRESET M \
                               "\n\n",                                       \
                    __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__);        \
            log_err(M, ##__VA_ARGS__);                                       \
            PURE_ABORT;                                                      \
            ;                                                                \
        }                                                                    \
    }

#define check_debug(A, M, ...)      \
    if (!(A)) {                     \
        ts_debug(M, ##__VA_ARGS__); \
        PURE_ABORT;                 \
    }

#define check_orig(A, M, ...)         \
    if (!(A)) {                       \
        ts_log_err(M, ##__VA_ARGS__); \
        errno = 0;                    \
        goto zed_debug_error;         \
    }

#else

// turn off for release mode
#define debug_print(M, ...)
#define clean_errno()
#define check_mem(A)
#define check(A, M, ...)
#define check_debug(A, M, ...)
#define check_orig(A, M, ...)

#endif

// #define PURE_ABORT MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
#if MPI_ENABLED
#define PURE_ABORT std::abort();
#else
#define PURE_ABORT std::abort();
#endif

#define sentinel(M, ...)                                                    \
    {                                                                       \
        char ___tname[16];                                                  \
        ::pthread_name(___tname);                                           \
        fprintf(stderr,                                                     \
                KBOLD                                        KRED           \
                "FATAL ERROR (SENTINEL on rank %s): " KRESET KNONBOLD KGREY \
                " (%s:%s:%d) " KRESET M "\n\n\n",                           \
                ___tname, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        PureRT::print_backtrace(stderr, 1, 1, '\n');                        \
        fflush(stdout);                                                     \
        PURE_ABORT;                                                         \
    }

// helpers to print variable contents
#define ts_log_print_bool(var) \
    ts_log_info("%s: %s", #var, ((var) ? "true" : "false"))
#define ts_log_print_int(var) ts_log_info("%s: %d", #var, var)
#define ts_log_print_uint(var) ts_log_info("%s: %u", #var, var)
#define ts_log_print_string(var) ts_log_info("%s: %s", #var, var)
#define ts_log_print_float(var) ts_log_info("%s: %f", #var, var)
#define ts_log_print_double(var) ts_log_info("%s: %lf", #var, var)
#define ts_log_print_ptr(var) ts_log_info("%s: %p", #var, var)

#define print_colored_stars(color_code)                                                                                                                                                                        \
    ts_print(                                                                                                                                                                                                  \
            color_code                                                                                                                                                                                         \
            "★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★" \
            "★" KRESET)
#define print_white_stars() print_colored_stars(KWHT)
#define print_green_stars() print_colored_stars(KGRN)
#define print_purple_stars() print_colored_stars(KPUR)
#define print_yellow_stars() print_colored_stars(KYEL)
#define print_blue_stars() print_colored_stars(KBLU)
#define print_red_stars() print_colored_stars(KRED)
#define print_cyan_stars() print_colored_stars(KCYN)

}; // namespace PureRT

// inluding this after sentinel is defined, as sentinel is used in
// helpers.h. not ideal.
#include "pure/support/helpers.h"

#endif

