#!/usr/bin/env ruby

# Author: James Psota
# File:   knl_core_id_lister.rb 

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



CORES_USED = 64
CORES_AVAIL = 68
THREADS = 4

cores = []

sequence = false
sequence_ht = !sequence

if(sequence)
    (0...THREADS).each do |thread|
        (0...CORES_USED).each do |core|
            core_id = (thread * CORES_AVAIL) + core
            cores << core_id
        end
    end
end

if(sequence_ht)
    (0...CORES_USED).each do |core|
        (0...THREADS).each do |thread|
            core_id = (thread * CORES_AVAIL) + core
            cores << core_id
        end
    end
end

puts cores.join(',')