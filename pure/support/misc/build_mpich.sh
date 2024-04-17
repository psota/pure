# Author: James Psota
# File:   build_mpich.sh 

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

# TODO: flesh this out using options see at https://www.mpich.org/static/downloads/1.3.1/mpich2-1.3.1-README.txt

VERSION=3.2

echo "Reminder: run this from the parent directory of the mpich-$VERSION source directory"
set -ex

echo "Building MPICH v$VERSION..."


if [ ! -d mpich-$VERSION ]; then
	wget http://www.mpich.org/static/downloads/$VERSION/mpich-$VERSION.tar.gz
	tar -xzvf mpich-$VERSION.tar.gz
fi
cd mpich-$VERSION

llvm_dir=/Volumes/Data/toolchains2/llvm
export CC=$llvm_dir/bin/clang 
export CXX=$llvm_dir/bin/clang
export LDFLAGS=$llvm_dir/lib/

./configure --enable-g=most --prefix=/Users/jim/local 
make clean
CPUS="$(nproc)"
make -j${CPUS}
make install

echo "Done building MPICH"
