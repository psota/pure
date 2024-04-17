// Author: James Psota
// File:   thread_debugger.h 

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

#ifndef PURE_SUPPORT_THREAD_DEBUGGER_H
#define PURE_SUPPORT_THREAD_DEBUGGER_H

#pragma once

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <sstream> 
#include <string>
#include <string>
#include <thread>

#include "pure/support/helpers.h"

#define DEBUG false

class ThreadDebugger {

private:

	std::atomic_bool run_debugger;
	

	// TODO(jim): there should really be an array of structs 
	// instead of parallel arrays to hold these values.


	std::atomic_bool user_interface_polling_in_progress;
	std::mutex user_interface_polling_in_progress_mutex;
	std::condition_variable user_interface_polling_in_progress_cv;

	size_t num_threads;
	std::thread *user_interface_thread;

	std::string *labels;

	std::mutex breakpoint_reached_mutex;
	std::condition_variable breakpoint_reached_cv;
	int num_breakpoints_reached;

	// indexed by thread id. tells thread to resume running
	std::mutex *user_triggered_go_ahead_mutex;
	std::condition_variable *user_triggered_go_ahead_cv;
	std::atomic_bool *user_triggered_go_ahead;

	// TODO(jim): test this out for gdb
	std::mutex user_interface_gdb_mode_mutex;
	std::condition_variable user_interface_gdb_mode_cv;
	bool user_interface_gdb_mode_enabled;

	std::string EligibleThreadsToRun();
	std::string FormattedLabels();
	void UserInterfaceThreadFunc();
	
public:

	ThreadDebugger(size_t /*num_threads_arg*/, bool /*enabled*/);
	~ThreadDebugger();

	void Breakpoint(int /*thread_id*/, std::string /*label*/);
	void Terminate();

};
#endif
