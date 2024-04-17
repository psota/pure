# Author: James Psota
# File:   intel_tools_setup.bash 

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

echo "THIS SEEMS OLD / DEPRECATED -- probably don't use this."

set -ex

swap_to_gnu
module unload libfabric/1.6.2

module load cdt/19.03
module unload altd darshan
module swap intel{,/19.0.3.199}
module load itac/2019.up3
itacvars.sh 
export CRAYPE_LINK_TYPE=dynamic
export LD_PRELOAD=$VT_ROOT/slib/libVT.so

# make clean
# make