// Author: James Psota
// File:   helpers.cpp 

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

#include "pure/support/helpers.h"

#include <errno.h>
#include <limits.h> /* PATH_MAX */
#include <string.h>
#include <sys/stat.h> /* mkdir(2) */

#include <cxxabi.h>
#include <execinfo.h>

#ifdef LINUX
#include <numa.h>
#endif

#if RUNTIME_IS_PURE
#include "pure.h"
#endif

namespace PureRT {

// Get current date/time, format is YYYY-MM-DD.HH:mm:ss
const std::string currentDateTime() {
    time_t    now = time(nullptr);
    struct tm tstruct;
    char      buf[80];

    struct tm tstruct_ret;
    tstruct = *localtime_r(&now, &tstruct_ret);
    // Visit http://en.cppreference.com/w/cpp/chrono/c/strftime
    // for more information about date/time format
    strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct_ret);

    return buf;
}

const std::string hostname_string() {
    char buf[80];
    auto ret = gethostname(buf, sizeof(buf));
    assert(ret == 0);

    return buf;
}

bool directory_exists(const char* pzPath) {
    if (pzPath == nullptr) {
        return false;
    }

    DIR* pDir;
    bool bExists = false;

    pDir = opendir(pzPath);

    if (pDir != nullptr) {
        bExists = true;
        (void)closedir(pDir);
    }

    return bExists;
}

int mkdir_p(const char* path) {
    /* Adapted from http://stackoverflow.com/a/2336245/119527 */
    const size_t len = strlen(path);
    char         _path[PATH_MAX];
    char*        p;

    errno = 0;

    /* Copy string so its mutable */
    if (len > sizeof(_path) - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(_path, path);

    /* Iterate the string */
    for (p = _path + 1; *p; p++) {
        if (*p == '/') {
            /* Temporarily truncate */
            *p = '\0';

            if (mkdir(_path, S_IRWXU) != 0) {
                if (errno != EEXIST)
                    return -1;
            }

            *p = '/';
        }
    }

    if (mkdir(_path, S_IRWXU) != 0) {
        if (errno != EEXIST)
            return -1;
    }

    return 0;
}

int get_cpu_on_linux() {
// nice overview of threads, affinity, and hyperthreading:
// https://eli.thegreenplace.net/2016/c11-threads-affinity-and-hyperthreading/
#ifdef LINUX
    return sched_getcpu();
#else
    // returns -1 if not available (e.g,. on OSX)
    return -1;
#endif
}

void set_cpu_affinity_on_linux(int cpu_id) {
    // bind thread to core
#ifdef LINUX
    // Create a cpu_set_t object representing a set of CPUs. Clear it
    // and mark only CPU i as set.
    // NUMACTL_CPUS may have more entries in it than num threads in this
    // process
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    const auto native_thread_handle = pthread_self();
    int ret = pthread_setaffinity_np(native_thread_handle, sizeof(cpu_set_t),
                                     &cpuset);
    if (ret != 0) {
        sentinel("Error calling pthread_setaffinity_np, after trying to pin "
                 "this thread to CPU %d: ret = %d.\n",
                 cpu_id, ret);
    }

#if DEBUG_CHECK
    int s = pthread_getaffinity_np(native_thread_handle, sizeof(cpu_set_t),
                                   &cpuset);
    assert(s == 0);
    for (auto j = 0; j < CPU_SETSIZE; j++) {
        if (CPU_ISSET(j, &cpuset)) {
            assert(j == cpu_id);
        }
    }
#endif
#endif
}

int numa_distance_of_cpus(int core_a_id, int core_b_id) {
#ifdef LINUX
    // numa_distance() reports the distance in the machine topology between
    // two nodes. The factors are a multiple of 10. It returns 0 when the
    // distance cannot be determined. A node has distance 10 to itself.
    // Reporting the distance requires a Linux kernel version of 2.6.10 or
    // newer. via https://linux.die.net/man/3/numa_distance
    const auto d = numa_distance(numa_node_of_cpu(core_a_id),
                                 numa_node_of_cpu(core_b_id));
    check(d > 0,
          "Expected numa_distance of cores %d and %d to be greater than "
          "zero "
          "but was %d",
          core_a_id, core_b_id, d);
    return d;
#else
    // hack just to get something working locally
    return core_b_id - core_a_id;
#endif
}

// WARNING: only currently supports linux machines with exactly 2
// hyperthreads per core
int thread_sibling_cpu_id(int my_core_id) {
#ifdef LINUX
    // given a core_id, returns the core_id of its hyperthread sibling

    char path[96];
    sprintf(path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
            my_core_id);
    // sprintf(path, "/Users/jim/temp/thread_siblings_list");

    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        fprintf(stderr, "Unable to open file %s", path);
        std::abort();
    }

    int c1 = -1, c2 = -1, c3 = -1;
    fscanf(f, "%d,%d,%d", &c1, &c2, &c3);
    fclose(f);

    assert(c3 == -1); // make sure there are only two hyperthreads (change
                      // this logic if there are more than two)
    assert(c1 != -1);
    assert(c2 != -1);
    assert(c1 != c2);

    if (c1 == my_core_id) {
        return c2;
    } else {
        assert(c2 == my_core_id);
        return c1;
    }
#else
    // fake value for OSX
    return 0;
#endif
}

std::string endpoint_type_name(PureRT::EndpointType et) {
    std::string name;
    switch (et) {
    case PureRT::EndpointType::SENDER:
        name = "Send";
        break;
    case PureRT::EndpointType::RECEIVER:
        name = "Receive";
        break;
    case PureRT::EndpointType::REDUCE:
        name = "Reduce";
        break;
    case PureRT::EndpointType::BCAST:
        name = "Bcast";
        break;
    default:
        // TODO(jim): sentinel("Invalid type %d", as_integer(et));
        exit(-5455);
    } // switch

    return name;
} // function endpoint_type_name

void assert_env_var_string(const char* env_var_name, const char* expected_str) {
    const char* value_str = std::getenv(env_var_name);
    if (value_str == nullptr) {
        std::cerr << "ERROR: Expected value " << expected_str
                  << " for environment variable " << env_var_name
                  << " but it is null." << std::endl;
    }

    // http://www.cplusplus.com/reference/string/string/compare/
    if (std::string(value_str) != std::string(expected_str)) {
        std::cerr << "ERROR: Expected value " << expected_str
                  << " for environment variable " << env_var_name
                  << " but it is " << value_str << "." << std::endl;
        std::abort();
    }
}

void print_pure_backtrace(FILE* out, char between_syms) {
// may need different values if profile mode. we try to get this between
// orig_main and the calling function
#if RUNTIME_IS_PURE
    fprintf(out, "BACKTRACE FOR PURE RANK %.4d\n", PURE_RANK);
#endif
    print_backtrace(out, 2, 5, between_syms);
    fprintf(out, "\n");
}

void print_backtrace(FILE* out, int skip_top_levels, int skip_bottom_levels,
                     char between_syms) {
#if LINUX
    print_backtrace_linux(out, skip_top_levels, skip_bottom_levels,
                          between_syms);
#endif
#if OSX
    print_backtrace_osx(out, skip_top_levels, skip_bottom_levels, between_syms);
#endif
    fflush(out);
}

void print_backtrace_linux(FILE* out, int skip_top_levels,
                           int skip_bottom_levels, char between_syms) {
    const int max_depth = 128;
    void*     callstack[max_depth];
    int       frames = backtrace(callstack, max_depth);
    char**    strs   = backtrace_symbols(callstack, frames);

    const std::string delimiter     = "(";
    const int         desired_token = 1; // empirically, at least on OSX

    bool printed_first_sym = false;
    for (auto i = skip_top_levels; i < frames - skip_bottom_levels; ++i) {
        char* sym = strs[i];
        // fprintf(stderr, KYEL "FULL STRING: %s\n" KRESET, sym);

        std::string s(sym);

        // we perform two finds, one for the ( and one for the )
        const auto pos1 = s.find("(");
        const auto pos2 = s.find("+0x");

        if (pos2 == std::string::npos) {
            // empty symbol -- no +0x
            continue;
        }

        std::string token = s.substr(pos1 + 1, (pos2 - pos1 - 1));
        // fprintf(stderr, KGRN "        found token %s\n" KRESET,
        // token.c_str());

        int   status;
        char* demangled =
                abi::__cxa_demangle(token.c_str(), nullptr, nullptr, &status);
        // we allow C strings to not be demangled (e.g., _pthread_body)
        if (status == -1 || status == -3) {
            sentinel("FAILED demangled with string: %s (status was "
                     "%d)\n",
                     token.c_str(), status);
        }

        if (printed_first_sym) {
            fprintf(out, "%c", between_syms);
        }
        if (status == -2) {
            // this SHOULD be a C string, so demangle failed
            // print the unmangled string
            fprintf(out, "%s", token.c_str());
        } else {
            fprintf(out, "%s", demangled);
        }
        printed_first_sym = true;
        std::free(demangled);
    }

    free(strs);
}
// skip levels seems to work fairly well, but some things may break it:
// inlining
// (?) and also there seem to be some empty layers at the bottom of the
// stack that don't get cleanly erased.

// DOCS:
// https://man7.org/linux/man-pages/man3/backtrace.3.html
// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/backtrace.3.html

// Linux string looks like this:
// /global/.../libpure.so(_ZSt13__invoke_implIvM11PureProcessFvP10PureThreadSt8optionalIiEEPS0_JS2_iEET_St21__invoke_memfun_derefOT0_OT1_DpOT2_+0xcf)
// [0x2aaaab82298f] OSX string has spaces between each field

void print_backtrace_osx(FILE* out, int skip_top_levels, int skip_bottom_levels,
                         char between_syms) {

    // OSX only at first
    const int max_depth = 128;
    void*     callstack[max_depth];
    int       frames = backtrace(callstack, max_depth);
    char**    strs   = backtrace_symbols(callstack, frames);

    const std::string delimiter     = " ";
    const int         desired_token = 3; // empirically, at least on OSX

    for (auto i = skip_top_levels; i < frames - skip_bottom_levels; ++i) {
        char* sym = strs[i];

        // extract out the symbol so we can demangle it
        std::string s(sym);
        size_t      pos = 0;
        std::string token;
        int         token_i = 0;
        // offset is the next token if that's ever helpful

        // find the actual symbol name
        while ((pos = s.find(delimiter)) != std::string::npos) {
            token = s.substr(0, pos);

            if (token_i == desired_token && token.length() > 0) {
                int   status;
                char* demangled = abi::__cxa_demangle(token.c_str(), nullptr,
                                                      nullptr, &status);
                // we allow C strings to not be demangled (e.g.,
                // _pthread_body)
                if (status == -1 || status == -3) {
                    sentinel("FAILED demangled with string: %s (status was "
                             "%d)\n",
                             token.c_str(), status);
                }

                if (i > skip_top_levels) {
                    fprintf(out, "%c", between_syms);
                }
                if (status == -2) {
                    // this SHOULD be a C string, so demangle failed
                    // print the unmangled string
                    fprintf(out, "%s", token.c_str());
                } else {
                    fprintf(out, "%s", demangled);
                }
                std::free(demangled);
            }

            if (token.length() > 0 &&
                token.compare(delimiter) != 0 /* token is not a space */) {
                ++token_i;
            }

            s.erase(0, pos + delimiter.length());
        } // while
    }
    free(strs);
}

void print_stack_trace(FILE* out, unsigned int max_frames) {

    fprintf(out, KBOLD KLT_PUR "\n ■ ■ ■ ■ ■ ■ ■ ■ ■ ■ ■ ■ ■ ■ ■  S T A C K   "
                               "T R A C E  ■ ■ ■ ■ ■ "
                               "■ ■ ■ ■ ■ ■ ■ ■ ■ ■\n" KRESET);

    // storage array for stack trace address data
    void* addrlist[max_frames + 1];

    // retrieve current stack addresses
    unsigned int addrlen =
            backtrace(addrlist, sizeof(addrlist) / sizeof(void*));

    if (addrlen == 0) {
        fprintf(out, "  \n");
        return;
    }

    // resolve addresses into strings containing
    // "filename(function+address)", Actually it will be ## program address
    // function + offset this array must be free()-ed
    char** symbollist = backtrace_symbols(addrlist, addrlen);

    size_t funcnamesize = 1024;
    char   funcname[1024];

    // iterate over the returned symbol lines. skip the first, it is the
    // address of this function.
    for (unsigned int i = 1; i < addrlen; i++) {
        char* begin_name   = nullptr;
        char* begin_offset = nullptr;

// find parentheses and +address offset surrounding the mangled name
#ifdef OSX
        // OSX style stack trace
        for (char* p = symbollist[i]; *p != 0; ++p) {

            /* note: clang-tidy finds issue with this:
              /Users/jim/Documents/Research/projects/hybrid_programming/pure/include/pure/support/helpers.h:135:33:
            warning: Out of bound memory access (access exceeds upper limit
            of memory block) [clang-analyzer-alpha.security.ArrayBoundV2] if
            ((*p == '_') && (*(p - 1) == ' ')) {
            */
            if ((*p == '_') && (*(p - 1) == ' ')) {
                begin_name = p - 1;
            } else if (*p == '+') {
                begin_offset = p - 1;
            }
        }

        if ((begin_name != nullptr) && (begin_offset != nullptr) &&
            (begin_name < begin_offset)) {
            *begin_name++   = '\0';
            *begin_offset++ = '\0';

            // mangled name is now in [begin_name, begin_offset) and caller
            // offset in [begin_offset, end_offset). now apply
            // __cxa_demangle():
            int   status;
            char* ret = abi::__cxa_demangle(begin_name, &funcname[0],
                                            &funcnamesize, &status);
            if (status == 0) {
                strcpy(funcname, ret);
                //                funcname = ret; // use possibly
                //                realloc()-ed string
                fprintf(out, "  %-30s %-40s %s\n", symbollist[i], funcname,
                        begin_offset);
            } else {
                // demangling failed. Output function name as a C function
                // with no arguments.
                fprintf(out, "  %-30s %-38s() %s\n", symbollist[i], begin_name,
                        begin_offset);
            }

#else  // !DARWIN - but is posix
       // not OSX style
       // ./module(function+0x15c) [0x8048a6d]

        char* end_offset = NULL;
        for (char* p = symbollist[i]; *p; ++p) {
            if (*p == '(')
                begin_name = p;
            else if (*p == '+')
                begin_offset = p;
            else if (*p == ')' && (begin_offset || begin_name))
                end_offset = p;
        }

        if (begin_name && end_offset && (begin_name < end_offset)) {
            *begin_name++ = '\0';
            *end_offset++ = '\0';
            if (begin_offset)
                *begin_offset++ = '\0';

            // mangled name is now in [begin_name, begin_offset) and caller
            // offset in [begin_offset, end_offset). now apply
            // __cxa_demangle():

            int   status = 0;
            char* ret = abi::__cxa_demangle(begin_name, funcname, &funcnamesize,
                                            &status);
            char* fname = begin_name;
            if (status == 0)
                fname = ret;

            if (begin_offset) {
                fprintf(out, "  %-30s ( %-40s  + %-6s) %s\n", symbollist[i],
                        fname, begin_offset, end_offset);
            } else {
                fprintf(out, "  %-30s ( %-40s    %-6s) %s\n", symbollist[i],
                        fname, "", end_offset);
            }
#endif // !DARWIN - but is posix
        } else {
            // couldn't parse the line? print the whole line.
            fprintf(out, "  %-40s\n", symbollist[i]);
        }
    }

    fflush(out);
    free(symbollist);
}
// Time / Clock helpers

int utc_time_monotonic(struct timespec* ts) {
    int ret;

#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
    clock_serv_t    cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    ret = (int)clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    ts->tv_sec  = mts.tv_sec;
    ts->tv_nsec = mts.tv_nsec;
#else
    // Linux
    // RDTSC --
    ret = clock_gettime(CLOCK_MONOTONIC, ts);
#endif
    return ret;
}

int utc_time_process_cpu(struct timespec* ts) {
    int ret;

#ifdef __MACH__
    // process time unsupported on OSX
    ts->tv_sec  = 0;
    ts->tv_nsec = 0;
    ret         = 0; // not success, but unsupported for OSX
#else
    // Linux
    ret = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, ts);
#endif
    return ret;
}

int utc_time_thread_cpu(struct timespec* ts) {
    int ret;

#ifdef __MACH__
    // thread time unsupported on OSX
    ts->tv_sec  = 0;
    ts->tv_nsec = 0;
    ret         = 0; // not success, but unsupported for OSX
#else
    // Linux
    ret = clock_gettime(CLOCK_THREAD_CPUTIME_ID, ts);
#endif
    return ret;
}

bool timespecEqualTo(const struct timespec& a, const struct timespec& b) {
    return (a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec);
}

// returns true is start is later in time than end
bool timespecGreaterThan(const struct timespec& a, const struct timespec& b) {

    if (a.tv_sec > b.tv_sec) {
        return true;
    } else if (a.tv_sec == b.tv_sec) {
        return (a.tv_nsec > b.tv_nsec);
    } else {
        return false;
    }
}

// via
// https://stackoverflow.com/questions/1640258/need-a-fast-random-generator-for-c
thread_local unsigned long xorshf_x = 123456789, xorshf_y = 362436069,
                           xorshf_z = 521288629;
unsigned long xor_shift_96() { // period 2^96-1
    xorshf_x ^= xorshf_x << 16;
    xorshf_x ^= xorshf_x >> 5;
    xorshf_x ^= xorshf_x << 1;

    auto      t = xorshf_x;
    xorshf_x    = xorshf_y;
    xorshf_y    = xorshf_z;
    xorshf_z    = t ^ xorshf_x ^ xorshf_y;

    return xorshf_z;
}

} // namespace PureRT
