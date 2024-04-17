// Author: James Psota
// File:   colors.h 

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

#ifndef PURE_SUPPORT_COLORS_H
#define PURE_SUPPORT_COLORS_H

// USAGE: printf(KYEL KBOLD "yellow\n" KRESET);
//        cout << KYEL << "yellow" << KRESET
// TODO(jim): figure out usage with cout

// Details at * http://misc.flogisoft.com/bash/tip_colors_and_formatting
//            * https://en.wikipedia.org/wiki/ANSI_escape_code#Colors
//            * https://gist.github.com/twerth/1099707
/*            * gcc defaults:
The default GCC_COLORS is ‘error=01;31:warning=01;35:note=01;36:caret=01;32:locus=01:quote=01’ where
‘01;31’ is bold red, ‘01;35’ is bold magenta, ‘01;36’ is bold cyan, ‘01;32’ is bold green and ‘01’
is bold. Setting GCC_COLORS to the empty string disables colors.
*/

#define KNRM "\x1B[0m"

// Foreground Colors
#define KDEFAULT "\e[39m"
#define KRED "\x1B[31m"
#define KLT_RED "\033[1;31m"
#define KGRN "\x1B[32m"
#define KYEL "\x1B[33m"
#define KLT_YEL "\033[1;33m"
#define KPUR "\033[0;35m"
#define KLT_PUR "\033[1;35m"
#define KBLU "\x1B[34m"
#define KMAG "\x1B[35m"
#define KCYN "\x1B[36m"
#define KWHT "\x1B[37m"
#define KGREY "\e[90m"
#define KLT_GREY "\e[37m"

// Background Colors
#define KRED_BG "\x1B[41m"
#define KGRN_BG "\x1B[42m"
#define KYEL_BG "\x1B[43m"
#define KBLU_BG "\x1B[44m"
#define KMAG_BG "\x1B[45m"
#define KCYN_BG "\x1B[46m"
#define KWHT_BG "\x1B[47m"

// Formatting
#define KBOLD "\033[1m"
#define KNONBOLD "\e[21m"
#define KUNDER "\e[4m"
#define KDIM "\e[2m"
#define KINVERT "\e[7m"

// Reset
#define KRESET "\x1b[0m"

#endif
