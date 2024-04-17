# Author: James Psota
# File:   Makefile.build_setup.mk 

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

ifeq ($(OS), nersc)
    # built on fast volume
    #BUILD_ROOT = /global/common/software/m1764/projects/purebits
    # the projects array takes too long to update often -- just stick with scratch
    BUILD_ROOT = $(CPL)
else
  # in-source builds for OSX and CSAIL
  BUILD_ROOT = $(CPL)
endif

BUILD_DIR    := $(BUILD_ROOT)/build
