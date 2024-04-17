# Author: James Psota
# File:   mpi_perf_app.sh 

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

# we need to wrap this into a script to get MPI to execute it cleanly and put the data to different files based on hostnames.

DATAFILE=perf.${HOSTNAME}.data
${PERF_CMD} -o ${DATAFILE} -- ${APP_CMD}

echo
echo SEE REPORT BY RUNNING_____________________________________________________________
echo "   ${PERF_REPORT_CMD} -i ${DATAFILE}"
echo