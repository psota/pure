# Pure

Pure is a parallel programming model and runtime system. Pure enables programmers to improve performance of parallel applications on multicore clusters with minimal additional programming effort.


##  Contents
1. [System Overview](#user-content-overview)
2. [Example Application Pseudocode](#user-content-example)
3. [Directory Contents](#directories)
4. [Installation](#user-content-installation)
5. [Writing and Compiling Pure Applications](#user-content-applications)
6. [Academic Papers](#user-content-papers)



## Overview <a name="overview"></a>

Pure is a parallel programming model and runtime system explicitly designed to take advantage of shared memory within nodes in the context of a mostly message passing interface enhanced with the ability to use tasks to make use of idle cores. Pure leverages shared memory in two ways: (a) by allowing cores to steal work from each other while waiting on messages to arrive, and, (b) by leveraging efficient lock-free data structures in shared memory to achieve high-performance messaging and collective operations between the ranks within nodes.

In our [PPoPP'24 paper](https://dl.acm.org/doi/abs/10.1145/3627535.3638503), we showed significant speedups from Pure, including speedups up to 2.1× on the CoMD molecular dynamics and the miniAMR adaptive mesh refinement applications scaling up to 4,096 cores. Further microbenchmarks in the paper show speedups over MPI from 2× to 17× on communication and collective operations running on 2 - 65,536 cores.


## Example Application Pseudocode  <a name="example"></a>

In this section we show a simple MPI program that implements a simple 1-D Jacobi-like stencil. This program is meant to illustrate the key features of Pure: messaging and optional task execution. See more detail in [the Pure paper](https://dl.acm.org/doi/abs/10.1145/3627535.3638503). Note that this code is slightly cleaned up for readability; see `tests/jacobi_with_tasks` for the runnable versions.

### MPI Psuedocode

```cpp 
#include "mpi.h"
void rand_stencil_mpi(double* const a, size_t arr_sz, size_t iters, int my_rank,
                      int n_ranks) {
    double temp[arr_sz];
    for (auto it = 0; it < iters; ++it) {
        for (auto i = 0; i < arr_sz; ++i) {
            temp[i] = random_work(a[i]);
        }
        for (auto i = 1; i < arr_sz - 1; ++i) {
            a[i] = (temp[i - 1] + temp[i] + temp[i + 1]) / 3.0;
        }
        if (my_rank > 0) {
            MPI_Send(&temp[0], 1, MPI_DOUBLE, my_rank - 1, 0, MPI_COMM_WORLD);
            double neighbor_hi_val;
            MPI_Recv(&neighbor_hi_val, 1, MPI_DOUBLE, my_rank - 1, 0,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            a[0] = (neighbor_hi_val + temp[0] + temp[1]) / 3.0;
        } // ends if not first rank
        if (my_rank < n_ranks - 1) {
            MPI_Send(&temp[arr_sz - 1], 1, MPI_DOUBLE, my_rank + 1, 0,
                     MPI_COMM_WORLD);
            double neighbor_lo_val;
            MPI_Recv(&neighbor_lo_val, 1, MPI_DOUBLE, my_rank + 1, 0,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            a[arr_sz - 1] =
                    (temp[arr_sz - 2] + temp[arr_sz - 1] + neighbor_lo_val) /
                    3.0;
        } // ends if not last rank
    }     // ends for all iterations
}
```
*N.B. See `tests/jacobi_with_tasks/baseline` for the runnable version of this code.*

### Pure Psuedocode
```cpp 
#include "pure.h"
void rand_stencil_pure(double* const a, size_t arr_sz, size_t iters,
                       int my_rank, int n_ranks) {
    double   temp[arr_sz];
    PureTask rand_work_task = [a, temp, arr_sz,
                               my_rank](chunk_id_t           start_chunk,
                                        chunk_id_t           end_chunk,
                                        std::optional<void*> cont_params) {
        auto [min_idx, max_idx] =
                pure_aligned_idx_range<double>(arr_sz, start_chunk, end_chunk);
        for (auto i = min_idx; i <= max_idx; ++i) {
            temp[i] = random_work(a[i]);
        }
    }; // ends defining the Pure Task rand_work_task
    for (auto it = 0; it < iters; ++it) {
        rand_work_task.execute(); // execute all chunks of rand_work_task
        for (auto i = 1; i < arr_sz - 1; ++i) {
            a[i] = (temp[i - 1] + temp[i] + temp[i + 1]) / 3.0;
        }
        if (my_rank > 0) {
            pure_send_msg(&temp[0], 1, PURE_DOUBLE, my_rank - 1, 0,
                          PURE_COMM_WORLD);
            double neighbor_hi_val;
            pure_recv_msg(&neighbor_hi_val, 1, PURE_DOUBLE, my_rank - 1, 0,
                          PURE_COMM_WORLD);
            a[0] = (neighbor_hi_val + temp[0] + temp[1]) / 3.0;
        } // ends if not first rank
        if (my_rank < n_ranks - 1) {
            pure_send_msg(&temp[arr_sz - 1], 1, PURE_DOUBLE, my_rank + 1, 0,
                          PURE_COMM_WORLD);
            double neighbor_lo_val;
            pure_recv_msg(&neighbor_lo_val, 1, PURE_DOUBLE, my_rank + 1, 0,
                          PURE_COMM_WORLD);
            a[arr_sz - 1] =
                    (temp[arr_sz - 2] + temp[arr_sz - 1] + neighbor_lo_val) /
                    3.0;
        } // ends if not last rank
    }     // ends for all iterations
}
```
*N.B. See `tests/jacobi_with_tasks/pure` for the runnable version of this code.*





## Directory Contents <a name="directories"></a>

This repository is organized into the following structure:

  * `src`: Source code for the Pure runtime system
    * `runtime`: Key runtime system source; key files include`PureProcess`, `PureThread`, and `PurePipeline`.
    * `transport`: Key messaging and collective implementations
    * `support`: Helpers, debugging, and benchmarking infrastructure
    * `Makefile`: Build infrastructure for libpure
  * `include`: Header files for the Pure runtime system and applications
  * `test`: Several complete Pure applications and their baseline (MPI) analogs
  * `support`: Miscellaneous tools for building, analyzing, and debugging Pure applications
     * `runtime`: Pure runtime tools, including `purify_all.rb` which is an MPI-to-Pure source-level translator
     * `Makefile_includes`: Defines most of the Pure build system; see variables for relevant configuration options.
     * `misc`: Various Pure tools (perf-based profiling tools, clang-based sanitizer tools, debugging and profiling visualization tools, etc.)
     * `experiments`: Pure experiment infrastructure for running many (hundreds, thousands) of jobs via SLURM in parallel and combining and analyizing the results. Includes an optional web-based results reporting system.
        * `benchmark_helpers.rb`: Defines a DSL for Pure experiments and is the main driver of the Pure experiment framework
        * `machine_helpers.rb`: Tooling for configuring Pure to specific machines and system software
        * `combiner_helpers.rb`: Tools to combine results from independently-run experiments
     * `R_helpers/`: Data analysis helpers for the Pure experiment data collection tools
  * `lib`: Auto-generated directory where libpure is stored
  * `build`: Auto-generated directory where object files are stored




## Installation <a name="installation"></a>

### System Requirements

Pure is mostly implemented as a native C++17 library that is compatible with most systems. Pure does require a small number of dependencies, which must be installed on your system before Pure can be compiled and installed.

### Dependencies

The foundational elements of Pure leverage shared memory to achieve improved performance relative to MPI; all of these foundational elements are written in native C++17. Pure does leverage a few external libraries, mostly for cross-node message-passing communication (MPI) and libraries used for non-performance-critical logging and debugging infrastructure (libjson and Boost).

Before proceeding, please ensure that the following are installed on your system:

* Any C++17-compatible compiler. Pure has been tested using gcc, clang, and Intel C compiler on Linux and OSX.
* MPI (2.0 or greater). Pure uses MPI by default for communication between nodes. The use of MPI, however, is transparent to the Pure application programmer. Pure has been tested with MPICH and Intel MPI, although any compatible implementation should work.
* [Ruby](https://www.ruby-lang.org/en/community/ruby-core/) is used as the scripting language for many Pure tools. Many Ruby Gems must be installed (you will be prompted to install them as you run various tools).
* [R](https://www.r-project.org/) is used to analyze experimental data and generate performance results *(optional)*
* [Boost](https://www.boost.org/users/download/) `boost::hash` is used in non-performance-critical code to help manage memory deallocations; requires version 1.63+.
* [jsoncpp](https://github.com/open-source-parsers/jsoncpp) to generate rank-level stats files
* [jemalloc](https://jemalloc.net/) (*optional*) if you would like to use jemalloc instead of the standard allocator.

### Configuring Pure

1. Set the `CPL` environment variable to your Pure root (i.e., the directory containing this file). e.g., if you use Bash, put the following line in your `.bash_profile` or `.bashrc`: 
    ```bash
    export CPL=path/to/pure`
    ```

2. Install [jsoncpp](https://github.com/open-source-parsers/jsoncpp), which we use to create json-based statistics for profiling purposes. Ideally just install jsoncpp using your favorite package manager (e.g., `apt-get` or `brew`). You may also use the included `build_jsoncpp` script: 
    ```bash
    ./$CPL/support/misc/build_jsoncpp.sh && cd $CPL/src/3rd_party/jsoncpp && python amalgamate.py
    ```

3. Install [Boost](https://www.boost.org/users/download/) and update `BOOST_INCLUDE` and `BOOST_LIB_DIR` in `support/Makefile_includes/Makefile.misk.mk`. Note: Boost 1.63 has been tested and other newer versions should work.

4. You will likely have to fix some Makefile variables in `support/Makefile_includes/Makefile.misk.mk` and possibly others to ensure that important variables such as `cc`, `CC`, `MPICH_PATH`, `MPIRUN`, `CFLAGS`, `CXXFLAGS`, `LFLAGS`, `NPROC` (to get the number of processors on your system, which is system-dependent). We recommend creating a new `ifeq` section in `Makefile.misc.mk` to select for your system (we use the `OS` environment variable but feel free to select on a different unique system specifier).






## Writing and Compiling Pure Applications <a name="applications"></a>

### Compiling Pure applications

1. `#include "pure.h` in your C++ source files that make calls to the Pure runtime.

2. Build your Pure applications using the provided Make-based Pure build system. Generally, configure your application using the Make variables listed in `test/Makefile.include.mk` that you wish to change from the default. Then, `include ../../Makefile.include.mk` at the bottom of your application `Makefile`. When you use the provided Pure build infrastructure, which we highly recommend, libpure will automatically be built and linked into your application executable. Note that you can choose if you prefer a static or dynamic libpure using the `LINK_TYPE` application Makefile variable (`LINK_TYPE = static` or `LINK_TYPE = dynamic`). The build system also includes necessary header file search paths. See the example programs in `test/*/pure/`.

3. After you configure your application Makefile, build your code, run `make` and to run your application, run `make run`.

4. N.B. Pure's build system includes an <a href="#build-targets">extensive set of build targets</a> to help to build, run, debug, and profile your applications. You can browse the targets in `test/Makefile.include.mk` and `support/Makefile_includes/*.mk`.


### Compiling Non-Pure applications (e.g., MPI) using Pure infrastructure

This distribution also includes infrastructure to build and profile non-Pure applications. This is useful as it allows you to create "baseline" applications to compare against Pure and use the same profiling infrastructure to time and compare baseline applications to Pure applications. 

**To compile non-Pure applications, the application developer needs to:**

1. `#include "pure_application_helpers.h` in your C++ source files that make calls to the helpers provided by Pure. Note that these helpers do not provide Pure runtime functionality but rather general application helpers related to benchmarking, profiling, and writing clean code.

2. Build your non-Pure applications using the provided Make-based build system. Generally, configure your application using the Make variables listed in `support/Makefile_includes/Makefile.nonpure.mk` that you wish to change from the default. Then, `include $(CPL)/support/Makefile_includes/Makefile.nonpure.mk` at the bottom of your application `Makefile`. See the example programs in `test/*/baseline/`.

3. To build your code, run `make` and to run your application, run `make run`.

4. N.B. Pure's build system includes an extensive set of <a href="#user-content-compiler_flags">compile-time options</a> and <a href="#user-content-build_targets">build targets</a> to help to modify the behavior of the Pure runtime and build, run, debug, and profile your applications, respectively. You can browse the targets in `support/Makefile_includes/*.mk`.


### Example Programs

You can find simple Pure programs in the `test` directory. We have additional programs that we are in the process of adding to this repository.


### Pure Compiler Flags <a name="compiler_flags"></a>

All options for the Pure runtime system are controlled using compile-time flags, which are typically specified in the application `Makefile`. Most of these variables have reasonable defaults in `tests/Makefile.include.mk`, but you can override them to test out different options. The Pure library and the application are built and stored in a directory that encodes the state of all of these configuration options using a SHA1 hash of the options; so, your system can have pre-built `libpure`s and application binaries for different configuration options. Many of the general options are available for both Pure and "non-Pure" applications (i.e., MPI) to ensure we are using the same basic options when comparing performance.


#### Required options ####

Your application `Makefile` *must* specify the following:
  * `TOTAL_THREADS`: The total number of ranks in your application (possibly spread out over multiple machines). [Type: `integer`]
  * `RUN_ARGS`: Command-line arguments passed to your application (i.e., readable using `argv`)
  * `ENABLE_HYPERTHREADS`: Specifies whether or not to run ranks on logical cores (aka "HyperThreads") or not.  [Type: `0` or `1`]
  * `PURE_USER_CODE_SRCS`: Space-separated list of C or C++ source files that are run through the MPI-to-Pure source translator. For example, `source_file.cpp` will be rewritten and saved as `source_file.purified.cpp`. [Type: text]
  * `NON_PURIFIED_SOURCES`: Space-separated list of C or C++ source files that are not run through the MPI-to-Pure source translator. These files should make any calls to the Pure runtime explicitely. [Type: text]
  * `BIN_NAME`: The name of your binary file. [Type: text]



#### Most commonly-used options ####
  * `PURE_NUM_PROCS`: Manually specifies the number of processes running across your entire system. Use `AUTO` to have the system use a good/reasonable default based on other settings. Default: `AUTO`. [Type: `integer` if not `AUTO`]
  * `PURE_RT_NUM_THREADS`: Manually specifies the number of threads (and therefore ranks) to run per Pure process. Default: `AUTO`. [Type: `integer` if not `AUTO`]
  * `THREADS_PER_NODE_LIMIT`: When `PURE_NUM_PROCS` and  `PURE_RT_NUM_THREADS` are set to `AUTO`, limits the number of ranks (threads) on a node to the set amount instead of the number of cores on that node (real or virtual, depending on the value of `ENABLE_HYPERTHREADS`).

  * `PROCESS_CHANNEL_VERSION`: Specifies the version of the intra-process message data structure that is used. We recommend using version `40` when not using Pure Tasks and version `460` to enable work stealing when using Pure Tasks. There are number of other versions; see `support/Makefile_includes/determine_preprocessor_vars.rb` for details. Version `411` is a useful mode to test that Pure Tasks are producing the correct value when work stealing is disabled. [Type: integer]
  * `PCV_4_NUM_CONT_CHUNKS`: The number of "chunks" a Pure Task is broken into, which is relevant when work stealing is enabled. [Type: `0` or `1`]

  * `DEBUG`: Builds runtime and application in debug mode. Includes many runtime error checks, builds with debugging symbols (`-g`) and no compiler optimization (`-O0`, `-fno-omit-frame-pointer`, etc.). [Type: `0` or `1`]
  * `PROFILE`: Builds runtime and application with debugging symbols but also with compiler optimizations (`-O3`). Useful for performance profiling. [Type: `0` or `1`]
  * `RELEASE`: Builds runtime and application with no debugging symbols and with all compiler optimizations (`-O3`, `-march=native`, etc.). Most likely to provide optimal runtime performance [Type: `0` or `1`]
  * `DISABLE_PURIFICATION`: Disables the MPI-to-Pure source code translator. Default: `0`. [Options: `0` or `1`]

#### Debugging options ####
  * `PAUSE_FOR_DEBUGGER_ATTACH`: Pauses the application upon startup and before the application runs to give you a chance to attach your debugger to a Pure process. It prints out the `pid` of up to four Pure processes.
  * `ASAN`: Enables the `-fsanitize=address` compiler flag, which compiles and runs your application with [Address Sanitizer](https://clang.llvm.org/docs/AddressSanitizer.html) enabled. Useful for finding memory leaks, double frees, out-of-bounds accesses, use-after-frees, etc. 
  * `TSAN`: Enables the `-fsanitize=thread` compiler flag, which compiles and runs your application with [Thread Sanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html) enabled. Useful for data races in your application (including the Pure Runtime).
  * `MSAN`: Enables the `-fsanitize=memory` compiler flag, which compiles and runs your application with [Memory Sanitizer](https://clang.llvm.org/docs/MemorySanitizer.html) enabled. Useful for detecting uninitialized reads.
  * `UBSAN`: Enables the `-fsanitize=undefined` compiler flag, which compiles and runs your application with [Undefined Behavior Sanitizer](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html) enabled. Useful for detecting undefined behavior such as dereferencing misaligned or null pointers or signed integer overflow.
  * `COLLECT_THREAD_TIMELINE_DETAIL`: Runs the application collecting trace cycle-based timepoints, which can then be fed to the Pure Timeline profiler. Default: `0`. [Options: `0` or `1`]
  * `DO_PRINT_CONT_DEBUG_INFO`: Prints out Pure Task debugging logs, which specify which Pure ranks execute which chunks of a particular Pure Task. Useful for debugging and getting a sense of distribution of chunk execution.


#### Other application options ####
  * `NUMA_SCHEME`: Specifies the manner in which ranks are laid out and pinned on the cores of a system. Specific to each system you run on. See `support/Makefile_includes/Makefile.cpu_config.mk`. Examples: `bind_sequence`, `bind_alternating`, `none`, etc.
  * `USE_JEMALLOC`: Uses the [jemalloc memory allocator](https://jemalloc.net/) instead of the default allocator.
  * `USER_CFLAGS`: Additional compiler flags to add when compiling C source files.
  * `USER_CXXFLAGS`: Additional compiler flags to add when compiling C++ source files.
  * `USER_LFLAGS`: Additional linker flags to add when linking.
  * `LINK_TYPE`: Determines if libpure is built as a static or dynamic library. Default: `dynamic`. [Options: `static` or `dynamic`]


#### Other Pure options ####
  * `PROCESS_CHANNEL_BUFFERED_MSG_SIZE`: Number of usable entries in the lock-free circular buffer that is used for intra-node messaging ("Process Channels"). 
  * `BUFFERED_CHAN_MAX_PAYLOAD_BYTES`: Threshold, in bytes, between using "buffered" and "rendezvous" style point-to-point messaging. If the message size is equal to or less than `BUFFERED_CHAN_MAX_PAYLOAD_BYTES` Pure will use the buffered (i.e., copy twice) approach. Default: `8192`. [Type: integer]


### Pure Build System Make Targets <a name="build_targets"></a>

The Pure build system comes with many make-driven tools to help debug, profile, and run Pure applications. See below for some of the most useful targets. Run these commands from the application directory (where the application's `Makefile` is).

#### Most common targets ####
  * `make`: Default target builds the application, including libpure
  * `make run`: Builds and runs the application 
  * `make vars`: Prints out the current configuration of key build parameters
  * `make clean`: Deletes object files, libraries (i.e., libpure), and application executables 
  * `make clean_test`: Deletes application object files and executables 

#### Debugging targets ####
  * `make gdb`: Loads the application in gdb. Tip: define commands to be run when gdb first loads by defining `USER_GDB_COMMANDS` in your application Makefile.
  * `make gdb-run`: like the `gdb` target, but immediately runs the program in [gdb](https://sourceware.org/gdb/).
  * `make lldb`: Runs application in [lldb](https://lldb.llvm.org/).
  * `make valgrind`: Checks for memory leaks with [valgrind memcheck](https://valgrind.org/info/tools.html#memcheck). Note: We recommending running with `ASAN=1` in your Makefile instead of this.
  * `make massif`: Profiles heap using [valgrind massif](https://valgrind.org/info/tools.html#memcheck). Also see other related targets: `massif-stack`, `ms_print`, `ms_print_stack`, `ms_print_totals`

#### Profiling targets ####
  * `make profile`: Does a performance counter-based profiling of the application; uses [Linux perf](https://perf.wiki.kernel.org/index.php/Main_Page). By default, measures cycles (`cycles:ppp`), overridable with `DEFAULT_PERF_EVENTS` environment variable. Run `make profile-report` to see the results of the profile.
  * `make profile-report`: View the perf-based results of a profile collected with `make profile`
  * `make flamegraph`: Generates a [Flamegraph](https://www.brendangregg.com/flamegraphs.html) for your application using perf. Defaults to visualizing cycles. 
  * `make thread-timeline`: Runs Pure's thread-timeline tool to visualize application and runtime event durations, and visualizes it in an interactive web-based interface. 
  * `make profile-stat`: View performance counter stats (using `perf stat`) 
  * `make profile-c2c`: Runs [`perf-c2c`](https://man7.org/linux/man-pages/man1/perf-c2c.1.html) cacheline contention analyzer on your Pure application. 

#### Other targets ####
  * `make libpure`: Build libpure only 
  * `make tidy`: Runs [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) on the codebase
  * `make purify-all`: Runs the Pure MPI-to-Pure source-level translator on Pure application code. Run by default automatically in above common build commands so this is usually not run by itself.
  * `make ranks-on-topo`: Creates a PDF showing the Pure ranks on top of the CPU topology. Useful if you are using custom rank layouts and want to make sure your rank layout is as you intended.
  * `make bloaty`: Profiles the binary size using [Google Bloaty](https://github.com/google/bloaty)
  * `make list-targets`: Lists out the possible targets of the Pure build system. Note: it's probably more helpful to use this list as this has descriptions of each target.



## Academic Papers <a name="papers"></a>

[PPoPP'24](https://dl.acm.org/doi/abs/10.1145/3627535.3638503) "Pure: Evolving Message Passing To Better Leverage Shared Memory Within Nodes"

