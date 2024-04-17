# Author: James Psota
# File:   set_recv_batch_version.sh 

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

echo "USAGE: $0 <version=v1,v2,v3>"
version=$1

#includes
d=$CPL/include/pure/transport/experimental
ln -sf $d/$version/recv_flat_batcher_$version.h $d/recv_flat_batcher.h
ln -sf $d/$version/recv_flat_batch_channel_$version.h $d/recv_flat_batch_channel.h

# sources
d=$CPL/src/transport/experimental
ln -sf $d/$version/recv_flat_batcher_$version.cpp $d/recv_flat_batcher.cpp
ln -sf $d/$version/recv_flat_batch_channel_$version.cpp $d/recv_flat_batch_channel.cpp

echo "Now using version $version. Make sure you are editing the correct file."