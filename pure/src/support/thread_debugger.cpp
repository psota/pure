// Author: James Psota
// File:   thread_debugger.cpp 

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

#include "pure/support/thread_debugger.h"


// invoked by PureProcess which is invoked by PureThread


ThreadDebugger::ThreadDebugger(size_t num_threads_arg, bool enabled) {
	num_threads = num_threads_arg;
	num_breakpoints_reached = 0;

    // the "go ahead" sychronizers are per-thread
	user_triggered_go_ahead_mutex = new std::mutex[num_threads];
	user_triggered_go_ahead_cv = new std::condition_variable[num_threads];
	user_triggered_go_ahead = new std::atomic_bool[num_threads];
    labels = new std::string[num_threads];

	for(auto i=0; i<num_threads; ++i) {
		user_triggered_go_ahead[i] = false;
	}

    user_interface_polling_in_progress = false;
    run_debugger = enabled;

    // TODO(jim): there's a data race somehow with this call, as suggested by Google Thread Sanitizer
    // SUMMARY: ThreadSanitizer: data race /afs/csail.mit.edu/u/j/jim/toolchains/gcc_4.8.2/include/c++/4.8.2/ext/new_allocator.h:110 __gnu_cxx::new_allocator<std::_Sp_counted_ptr_inplace<std::thread::_Impl<std::_Bind_simple<std::_Mem_fn<void (ThreadDebugger::*)()> (ThreadDebugger*)> >, std::allocator<std::thread::_Impl<std::_Bind_simple<std::_Mem_fn<void (ThreadDebugger::*)()> (ThreadDebugger*)> > >, (__gnu_cxx::_Lock_policy)2> >::deallocate(std::_Sp_counted_ptr_inplace<std::thread::_Impl<std::_Bind_simple<std::_Mem_fn<void (ThreadDebugger::*)()> (ThreadDebugger*)> >, std::allocator<std::thread::_Impl<std::_Bind_simple<std::_Mem_fn<void (ThreadDebugger::*)()> (ThreadDebugger*)> > >, (__gnu_cxx::_Lock_policy)2>*, unsigned long)
	user_interface_thread = new std::thread(&ThreadDebugger::UserInterfaceThreadFunc, this);

    user_interface_gdb_mode_enabled = false;

}

ThreadDebugger::~ThreadDebugger() {

    run_debugger = false; // tells the user interface thread to exit; likely unnecessary because Terminate() should be used.
    user_interface_thread->join();

    delete(user_interface_thread);
    delete[] user_triggered_go_ahead_mutex;
    delete[] user_triggered_go_ahead_cv;
    delete[] user_triggered_go_ahead;
    delete[] labels;

}


void ThreadDebugger::Breakpoint(int thread_id, std::string label) {

    if(!run_debugger.load()) { return; // early exit
}

    assert(thread_id >= 0);
    assert(thread_id < num_threads);


    sleep_random_seconds(220);
    fprintf(stderr, KGRN ">>> TDB:" KRESET);
    std::cerr << " thread " << thread_id << " breakpoint: '" << label << "'" << std::endl;

    if(DEBUG) { sleep_random_seconds(10);
}
    if(DEBUG) { std::cerr << "thread " << thread_id << " at beginning of ThreadDebugger::Breakpoint and EligibleThreadsToRun = " << EligibleThreadsToRun() << std::endl;
}

    labels[thread_id] = label;

    // first, wait until the currently-running breakpoint is done, if it is so running.
    if(user_interface_polling_in_progress) {
        std::unique_lock<std::mutex> user_interface_polling_in_progress_lock(user_interface_polling_in_progress_mutex);
        user_interface_polling_in_progress_cv.wait(
            user_interface_polling_in_progress_lock,
            [this](){ return !user_interface_polling_in_progress; }
            ); 
    }

    // reset this state. doing this here instead of at the end of the UI thread as that
    // can cause a data race without additional synchronization. It should be fine to do
    // this here because this value doesn't change by the UI thread until the user-interactive
    // polling portion, and at this point in this function the UI thread is still sleeping.
    user_triggered_go_ahead[thread_id] = false;

    if(DEBUG) { sleep_random_seconds(100);
}
    if(DEBUG) { std::cerr << "thread " << thread_id << " got past ui polling in progress check and EligibleThreadsToRun = " << EligibleThreadsToRun() << std::endl;
}

	// signals the user interface thread to wake up
    {
    	std::unique_lock<std::mutex> breakpoint_reached_lock(breakpoint_reached_mutex);
        ++num_breakpoints_reached;
        assert(num_breakpoints_reached >= 0);
        assert(num_breakpoints_reached <= num_threads);
        if(DEBUG) { sleep_random_seconds(3);
}
		if(DEBUG) { std::cerr << "thread " << thread_id << " reached breakpoint and will now notify the UI thread. " << EligibleThreadsToRun() << std::endl;
}

        // TODO(jim): add this back: std::cerr << "TDB[" << thread_id << "] BREAKPOINT (" << label << ")" << std::endl;
    } // breakpoint_reached_mutex is released here at the end of the scope
    breakpoint_reached_cv.notify_one();

    if(DEBUG) { sleep_random_seconds(6);
}
    if(DEBUG) { std::cerr << "thread " << thread_id << " getting user_triggered_go_ahead_lock lock" << std::endl;
}

    // block on go-ahead from UI thread
    std::unique_lock<std::mutex> user_triggered_go_ahead_lock(user_triggered_go_ahead_mutex[thread_id]);

    if(DEBUG) { sleep_random_seconds(4);
}
    if(DEBUG) { std::cerr << "Thread " << thread_id << " got user_triggered_go_ahead_lock; now waiting for signal from UI..." << std::endl;
}
    if(DEBUG) { std::cerr << "Thread " << thread_id << " got user_triggered_go_ahead_lock; now waiting for signal from UI..." << std::endl;
}

    user_triggered_go_ahead_cv[thread_id].wait( 
        user_triggered_go_ahead_lock, 
        [this, thread_id](){
            if(DEBUG) { std::cerr << "*** IN WAIT: thread " << thread_id << " waiting; user_triggered_go_ahead[thread_id] = " << user_triggered_go_ahead[thread_id].load() << std::endl;
}
            return user_triggered_go_ahead[thread_id].load();
        } 
        );

    if(DEBUG) { sleep_random_seconds(4);
}
    if(DEBUG) { std::cerr << std::endl << "Thread " << thread_id << " got signal from UI and is now exiting breakpoint" << std::endl;
}

}


void ThreadDebugger::Terminate() {

    //std::cerr << "ThreadDebugger::Terminate called and about to terminate UI thread." << std::endl;
    run_debugger = false;

    // this is a hack to make the UI thread wake up so it can check the run_debugger flag
    // because a thread can't wait on more than one condition variable.

    // TODO(jim): verify this, but I'm pretty sure this lock isn't needed

//    {
//        std::lock_guard<std::mutex> breakpoint_reached_lock(breakpoint_reached_mutex);
//    } // breakpoint_reached_mutex is released here at the end of the scope

    // signals the user interface thread to wake up
    breakpoint_reached_cv.notify_one();
}


std::string ThreadDebugger::EligibleThreadsToRun() {
    std::stringstream ss;
    ss << "{ ";
    for(auto i=0; i<num_threads; ++i) {
        if(!user_triggered_go_ahead[i].load(std::memory_order_relaxed)) {
            ss << i << " ";
        }
    }
    ss << "}";
    if(DEBUG) { ss << " num_breakpoints_reached = " << num_breakpoints_reached;
}
    if(DEBUG) { ss << " user_interface_polling_in_progress = " << user_interface_polling_in_progress.load(std::memory_order_relaxed);
}

    return ss.str();
}

std::string ThreadDebugger::FormattedLabels() {

    std::stringstream ss;
    for(auto i=0; i<num_threads; ++i) {
        if(!user_triggered_go_ahead[i].load(std::memory_order_relaxed)) {
            ss << "  thread " << i << ": " << labels[i] << std::endl;
        }
    }

    return ss.str();

}


void ThreadDebugger::UserInterfaceThreadFunc() {


    while(run_debugger.load(std::memory_order_relaxed)) {

        if(DEBUG) { sleep_random_seconds(10);
}
        if(DEBUG) { std::cerr << "TDB UI Thread: trying to get lock breakpoint_reached_lock" << std::endl;
}

    	std::unique_lock<std::mutex> breakpoint_reached_lock(breakpoint_reached_mutex);

        if(DEBUG) { sleep_random_seconds(10);
}
        if(DEBUG) { std::cerr << "TDB UI Thread: got lock breakpoint_reached_lock. now about to wait " << EligibleThreadsToRun() << std::endl;
}

        breakpoint_reached_cv.wait( breakpoint_reached_lock, 
            [this](){
                        if(DEBUG) { sleep_random_seconds(10);
}
                        if(DEBUG) { std::cerr << "in UI breakpoint_reached_cv.wait() -- details: " << EligibleThreadsToRun() << std::endl;
}
                        return (num_breakpoints_reached == num_threads || !run_debugger.load(std::memory_order_relaxed));
                    } 
        );

        // early exit condition if Terminate is called.
        if(!run_debugger.load()) {
            if(DEBUG) { std::cerr << "TDB UI Thread: Terminating early as run_debugger was set to false." << std::endl; 
}
            return;
        }

        user_interface_polling_in_progress = true;
     	
        // TODO(jim): remove this. testing.
        if(DEBUG) { sleep_random_seconds(10);
}
    	if(DEBUG) { std::cerr << "UI thread done waiting on all breakpoints reached." << std::endl;
}

        std::string string_input;
        std::stringstream stringsteam_input;
        int thread_id_submitted_by_user;
    	// now, all breakpoints have been reached. wait on the user's input.
        for(auto n = 0; n < num_threads; ++n) {

            while(true) {
                sleep_random_seconds(5);
                printf(KGRN ">>> TDB:" KRESET);
                std::cout << " which thread to run from " << EligibleThreadsToRun() << ", 'g' for gdb, or 'i' for info: ";

                getline(std::cin, string_input);
                std::stringstream stringstream_input(string_input);

                if(stringstream_input >> thread_id_submitted_by_user && 
                    thread_id_submitted_by_user >= 0 &&
                    thread_id_submitted_by_user < num_threads &&
                    !user_triggered_go_ahead[thread_id_submitted_by_user].load(std::memory_order_relaxed)
                    ) {
                    std::cerr << "Running thread " << thread_id_submitted_by_user << " (leaving bp " << labels[thread_id_submitted_by_user] << ")" << std::endl;
                    if(DEBUG) { std::cerr << "Got " << thread_id_submitted_by_user << " from user and go_ahead flag for this thread is " << user_triggered_go_ahead[thread_id_submitted_by_user].load(std::memory_order_relaxed) << std::endl;
}
                    break;
                } else if ((string_input.compare("g") == 0) || (string_input.compare("gdb") == 0)) {
                    
                    printf(KGRN "Suspending TDB for GDB. Type 'c' to continue.\n" KRESET);
                    user_interface_gdb_mode_enabled = true;
                    raise(SIGINT);

                } else if ((string_input.compare("i") == 0) || (string_input.compare("input") == 0)) {
                    std::cout << FormattedLabels();
                } else {
                    printf(KGRN "  >>>" KRESET);
                    std::cout << "ERROR: enter a valid thread id from " << EligibleThreadsToRun() << std::endl;
                    std::cin.clear();
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                }
            }
            std::cerr << std::endl;

            // signal that thread
            if(DEBUG) { sleep_random_seconds(4);
}
            if(DEBUG) { std::cout << "TDB UI Thread: go-ahead lock for thread " << thread_id_submitted_by_user << "..." << std::endl;
}

            {
                std::unique_lock<std::mutex> user_triggered_go_ahead_lock(user_triggered_go_ahead_mutex[thread_id_submitted_by_user]);
            
                if(DEBUG) { sleep_random_seconds(4);
}
                //if(DEBUG) std::cout << "TDB UI Thread: notifying thread " << thread_id_submitted_by_user << " to go ahead..." << std::endl;
                
                if(DEBUG) { std::cout << "TDB UI Thread: notifying thread " << thread_id_submitted_by_user << " to go ahead..." << std::endl;
}
                user_triggered_go_ahead[thread_id_submitted_by_user] = true;
            
            }
            user_triggered_go_ahead_cv[thread_id_submitted_by_user].notify_one();

        }

        // early exit for when Terminate() is called.
        // this is intentionally checked again after all breakpoints are handled
        // (via user triggering)
        if(!run_debugger.load(std::memory_order_relaxed)) { break;
}

        num_breakpoints_reached = 0; // reset for next time

        // finally, let's start any waiting threads that have already called their next breakpoint
        // and are ready to go.
        {
            std::unique_lock<std::mutex> user_interface_polling_in_progress_lock(user_interface_polling_in_progress_mutex);
            user_interface_polling_in_progress = false;
        }
        user_interface_polling_in_progress_cv.notify_all();
    
    }

}
