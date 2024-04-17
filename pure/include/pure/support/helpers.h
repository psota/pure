// Author: James Psota
// File:   helpers.h 

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


// note: these helpers only owrk for C++ files other than a few functions
// defined below

#ifndef PURE_SUPPORT_HELPERS_H
#define PURE_SUPPORT_HELPERS_H

#pragma once

#include "pure/support/zed_debug.h"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cxxabi.h>
#include <dirent.h>
#include <execinfo.h>
#include <iomanip>
#include <iostream>
#include <pthread.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <typeinfo>
#include <unistd.h>

#ifdef LINUX
//#include <bsd/stdlib.h>
#include <sched.h>
#endif

#include <sys/time.h>
#include <time.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "pure/common/pure_rt_enums.h"
#include "pure/support/colors.h"

// this shouldn't be done here -- it should be done in files that include this
// using namespace PureRT;

template <typename Enumeration>
static auto as_integer(Enumeration const value) ->
        typename std::underlying_type<Enumeration>::type {
    return static_cast<typename std::underlying_type<Enumeration>::type>(value);
}

static inline void sleep_random_seconds(const int seconds) {
    usleep(rand() % (seconds * 1000));
}

//// 2-d Matrix Helpers
////////////////////////////////////////////////////////////
template <typename T>
size_t bytes_in_2d_matrix(size_t rows, size_t cols) {
    return rows * cols * sizeof(T);
}

// gives index into 1-d array that corresponds to the 2d matrix[i][j]
// important: rows_in_matrix must be const or constexpr
template <int rows_in_matrix>
constexpr int two_d_idx(int i, int j) {
    return (j * rows_in_matrix) + i;
}

/////////////////////////////////////////////////////////////////////////////////

namespace PureRT {
// defined in src/support/helpers.cpp
const std::string currentDateTime();
const std::string hostname_string();
bool              directory_exists(const char* /*pzPath*/);
int               mkdir_p(const char* /* path */);
std::string       endpoint_type_name(PureRT::EndpointType /*et*/);
int               get_cpu_on_linux();
void              set_cpu_affinity_on_linux(int cpu_id);
int               numa_distance_of_cpus(int core_a_id, int core_b_id);
int               thread_sibling_cpu_id(int my_core_id);

void print_pure_backtrace(FILE* out = stderr, char between_syms = '\t');
void print_backtrace(FILE* out, int skip_top_levels = 1,
                     int skip_bottom_levels = 1, char between_syms = '\t');

void print_backtrace_linux(FILE* out, int skip_top_levels,
                           int skip_bottom_levels, char between_syms);

void print_backtrace_osx(FILE* out, int skip_top_levels, int skip_bottom_levels,
                         char between_syms);

void print_stack_trace(FILE* out = stderr, unsigned int max_frames = 63);

template <typename ValType>
void assert_env_var_value(const char* env_var_name, ValType expected_val) {
    const char* value_str = std::getenv(env_var_name);

    if (value_str == nullptr) {
        std::cerr << "ERROR: Expected value " << expected_val
                  << " for environment variable " << env_var_name
                  << " but it is null." << std::endl;
    }

    ValType actual;
    if (std::is_integral<ValType>::value) {
        actual = std::stoi(value_str);
    } else if (std::is_floating_point<ValType>::value) {
        actual = std::stof(value_str);
    } else {
        sentinel("Invalid template parameter type; only integer and floating "
                 "point supported.")
    }

    //    atoi(value_str);
    if (actual != expected_val) {
        std::cerr << "ERROR: Expected value " << expected_val
                  << " for environment variable " << env_var_name
                  << " but it is " << actual << "." << std::endl;
        std::abort();
    }
}

inline void assert_nprocs(int expected_nprocs) {
    assert_env_var_value<int>("NPROCS", expected_nprocs);
}
inline void assert_num_procs(int expected_pure_nprocs) {
    assert_env_var_value<int>("PURE_NUM_PROCS", expected_pure_nprocs);
}
inline void assert_num_threads(int expected_nthreads) {
    assert_env_var_value<int>("PURE_RT_NUM_THREADS", expected_nthreads);
}

// we make this robust against both type and const-ness. We just care about the
// actual pointer value.
#define assert_cacheline_aligned(ptr) \
    __assert_cacheline_aligned(       \
            const_cast<const void*>(reinterpret_cast<const void*>(ptr)))

template <int alignment_bytes>
inline void assert_aligned(void const* const ptr) {
    check((reinterpret_cast<uintptr_t>(ptr) % alignment_bytes) == 0,
          "Expected ptr %p to be aligned to %d bytes, but the modulus is %lu",
          ptr, alignment_bytes,
          reinterpret_cast<uintptr_t>(ptr) % alignment_bytes);
}

inline void __assert_cacheline_aligned(void const* const ptr) {
    assert_aligned<CACHE_LINE_BYTES>(ptr);
}

inline void assert_false_sharing_range_aligned(void const* const ptr) {
    assert_aligned<FALSE_SHARING_RANGE>(ptr);
}

int utc_time_monotonic(struct timespec* ts);
int utc_time_process_cpu(struct timespec* ts);
int utc_time_thread_cpu(struct timespec* ts);

bool timespecEqualTo(const struct timespec& /* a */,
                     const struct timespec& /* b */);
bool timespecGreaterThan(const struct timespec& /* a */,
                         const struct timespec& /* b */);
void assert_env_var_string(const char* /* env_var_name */,
                           const char* /* expected_str */);

unsigned long xor_shift_96();

#ifndef AMPI_MODE_ENABLED
// work around old AMPI C standard
constexpr int round_up(int num_to_round, int multiple) {
#else
inline int round_up(int num_to_round, int multiple) {
#endif
    if (multiple == 0) {
        return num_to_round;
    }

    int remainder = num_to_round % multiple;
    if (remainder == 0) {
        return num_to_round;
    }

    return num_to_round + multiple - remainder;
}

} // namespace PureRT

enum class PayloadValueMode { POSITIONAL, SMALL, TINY };

template <typename PayloadType>
inline PayloadType
payload_value(int iteration, int sender_rank, int position,
              PayloadValueMode mode = PayloadValueMode::POSITIONAL) noexcept {

    PayloadType ret_val;
    if (std::is_floating_point<PayloadType>::value) {
        ret_val = (((iteration) + 1) * 1.00000001 + (sender_rank)*1.001001 +
                   (position)*1.000001);
    } else if (std::is_integral<PayloadType>::value) {
        if (mode == PayloadValueMode::POSITIONAL) {
            // SCHEME WILL ENCODE USING the following:
            // <iteration+1><00000><sender_rank><00><position i>
            ret_val = (((iteration) + 1) * 100000 + (sender_rank)*100 +
                       (position)*1);
        } else if (mode == PayloadValueMode::SMALL) {
            ret_val = (((iteration) + 1) * 8 + (sender_rank)*4 + (position)*1);
        } else if (mode == PayloadValueMode::TINY) {
            ret_val =
                    (((iteration) + 1) * 1.3 + (sender_rank)*1 + (position)*1);
        } else {
            sentinel("Unsupported PayloadValueMode %d", mode);
        }
    } else {
        sentinel("Unsupported PayloadType. Unable to generated payload_value "
                 "for types other than "
                 "floating point and integral values");
    }

    return ret_val;
}

template <typename ArrayType>
std::string debug_array_to_string(ArrayType* arr, size_t num_elts,
                                  const std::string& array_name) {

    std::stringstream ss;
    ss.precision(12);

    for (size_t i = 0; i < num_elts; ++i) {
        ss << array_name << "[" << i << "] =\t" << arr[i] << '\n';
    }

    return ss.str();

} // function debug_print_array

void inline circular_increment(unsigned int num_entries, unsigned int& index) {
    if (index == (num_entries - 1)) {
        index = 0;
    } else {
        ++index;
    }
}

template <typename T>
float percentage(T v1, T v2) {
    assert(static_cast<float>(v2) != 0.0f);
    return 100 * static_cast<float>(v1) / static_cast<float>(v2);
}

// from
// http://stackoverflow.com/questions/12877521/human-readable-type-info-name
// note: returns a string that the caller has to free
std::string inline demangle(const char* mangled) {
    int                                      status;
    std::unique_ptr<char[], void (*)(void*)> result(
            abi::__cxa_demangle(mangled, 0, 0, &status), std::free);
    return result.get() ? std::string(result.get())
                        : "An error occurred in trying to demangle token";
}

void inline assert_mpi_running() {
#ifndef MPI_ENABLED
    sentinel("ERROR: MPI is not running, but it's expected to be running.");
#endif
}

void inline x86_pause_instruction() noexcept {
#ifndef OSX
    __asm__ __volatile__("pause");
#endif
}

/*
 char tname[16];
 pthread_name(tname);
 */
inline void pthread_name(char* out_name) {
    int ret = pthread_getname_np(pthread_self(), out_name, 16);
    assert(ret == 0);
}

inline void pthread_set_name(char* name) {
#if LINUX
    const auto native_thread_handle = pthread_self();
    auto const ret = pthread_setname_np(native_thread_handle, name);
#elif OSX
    auto const ret = pthread_setname_np(name);
#endif
    assert(ret == 0);
}

inline void set_pure_thread_name(unsigned int pure_rank, unsigned int mpi_rank,
                                 unsigned int thread_num_in_process) {
    std::stringstream thread_name_ss;
    // format: <pure rank><mpi_rank><thread num>: p0005m0002t0003
    const size_t field_width = 4;
    thread_name_ss << "p" << std::setfill('0') << std::setw(field_width)
                   << pure_rank;
    thread_name_ss << "m" << std::setfill('0') << std::setw(field_width)
                   << mpi_rank;
    thread_name_ss << "t" << std::setfill('0') << std::setw(field_width)
                   << thread_num_in_process;
    assert(thread_name_ss.str().size() <= 15); // pthread_setname_np restriction

    const std::string& tmp  = thread_name_ss.str();
    const char*        name = tmp.c_str();

    int ret;
#ifdef LINUX
    // more details:
    // http://stackoverflow.com/questions/2369738/can-i-set-the-name-of-a-thread-in-pthreads-linux/7989973#7989973
    const auto native_thread_handle = pthread_self();
    ret = pthread_setname_np(native_thread_handle, name);
    assert(ret == 0);
#elif OSX
    ret            = pthread_setname_np(name);
    assert(ret == 0);
#endif
}

//  process- and thread-level timings are implemented using registers and are
//  not globally consistent. therefore, end can be less than start if process
//  migration is allowed. therefore, we only assert monotonicity with
//  CLOCK_MONOTONIC
template <bool assert_monotonicity>
uint64_t timespecDiff(const struct timespec& end,
                      const struct timespec& start) {
    if (end.tv_sec == start.tv_sec) {
        if (assert_monotonicity) {
            assert(end.tv_nsec >= start.tv_nsec);
        }
        return end.tv_nsec - start.tv_nsec;
    }

    if (assert_monotonicity) {
        assert(end.tv_sec > start.tv_sec);
    }
    auto sec_diff = uint64_t(end.tv_sec - start.tv_sec);
    assert(sec_diff < std::numeric_limits<uint64_t>::max() / 1000000000UL);
    return sec_diff * 1000000000UL + end.tv_nsec - start.tv_nsec;
}

#if USE_ASMLIB
#include "pure/3rd_party/asmlib/asmlib.h"
#define MEMCPY_IMPL A_memcpy
#else
#define MEMCPY_IMPL std::memcpy
#endif

// special memcpy implementations
#ifndef USE_ASMLIB_MEMSET
#define USE_ASMLIB_MEMSET 0 // testing on Cori
#endif

#if USE_ASMLIB_MEMSET
#include "pure/3rd_party/asmlib/asmlib.h"
#define MEMSET_IMPL A_memset
#else
#define MEMSET_IMPL std::memset
#endif

// returns the pointer to the allocated memory (like malloc)
template <int do_alignment = 0, size_t alignment_bytes = CACHE_LINE_BYTES,
          bool cache_line_roundup_size = false, typename return_type = void>
return_type* jp_memory_alloc(size_t size_bytes,
                             int*   allocated_size_bytes = nullptr) {

    if (size_bytes <= 0) {
        PureRT::print_stack_trace();
    }

    assert(size_bytes > 0);
    size_bytes = cache_line_roundup_size
                         ? PureRT::round_up(size_bytes, CACHE_LINE_BYTES)
                         : size_bytes;
    assert(size_bytes > 0);

    if (allocated_size_bytes != nullptr) {
        // we may round up size_bytes so we return the value we actually
        // allocated
        *allocated_size_bytes = size_bytes;
    }

    void* ptr;
    if (do_alignment) {
        int ret = posix_memalign(&ptr, alignment_bytes, size_bytes);
        assert(ret == 0);
    } else {
        ptr = std::malloc(size_bytes);
    }
    assert(ptr != nullptr);

    // we return size_bytes because it may be different than what was asked for
    // if rounding up to the cache line size
    return static_cast<return_type*>(ptr);
}

// returns the number of bytes allocated
#include <inttypes.h>
// DEPRECATED -- use the one above
#define JP_MEM_ALLOC_TIMING 0
template <int do_alignment = 0, size_t alignment_bytes = CACHE_LINE_BYTES,
          bool cache_line_roundup_size = false>
int jp_memory_alloc(void** ptr_addr, size_t size_bytes) {
    size_bytes = cache_line_roundup_size
                         ? PureRT::round_up(size_bytes, CACHE_LINE_BYTES)
                         : size_bytes;

#if JP_MEM_ALLOC_TIMING
    struct timespec start;
    PureRT::utc_time_monotonic(&start);
#endif
    if (do_alignment) {
        int ret = posix_memalign(ptr_addr, alignment_bytes, size_bytes);
        assert(ret == 0);
    } else {
        *ptr_addr = std::malloc(size_bytes);
    }
    assert(*ptr_addr != nullptr);

#if JP_MEM_ALLOC_TIMING
    struct timespec end;
    PureRT::utc_time_monotonic(&end);
    uint64_t diff = timespecDiff<true>(end, start);
    printf(KRED "jp_mem_alloc: %zu bytess; %" PRIu64 " ns\n" KRESET, size_bytes,
           diff);
#endif

    // we return size_bytes because it may be different than what was asked for
    // if rounding up to the cache line size
    return size_bytes;
}

/* compute the next highest power of 2 of 32-bit v via
https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2

In 12 operations, this code computes the next highest power of 2 for a 32-bit
integer. The result may be expressed by the formula 1U << (lg(v - 1) + 1). Note
that in the edge case where v is 0, it returns 0, which isn't a power of 2; you
might append the expression v += (v == 0) to remedy this if it matters. It would
be faster by 2 operations to use the formula and the log base 2 method that uses
a lookup table, but in some situations, lookup tables are not suitable, so the
above code may be best. (On a Athlon™ XP 2100+ I've found the above shift-left
and then OR code is as fast as using a single BSR assembly language instruction,
which scans in reverse to find the highest set bit.) It works by copying the
highest set bit to all of the lower bits, and then adding one, which results in
carries that set all of the lower bits to 0 and one bit beyond the highest set
bit to 1. If the original number was a power of 2, then the decrement will
reduce it to one less, so that we round up to the same original value. You might
alternatively compute the next higher power of 2 in only 8 or 9 operations using
a lookup table for floor(lg(v)) and then evaluating 1<<(1+floor(lg(v))); Atul
Divekar suggested I mention this on September 5, 2010.
*/
inline unsigned int next_pow_2(unsigned int v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

// via http://burtleburtle.net/bob/hash/integer.html
inline uint32_t int_hash(uint32_t a) {
    a = (a ^ 61) ^ (a >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2d;
    a = a ^ (a >> 15);
    return a;
}

#endif
