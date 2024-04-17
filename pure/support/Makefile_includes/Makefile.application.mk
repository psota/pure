# Author: James Psota
# File:   Makefile.application.mk 

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


# common makefile for the application setup that both nonpure and pure application makefiles include
# via Makefile.nonpure.mk and Makefile.include.mk

TEST_DIR_PREFIX := $(shell $(SUPPORT_PATH)/misc/test_subdir.rb $(TEST_DIR))
BUILD_TEST_DIR := $(BUILD_PREFIX)/$(TEST_DIR_PREFIX)

# extend binary name based on flags hash 
BIN_PREFIX := $(BIN_NAME)
export BIN_NAME_NO_PATH := $(BIN_PREFIX)@$(FLAGS_HASH)
BIN_NAME := $(BUILD_TEST_DIR)/$(BIN_NAME_NO_PATH)
export MAIN = $(BIN_NAME)