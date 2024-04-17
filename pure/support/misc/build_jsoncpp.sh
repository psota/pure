# Author: James Psota
# File:   build_jsoncpp.sh 

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

echo "BUILDING LIBJSONCPP... THE TEST WILL LIKELY FAIL. If you want to rebuild this for another toolchain (e.g., Intel vs. GNU), make sure to clean out the old library first with 'make clean'"

if [ $OS == 'osx' ]; then
    export C_INCLUDE_PATH=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include 
    export  LD_LIBRARY_PATH=/System/Library/Frameworks/System.Framework
    export DYLD_LIBRARY_PATH==/System/Library/Frameworks/System.Framework
fi

cd $CPL/src/3rd_party/jsoncpp
mkdir -p build/release
cd build/release
cmake -DCMAKE_BUILD_TYPE=release -DBUILD_STATIC_LIBS=OFF -DBUILD_SHARED_LIBS=ON -DARCHIVE_INSTALL_DIR=. -G "Unix Makefiles" ../..
make clean

if [ $OS == 'osx' ]; then
    export C_INCLUDE_PATH=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include  LD_LIBRARY_PATH=/System/Library/Frameworks/System.Framework  DYLD_LIBRARY_PATH==/System/Library/Frameworks/System.Framework make
else
    make
fi

echo "done."