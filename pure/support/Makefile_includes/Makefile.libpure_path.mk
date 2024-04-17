# Author: James Psota
# File:   Makefile.libpure_path.mk 

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

LIB_DIR    := $(BUILD_ROOT)/lib
LIB_PREFIX := $(LIB_DIR)/$(OBJ_DIR_PREFIX)

CFLAGS += -DLINK_TYPE=$(LINK_TYPE)
CXXFLAGS += -DLINK_TYPE=$(LINK_TYPE)

ifeq ($(LINK_TYPE), static)
    LIBPURE_NAME = libpure.a
else
ifeq ($(OS), osx)
$(error Linking dynamically on OSX is currently not supported)
    LIBPURE_NAME = libpure.dylib
else
    LIBPURE_NAME = libpure.so
endif
endif
LIBPURE_FULL_PATH = $(LIB_PREFIX)/$(LIBPURE_NAME)
