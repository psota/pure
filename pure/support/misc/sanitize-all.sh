# Author: James Psota
# File:   sanitize-all.sh 

# Copyright (c) 2024 James Psota
# __________________________________________

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

#!/bin/bash

#set -ex

SANITIZER_LOG_DIR=sanitizer
rm -f ${SANITIZER_LOG_DIR}/*

##############################################
# Cleaning first so sanitizer files don't get removed with make clean
DEBUG=1 PROFILE=0 ASAN=1 TSAN=0 MSAN=0 UBSAN=0 VALGRIND_MODE=0  COLLECT_THREAD_TIMELINE_DETAIL=0 make clean
DEBUG=1 PROFILE=0 ASAN=0 TSAN=1 MSAN=0 UBSAN=0 VALGRIND_MODE=0 COLLECT_THREAD_TIMELINE_DETAIL=0 make clean
DEBUG=1 PROFILE=0 ASAN=0 TSAN=0 MSAN=1 UBSAN=0 VALGRIND_MODE=0 COLLECT_THREAD_TIMELINE_DETAIL=0 make clean
DEBUG=1 PROFILE=0 ASAN=0 TSAN=0 MSAN=0 UBSAN=1 VALGRIND_MODE=0 COLLECT_THREAD_TIMELINE_DETAIL=0 make clean

#############################################

echo "##### Running ASAN with current application Makefile settings"
DEBUG=1 PROFILE=0 ASAN=1 TSAN=0 MSAN=0 UBSAN=0 VALGRIND_MODE=0 COLLECT_THREAD_TIMELINE_DETAIL=0 make run

echo "##### Running TSAN with current application Makefile settings"
DEBUG=1 PROFILE=0 ASAN=0 TSAN=1 MSAN=0 UBSAN=0 VALGRIND_MODE=0 COLLECT_THREAD_TIMELINE_DETAIL=0 make run

echo "##### Running MSAN with current application Makefile settings"
DEBUG=1 PROFILE=0 ASAN=0 TSAN=0 MSAN=1 UBSAN=0 VALGRIND_MODE=0 COLLECT_THREAD_TIMELINE_DETAIL=0 make run

echo "##### Running UBSAN with current application Makefile settings"
DEBUG=1 PROFILE=0 ASAN=0 TSAN=0 MSAN=0 UBSAN=1 VALGRIND_MODE=0 COLLECT_THREAD_TIMELINE_DETAIL=0 make run

cd ${SANITIZER_LOG_DIR}
nfiles=$(ls | wc -w)
ls -lta

if [[ $nfiles -gt 0 ]]; then
    echo "\n##### FINISHED running all sanitizers and there were errors ($nfiles sanitizer files). See above for ERRORS."  
else
    echo "\n##### FINISHED running all sanitizers and there were NO errors. Success!"  
fi
