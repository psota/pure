# Author: James Psota
# File:   mpich_debug_compile.sh 

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

set -ex

echo "Compiling DEBUG mode of MPICH. Run this from the mpich source directory."
EXTRA_FLAGS="-g3 -ggdb -fno-inline -fno-omit-frame-pointer"
MODE=debug

MPICHLIB_CFLAGS=$EXTRA_FLAGS MPICHLIB_CPPFLAGS=$EXTRA_FLAGS MPICHLIB_CXXFLAGS=$EXTRA_FLAGS ./configure \
--prefix=/afs/csail.mit.edu/u/j/jim/toolchains/mpich/$MODE --program-suffix=-$MODE --disable-fortran \
--enable-fast=O0 --enable-strict=all --enable-g=all CFLAGS=-g3 

make -j32

make install