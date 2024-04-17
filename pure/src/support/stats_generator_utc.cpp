// Author: James Psota
// File:   stats_generator_utc.cpp 

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

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <numeric>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

#include "pure/support/stats_generator.h"

namespace PureRT {

Json::Value gen_base_stats(int pure_thread_rank, const string& timestamp) {

    Json::Value json_root;
    json_root["hostname"] = PureRT::hostname_string();

    json_root["pure_thread_rank"] = pure_thread_rank;
    json_root["created_at"]       = timestamp;

    const char* pure_run_unique_id = std::getenv("PURE_RUN_UNIQUE_ID");
    json_root["pure_run_unique_id"] =
            pure_run_unique_id ? pure_run_unique_id : "";

    return json_root;
}

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
string write_to_file(Json::Value& json_root, int pure_thread_rank,
                     const string& timestamp) {

    const char* json_stats_dir  = std::getenv("JSON_STATS_DIR");
    json_root["json_stats_dir"] = json_stats_dir ? json_stats_dir : "";

    stringstream stats_base_filename, stats_filename;
    stats_base_filename << timestamp << "_rank_" << pure_thread_rank << ".json";
    json_root["stats_filename"] = stats_base_filename.str();

    ofstream stats_file;
    // if JSON_STATS_DIR is set (in the environment), use a special directory.
    // otherwise, just use temp_latest
    string parent_dir;
    if (json_stats_dir && strlen(json_stats_dir) > 0) {
        parent_dir = std::string(json_stats_dir) + std::string("/");
    } else {
        parent_dir = "runs/temp_latest/";
    }

    // make parent dir if necessary
    if (!directory_exists(parent_dir.c_str())) {
        if (mkdir_p(parent_dir.c_str()) != 0) {
            sentinel("Failed to mkdir_p directory %s", parent_dir.c_str());
        }
    }

    stats_filename << parent_dir << "stats_" << stats_base_filename.str();
    const size_t max_retries = 5;
    for (auto i = 0; i <= max_retries; ++i) {
        stats_file.open(stats_filename.str());

        if (stats_file.fail()) {
            if (i < max_retries) {
                continue;
            } else {
                std::cerr << "ERROR: stats output file " << stats_filename.str()
                          << " failed to open for writing after retrying "
                          << max_retries << " times. Exiting." << std::endl
                          << std::endl;
                stats_file.close();
                exit(-200);
            }

        } else {
            // file opened successfully
            stats_file << json_root;
            stats_file.close();
            stringstream ss;
            ss << json_root << std::endl;
            ss << std::endl;

            // this does a string copy but we want to do it even for release
            // compilations because at this point the program will already have
            // completed running
            if (ss.str().size() <= 0) {
                sentinel("JSON output string is of size zero but should have "
                         "data. JSON_STATS_DIR: %s",
                         json_stats_dir);
            }

            return ss.str();
        }
    } // ends for loop
}

// mutates json_root object
void populate_end_to_end(Json::Value& json_root, uint64_t elapsed_ns_monotonic,
                         uint64_t elapsed_ns_process_cpu,
                         uint64_t elapsed_ns_thread_cpu) {
    json_root["elapsed_ns_monotonic"] =
            static_cast<Json::UInt64>(elapsed_ns_monotonic);
    json_root["elapsed_ns_process_cpu"] =
            static_cast<Json::UInt64>(elapsed_ns_process_cpu);
    json_root["elapsed_ns_thread_cpu"] =
            static_cast<Json::UInt64>(elapsed_ns_thread_cpu);
}

std::string generate_stats(int pure_thread_rank, uint64_t elapsed_ns_monotonic,
                           uint64_t elapsed_ns_process_cpu,
                           uint64_t elapsed_ns_thread_cpu,
                           uint64_t memory_footprint_bytes) {

    string      timestamp = PureRT::currentDateTime();
    Json::Value json_root = gen_base_stats(pure_thread_rank, timestamp);
    populate_end_to_end(json_root, elapsed_ns_monotonic, elapsed_ns_process_cpu,
                        elapsed_ns_thread_cpu);

    json_root["memory_footprint_bytes"] =
            static_cast<Json::UInt64>(memory_footprint_bytes);
    json_root["cpu_number"] = get_cpu_on_linux();

    return write_to_file(json_root, pure_thread_rank, timestamp);
}

// overloaded version which allows a vector of custom timers
std::string
generate_stats(int pure_thread_rank, const BenchmarkTimer& end_to_end_timer,
               const std::vector<BenchmarkTimer*>& user_custom_timers,
               const std::vector<BenchmarkTimer*>& runtime_custom_timers,
               uint64_t                            memory_footprint_bytes) {

    string timestamp = PureRT::currentDateTime();

    Json::Value json_root = gen_base_stats(pure_thread_rank, timestamp);
    populate_end_to_end(json_root, end_to_end_timer.elapsed_ns_monotonic(),
                        end_to_end_timer.elapsed_ns_process_cpu(),
                        end_to_end_timer.elapsed_ns_thread_cpu());

    std::vector<BenchmarkTimer*> all_custom_timers;
    for (const auto e : user_custom_timers) {
        all_custom_timers.push_back(e);
    }
    for (const auto e : runtime_custom_timers) {
        all_custom_timers.push_back(e);
    }

    check(all_custom_timers.size() <= MAX_CUSTOM_TIMERS,
          "There should be at most %d custom timers, but there are %d. To "
          "increase this number, update the constant in benchmark_timer.h and "
          "also heed the insturctions above that constant definition.",
          MAX_CUSTOM_TIMERS, all_custom_timers.size());

    int custom_cnt = 0;
    for (const auto& t : all_custom_timers) {
        char custom_name[32];
        sprintf(custom_name, "custom%d_", custom_cnt);
        char this_label[32];

        strcpy(this_label, custom_name);
        char* suffix_start = this_label + strlen(custom_name);

        strcpy(suffix_start, "timer_name");
        json_root[this_label] = t->Name();

        strcpy(suffix_start, "elapsed_ns_monotonic");
        json_root[this_label] =
                static_cast<Json::UInt64>(t->elapsed_ns_monotonic());

        strcpy(suffix_start, "elapsed_ns_process");
        json_root[this_label] =
                static_cast<Json::UInt64>(t->elapsed_ns_process_cpu());
        strcpy(suffix_start, "elapsed_ns_thread");
        json_root[this_label] =
                static_cast<Json::UInt64>(t->elapsed_ns_thread_cpu());
        ++custom_cnt;
    }

    json_root["memory_footprint_bytes"] =
            static_cast<Json::UInt64>(memory_footprint_bytes);
    json_root["cpu_number"] = get_cpu_on_linux();

    return write_to_file(json_root, pure_thread_rank, timestamp);
}

#if COLLECT_THREAD_TIMELINE_DETAIL

// Thread Timeline Functions
/*
 * Generates a CSV file with the following form:
 *   thread_rank, timer_name, start_ns_from_beginning, end_ns_from_beginning,
 * thread_comm_partner_rank, sequence_number, interval_note
 *
 * with filename timer_interval_data/r7_<timer_name>.csv
 */
void TimerIntervalDetailsToCsv(
        int rank, BenchmarkTimer& end_to_end_timer,
        const std::vector<BenchmarkTimer*>& user_custom_timers,
        const std::vector<BenchmarkTimer*>& runtime_custom_timers) {

    // combine the timers vector into a single vector, ordered by
    // start_ns_from_beginning
    std::vector<BenchmarkTimer*> all_timers;
    all_timers.reserve(user_custom_timers.size() +
                       runtime_custom_timers.size() + 1);

    all_timers.push_back(&end_to_end_timer); // end_to_end timer is special
    for (const auto e : user_custom_timers) {
        all_timers.push_back(e);
    }
    for (const auto e : runtime_custom_timers) {
        all_timers.push_back(e);
    }

    assert(all_timers.size() > 0);
    std::vector<TimerIntervalDetails> combined_intervals;

    const size_t total_num_intervals =
            std::accumulate(all_timers.begin(), all_timers.end(), 0,
                            [](int sum, const BenchmarkTimer* timer) {
                                return sum + timer->NumIntervalDetails();
                            });

    std::unordered_map<std::string, int> name_to_total_count_map;
    combined_intervals.reserve(total_num_intervals);

    // combine one pair at a time
    for (auto i = 0; i < all_timers.size(); ++i) {
        // note that some all_timers will be empty, especially true for runtime
        // all_timers.
        name_to_total_count_map.insert(std::pair<std::string, int>(
                all_timers[i]->Name(), all_timers[i]->NumIntervalDetails()));

        printf("%s: %u\n", all_timers[i]->Name().c_str(),
               all_timers[i]->NumIntervalDetails());

        // this is a hack and I'm sure there's a better way, but it will save
        // runtime time during the application
        // assert(strlen(all_timers[i]->Name()) <= MAX_BT_STR_LEN);
        for (auto& id : all_timers[i]->IntervalDetails()) {
            // strncpy(id.timer_name_, all_timers[i]->Name().c_str(),
            //         MAX_BT_STR_LEN);
            id.bt = all_timers[i];
        }

        combined_intervals.insert(combined_intervals.end(),
                                  all_timers[i]->IntervalDetails().begin(),
                                  all_timers[i]->IntervalDetails().end());
    }

    // this should really use std::merge but I was hitting weird memory
    // errors
    std::sort(combined_intervals.begin(), combined_intervals.end(),
              [](const TimerIntervalDetails a, const TimerIntervalDetails b) {
                  if (timespecEqualTo(a.start_clock_monotonic_,
                                      b.start_clock_monotonic_)) {
                      // start times are the same; sort based on end time
                      return timespecGreaterThan(b.end_clock_monotonic_,
                                                 a.end_clock_monotonic_);

                  } else {
                      // sort based on start time
                      return timespecGreaterThan(b.start_clock_monotonic_,
                                                 a.start_clock_monotonic_);
                  }
              });

    /////////

    stringstream csv_base_filename;
    csv_base_filename << "r" << rank << "_all_timers.csv";
    string parent_dir = "timer_interval";

    // make parent dir if necessary
    if (!directory_exists(parent_dir.c_str())) {
        if (mkdir_p(parent_dir.c_str()) != 0) {
            sentinel("Failed to mkdir_p directory %s", parent_dir.c_str());
        }
    }

    stringstream csv_filename;
    csv_filename << parent_dir << "/" << csv_base_filename.str();

    ofstream csv_stream;

    const struct timespec program_origin = end_to_end_timer.OriginTimespec();
    const size_t          max_retries    = 5;
    for (auto i = 0; i < max_retries; ++i) {
        csv_stream.open(csv_filename.str());
        if (csv_stream.fail()) {
            if (i < max_retries) {
                continue;
            } else {
                std::cerr << "ERROR: CSV timer interval output file "
                          << csv_filename.str()
                          << " failed to open for writing after retrying "
                          << max_retries << " times. Exiting." << std::endl
                          << std::endl;
                csv_stream.close();
                exit(-200);
            }
        } else {
            // file opened successfully

            // header
            csv_stream << "thread_rank,timer_name,start_ns_from_beginning,"
                          "end_ns_from_beginning,duration_ns,thread_comm_"
                          "partner_rank,sequence_number,total_num_this_timer,"
                          "program_origin_sec,program_origin_nsec"
                       << std::endl;

            // loop through all rows in interval details
            for (const auto& id : combined_intervals) {

                if (timespecGreaterThan(program_origin,
                                        id.start_clock_monotonic_)) {
// skip any early intervals to other timers that may
// have happened before the main call happened.
#if DEBUG_CHECK
                    fprintf(stderr, "Skipping interval for %s which was "
                                    "called before the main end-to-end "
                                    "timer started.\n",
                            id.bt->Name().c_str());
#endif
                    continue;
                }

                // fprintf(stderr, "program origin: %zu %zu\n",
                //         program_origin.tv_sec, program_origin.tv_nsec);
                // fprintf(stderr, "%s: %zu %zu\n", id.timer_name_.c_str(),
                //         id.start_clock_monotonic_.tv_sec,
                //         id.start_clock_monotonic_.tv_nsec);

                csv_stream << rank << "," << id.bt->Name() << ","
                           << timespecDiff<true>(id.start_clock_monotonic_,
                                                 program_origin)
                           << "," << timespecDiff<true>(id.end_clock_monotonic_,
                                                        program_origin)
                           << "," << id.NanosecDuration() << ","
                           << id.partner_rank_ << "," << id.sequence_number_
                           << "," << name_to_total_count_map[id.bt->Name()]
                           << "," << program_origin.tv_sec << ","
                           << program_origin.tv_nsec << std::endl;
            }
            csv_stream.close();
        } // ends file opened success
    }

} // ends function
#endif

} // namespace PureRT
