// Author: James Psota
// File:   pure_preprocessor.h 

// Copyright (c) 2024 James Psota
// __________________________________________

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
// 
//   http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifdef PURE_PREPROCESSOR_H
#define PURE_PREPROCESSOR_H

// Clang attributes: https://clang.llvm.org/docs/AttributeReference.html


// not working right now. confused about semicolon.
// #if PROFILE_MODE

// #define PURE_DECL_ATTRIBUTE_NOINLINE_PROFILE __attribute__((noinline))
// #else
// #define PURE_DECL_ATTRIBUTE_NOINLINE_PROFILE 
// #endif

// #endif


#define RESTRICT __restrict__