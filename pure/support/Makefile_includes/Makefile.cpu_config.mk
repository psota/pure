# Author: James Psota
# File:   Makefile.cpu_config.mk 

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

# different configurations for cpu affinity on linux machines
# use $CPL/support/misc/cpu_binding_config.rb to help define these

ifneq ($(OS), osx)
#	 CPU_MODEL ?= $(shell cat /proc/cpuinfo | grep "model name" | head -1 | cut -c 14-)
	# CPU_MODEL must be defined in the environment (bashrc)
	ifeq ($(CPU_MODEL),)
$(error CPU_MODEL must be defined in the environment.)
	endif
endif

# Perlmutter: AMD EPYC 7763 64-Core Processor
ifeq ($(NERSC_HOST), perlmutter)
	# for use with helper cores -- must be defined to use helper cores
	#ALL_CPUS_IN_SEQ = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
	# use this command to generate values: cpu_binding_config.rb 0-15,128-143 16-31,144-159 32-47,160-175 48-63,176-191 64-79,192-207 80-95,208-223 96-111,224-239 112-127,240-255 seq

	#ALL_CPUS_ALTERNATING = 0,16,1,17,2,18,3,19,4,20,5,21,6,22,7,23,8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31,32,48,33,49,34,50,35,51,36,52,37,53,38,54,39,55,40,56,41,57,42,58,43,59,44,60,45,61,46,62,47,63

	ifeq ($(NUMA_SCHEME),hyperthread_siblings)
		NUMACTL_CPUS = 0,128
		NUMACTL_DESC = Hyperthread siblings
	else ifeq ($(NUMA_SCHEME),shared_l3)
		NUMACTL_CPUS = 0,1
		NUMACTL_DESC = Shared L3
	else ifeq ($(NUMA_SCHEME),different_numa)
		NUMACTL_CPUS = 0,16
		NUMACTL_DESC = Different NUMA nodes
	else ifeq ($(NUMA_SCHEME),bind_sequence)
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
		NUMACTL_DESC = NUMA 0 (all hts), then NUMA 1, up to NUMA 7
	else ifeq ($(NUMA_SCHEME),bind_sequence_defer_ht_use)
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
		NUMACTL_DESC = NUMA 0, then NUMA 1, up to NUMA 7, hts later

# consider adding another approach where we do one thread per core first, then loop back and do more.


# 	else ifeq ($(NUMA_SCHEME),bind_sequence_omp)
# 		NUMACTL_CPUS = 0,8,4,12,32,40,36,44,16,24,20,28,48,56,52,60
# 		NUMACTL_DESC = NUMA 0 then 1
# 	else ifeq ($(NUMA_SCHEME),bind_sequence_8_numa_threads)
# 		# WARNING! Only use this when THREADS_PER_NODE_LIMIT = 16
# 		# 8 threads per NUMA region, originally inspired by DT SH size D, which seems to only allow 16 threads per node in total
# 		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,16,17,18,19,20,21,22,23
# 		NUMACTL_DESC = NUMA 0 then 1, exactly 8 threads per NUMA node
# 		ifneq (${THREADS_PER_NODE_LIMIT},16)
# $(error bind_sequence_8_numa_threads only works when THREADS_PER_NODE_LIMIT = 16)
# 		endif
# 	else ifeq ($(NUMA_SCHEME),bind_sequence_20_numa_threads)
# 		# WARNING! Only use this when you want exactly 20 ranks per NUMA node (e.g., SH A)
# 		# 20 threads per NUMA region, originally inspired by DT SH size A, which has 40 threads per node
# 		# can be run with PureProcPerNuma and default Pure runtime
# 		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51
# 		NUMACTL_DESC = NUMA 0 then 1, exactly 20 threads per NUMA node

# 	else ifeq ($(NUMA_SCHEME),bind_sequence_22_numa_threads)
# 		# numa 0 then numa 1, 22 threads per numa region. Eg. for use with DT BH/WH size C
# 		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
# 		NUMACTL_DESC = NUMA 0 then 1, exactly 22 threads each numa node

# 	else ifeq ($(NUMA_SCHEME),bind_sequence_ht)
# 		NUMACTL_CPUS = 0,32,1,33,2,34,3,35,4,36,5,37,6,38,7,39,8,40,9,41,10,42,11,43,12,44,13,45,14,46,15,47,16,48,17,49,18,50,19,51,20,52,21,53,22,54,23,55,24,56,25,57,26,58,27,59,28,60,29,61,30,62,31,63
# 		NUMACTL_DESC = NUMA 0 then 1, hyperthreads packed
# 	else ifeq ($(NUMA_SCHEME),bind_alternating)
# 		NUMACTL_CPUS = 0,16,1,17,2,18,3,19,4,20,5,21,6,22,7,23,8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31,32,48,33,49,34,50,35,51,36,52,37,53,38,54,39,55,40,56,41,57,42,58,43,59,44,60,45,61,46,62,47,63
# 		NUMACTL_DESC = Alternating NUMA 0 and NUMA 1
# 	else ifeq ($(NUMA_SCHEME),bind_alternating_2)
# 		NUMACTL_CPUS = 0,1,16,17,2,3,18,19,4,5,20,21,6,7,22,23,8,9,24,25,10,11,26,27,12,13,28,29,14,15,30,31,32,33,48,49,34,35,50,51,36,37,52,53,38,39,54,55,40,41,56,57,42,43,58,59,44,45,60,61,46,47,62,63
# 		NUMACTL_DESC = Alternating NUMA 0 2 CPUs, and NUMA 1 2 CPUs
	else ifeq ($(NUMA_SCHEME),none)
		NUMACTL_CPUS=
		NUMACTL_DESC = Unspecified CPU affinity
	endif
endif


# Cori Haswell
ifeq ($(CPU_MODEL), Intel(R) Xeon(R) CPU E5-2698 v3 @ 2.30GHz)
	# for use with helper cores -- must be defined to use helper cores
	#ALL_CPUS_IN_SEQ = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
	ALL_CPUS_ALTERNATING = 0,16,1,17,2,18,3,19,4,20,5,21,6,22,7,23,8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31,32,48,33,49,34,50,35,51,36,52,37,53,38,54,39,55,40,56,41,57,42,58,43,59,44,60,45,61,46,62,47,63

	ifeq ($(NUMA_SCHEME),hyperthread_siblings)
		NUMACTL_CPUS = 0,32
		NUMACTL_DESC = Hyperthread siblings
	else ifeq ($(NUMA_SCHEME),shared_l3)
		NUMACTL_CPUS = 4,5
		NUMACTL_DESC = Shared L3
	else ifeq ($(NUMA_SCHEME),different_numa)
		NUMACTL_CPUS = 4,20
		NUMACTL_DESC = Different NUMA nodes
	else ifeq ($(NUMA_SCHEME),bind_sequence)
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
		NUMACTL_DESC = NUMA 0 then 1
	else ifeq ($(NUMA_SCHEME),bind_sequence_omp)
		NUMACTL_CPUS = 0,8,4,12,32,40,36,44,16,24,20,28,48,56,52,60
		NUMACTL_DESC = NUMA 0 then 1
	else ifeq ($(NUMA_SCHEME),bind_sequence_8_numa_threads)
		# WARNING! Only use this when THREADS_PER_NODE_LIMIT = 16
		# 8 threads per NUMA region, originally inspired by DT SH size D, which seems to only allow 16 threads per node in total
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,16,17,18,19,20,21,22,23
		NUMACTL_DESC = NUMA 0 then 1, exactly 8 threads per NUMA node
		ifneq (${THREADS_PER_NODE_LIMIT},16)
$(error bind_sequence_8_numa_threads only works when THREADS_PER_NODE_LIMIT = 16)
		endif
	else ifeq ($(NUMA_SCHEME),bind_sequence_20_numa_threads)
		# WARNING! Only use this when you want exactly 20 ranks per NUMA node (e.g., SH A)
		# 20 threads per NUMA region, originally inspired by DT SH size A, which has 40 threads per node
		# can be run with PureProcPerNuma and default Pure runtime
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51
		NUMACTL_DESC = NUMA 0 then 1, exactly 20 threads per NUMA node

	else ifeq ($(NUMA_SCHEME),bind_sequence_22_numa_threads)
		# numa 0 then numa 1, 22 threads per numa region. Eg. for use with DT BH/WH size C
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
		NUMACTL_DESC = NUMA 0 then 1, exactly 22 threads each numa node

	else ifeq ($(NUMA_SCHEME),bind_sequence_ht)
		NUMACTL_CPUS = 0,32,1,33,2,34,3,35,4,36,5,37,6,38,7,39,8,40,9,41,10,42,11,43,12,44,13,45,14,46,15,47,16,48,17,49,18,50,19,51,20,52,21,53,22,54,23,55,24,56,25,57,26,58,27,59,28,60,29,61,30,62,31,63
		NUMACTL_DESC = NUMA 0 then 1, hyperthreads packed
	else ifeq ($(NUMA_SCHEME),bind_alternating)
		NUMACTL_CPUS = 0,16,1,17,2,18,3,19,4,20,5,21,6,22,7,23,8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31,32,48,33,49,34,50,35,51,36,52,37,53,38,54,39,55,40,56,41,57,42,58,43,59,44,60,45,61,46,62,47,63
		NUMACTL_DESC = Alternating NUMA 0 and NUMA 1
	else ifeq ($(NUMA_SCHEME),bind_alternating_2)
		NUMACTL_CPUS = 0,1,16,17,2,3,18,19,4,5,20,21,6,7,22,23,8,9,24,25,10,11,26,27,12,13,28,29,14,15,30,31,32,33,48,49,34,35,50,51,36,37,52,53,38,39,54,55,40,41,56,57,42,43,58,59,44,45,60,61,46,47,62,63
		NUMACTL_DESC = Alternating NUMA 0 2 CPUs, and NUMA 1 2 CPUs
	else ifeq ($(NUMA_SCHEME),none)
		NUMACTL_CPUS=
		NUMACTL_DESC = Unspecified CPU affinity
	endif
endif

# Cori KNL
# IMPORTANT! only using 64 of the 68 physical KNL cores. Change this if we change this.
# Use knl_core_id_lister.rb to generate these.
ifeq ($(CPU_MODEL), Intel(R) Xeon Phi(TM) CPU 7250 @ 1.40GHz)
	ALL_CPUS_ALTERNATING = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256,257,258,259,260,261,262,263,264,265,266,267

	ifeq ($(NUMA_SCHEME),bind_sequence)
		NUMACTL_CPUS = $(ALL_CPUS_ALTERNATING)
		NUMACTL_DESC = NUMA 0, one HT per physical core at a time
	else ifeq ($(NUMA_SCHEME),bind_sequence_ht)
		NUMACTL_CPUS = 0,68,136,204,1,69,137,205,2,70,138,206,3,71,139,207,4,72,140,208,5,73,141,209,6,74,142,210,7,75,143,211,8,76,144,212,9,77,145,213,10,78,146,214,11,79,147,215,12,80,148,216,13,81,149,217,14,82,150,218,15,83,151,219,16,84,152,220,17,85,153,221,18,86,154,222,19,87,155,223,20,88,156,224,21,89,157,225,22,90,158,226,23,91,159,227,24,92,160,228,25,93,161,229,26,94,162,230,27,95,163,231,28,96,164,232,29,97,165,233,30,98,166,234,31,99,167,235,32,100,168,236,33,101,169,237,34,102,170,238,35,103,171,239,36,104,172,240,37,105,173,241,38,106,174,242,39,107,175,243,40,108,176,244,41,109,177,245,42,110,178,246,43,111,179,247,44,112,180,248,45,113,181,249,46,114,182,250,47,115,183,251,48,116,184,252,49,117,185,253,50,118,186,254,51,119,187,255,52,120,188,256,53,121,189,257,54,122,190,258,55,123,191,259,56,124,192,260,57,125,193,261,58,126,194,262,59,127,195,263,60,128,196,264,61,129,197,265,62,130,198,266,63,131,199,267
		NUMACTL_DESC = NUMA 0, all HTs at a time
	else ifeq ($(NUMA_SCHEME),none)
		NUMACTL_CPUS=
		NUMACTL_DESC = Unspecified CPU affinity
	endif
endif

# Edison and lanka research (match compute or login node, respectively, but config is for compute mode)
ifeq ($(CPU_MODEL), Intel(R) Xeon(R) CPU E5-2695 v2 @ 2.40GHz)
$(warning CONFIGURING $(NUMA_SCHEME) for EDISON)
		ifeq ($(NUMA_SCHEME),hyperthread_siblings)
			NUMACTL_CPUS = 0,24
			NUMACTL_DESC = Hyperthread siblings
		else ifeq ($(NUMA_SCHEME),shared_l3)
			NUMACTL_CPUS = 4,5
			NUMACTL_DESC = Shared L3
		else ifeq ($(NUMA_SCHEME),different_numa)
			NUMACTL_CPUS = 4,16
			NUMACTL_DESC = Different NUMA nodes
		else ifeq ($(NUMA_SCHEME),none)
			NUMACTL_CPUS=
			NUMACTL_DESC = Unspecified CPU affinity
		else ifeq ($(NUMA_SCHEME),bind_sequence)
			NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,24,25,26,27,28,29,30,31,32,33,34,35,12,13,14,15,16,17,18,19,20,21,22,23,36,37,38,39,40,41,42,43,44,45,46,47
			NUMACTL_DESC = NUMA 0 then 1
		else ifeq ($(NUMA_SCHEME),bind_alternating)
			NUMACTL_CPUS = 0,12,1,13,2,14,3,15,4,16,5,17,6,18,7,19,8,20,9,21,10,22,11,23,24,36,25,37,26,38,27,39,28,40,29,41,30,42,31,43,32,44,33,45,34,46,35,47
			NUMACTL_DESC = NUMA 0 then 1
		endif
	endif

ifeq ($(CPU_MODEL), Intel(R) Xeon(R) CPU E5-4603 0 @ 2.00GHz)
$(warning CONFIGURING $(NUMA_SCHEME) for EDISON. *** WARNING: Configuring on login node but numactl is for compute node. ***)
		ifeq ($(NUMA_SCHEME),hyperthread_siblings)
			NUMACTL_CPUS = 0,24
			NUMACTL_DESC = Hyperthread siblings
		else ifeq ($(NUMA_SCHEME),shared_l3)
			NUMACTL_CPUS = 4,5
			NUMACTL_DESC = Shared L3
		else ifeq ($(NUMA_SCHEME),different_numa)
			NUMACTL_CPUS = 4,16
			NUMACTL_DESC = Different NUMA nodes
		else ifeq ($(NUMA_SCHEME),none)
			NUMACTL_CPUS=
			NUMACTL_DESC = Unspecified CPU affinity
		endif
	endif

# salike and lanka (CSAIL)
# NUMA node0 CPU(s):     0-11,24-35
# NUMA node1 CPU(s):     12-23,36-47
ifeq ($(CPU_MODEL), Intel(R) Xeon(R) CPU E5-2680 v3 @ 2.50GHz)
$(warning CONFIGURING $(NUMA_SCHEME) for SALIKE or LANKA)
	ALL_CPUS_ALTERNATING = 0,1,2,3,4,5,6,7,8,9,10,11,24,25,26,27,28,29,30,31,32,33,34,35,12,13,14,15,16,17,18,19,20,21,22,23,36,37,38,39,40,41,42,43,44,45,46,47
	ifeq ($(NUMA_SCHEME),hyperthread_siblings)
		NUMACTL_CPUS = 0,24
		NUMACTL_DESC = Hyperthread siblings
	else ifeq ($(NUMA_SCHEME),shared_l3)
		NUMACTL_CPUS = 4,5
		NUMACTL_DESC = Shared L3
	else ifeq ($(NUMA_SCHEME),different_numa)
		NUMACTL_CPUS = 4,16
		NUMACTL_DESC = Different NUMA nodes
	else ifeq ($(NUMA_SCHEME),bind_sequence)
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,24,25,26,27,28,29,30,31,32,33,34,35,12,13,14,15,16,17,18,19,20,21,22,23,36,37,38,39,40,41,42,43,44,45,46,47
		NUMACTL_DESC = NUMA 0 then 1
	else ifeq ($(NUMA_SCHEME),bind_sequence_20_numa_threads)
		# WARNING! Only use this when you want exactly 20 ranks per NUMA node (e.g., SH A)
		# 20 threads per NUMA region, originally inspired by DT SH size A, which has 40 threads per node
		# can be run with PureProcPerNuma and default Pure runtime
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,24,25,26,27,28,29,30,31,12,13,14,15,16,17,18,19,20,21,22,23,36,37,38,39,40,41,42,43
		NUMACTL_DESC = NUMA 0 then 1, exactly 20 threads per NUMA node
	else ifeq ($(NUMA_SCHEME),bind_sequence_ht)
		NUMACTL_CPUS = 0,24,1,25,2,26,3,27,4,28,5,29,6,30,7,31,8,32,9,33,10,34,11,35,12,36,13,37,14,38,15,39,16,40,17,41,18,42,19,43,20,44,21,45,22,46,23,47
		NUMACTL_DESC = NUMA 0 then 1, hyperthreads packed
	else ifeq ($(NUMA_SCHEME),bind_alternating)
		NUMACTL_CPUS = 0,12,1,13,2,14,3,15,4,16,5,17,6,18,7,19,8,20,9,21,10,22,11,23,24,36,25,37,26,38,27,39,28,40,29,41,30,42,31,43,32,44,33,45,34,46,35,47
		NUMACTL_DESC = Alternating NUMA 0 and NUMA 1
	else ifeq ($(NUMA_SCHEME),bind_alternating_ht)
		NUMACTL_CPUS = 0,12,24,36,1,13,25,37,2,14,26,38,3,15,27,39,4,16,28,40,5,17,29,41,6,18,30,42,7,19,31,43,8,20,32,44,9,21,33,45,10,22,34,46,11,23,35,47
		NUMACTL_DESC = Alternating NUMA 0 and NUMA 1, hyperthreads packed
	else ifeq ($(NUMA_SCHEME),none)
		NUMACTL_CPUS=
		NUMACTL_DESC = Unspecified CPU affinity
	endif
endif

# Draco (CSAIL)
ifeq ($(CPU_MODEL), Intel(R) Xeon(R) CPU E5-2670 0 @ 2.60GHz)
$(warning CONFIGURING $(NUMA_SCHEME) for DRACO)
       ifeq ($(NUMA_SCHEME),hyperthread_siblings)
               NUMACTL_CPUS = 0,16
               NUMACTL_DESC = Hyperthread siblings
       else ifeq ($(NUMA_SCHEME),shared_l3)
               NUMACTL_CPUS = 4,5
               NUMACTL_DESC = Shared L3
       else ifeq ($(NUMA_SCHEME),different_numa)
               NUMACTL_CPUS = 4,12
               NUMACTL_DESC = Different NUMA nodes
       else ifeq ($(NUMA_SCHEME),bind_sequence)
               NUMACTL_CPUS = 0,1,2,3,4,5,6,7,16,17,18,19,20,21,22,23,8,9,10,11,12,13,14,15,24,25,26,27,28,29,30,31
               NUMACTL_DESC = NUMA 0 then 1
       else ifeq ($(NUMA_SCHEME),bind_alternating)
               NUMACTL_CPUS = 0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15,16,24,17,25,18,26,19,27,20,28,21,29,22,30,23,31
               NUMACTL_DESC = Alternating NUMA 0 and NUMA 1
       else ifeq ($(NUMA_SCHEME),none)
               NUMACTL_CPUS=
               NUMACTL_DESC = Unspecified CPU affinity
       endif
endif

# OSX (for debugging)
ifeq ($(OS), osx) 
#$(warning SIMULATING $(NUMA_SCHEME) for OSX. Not actually doing binding (this is just to get data collection scripts to run, etc.))
	ifeq ($(NUMA_SCHEME),hyperthread_siblings)
		NUMACTL_DESC = Hyperthread siblings
	else ifeq ($(NUMA_SCHEME),shared_l3)
		NUMACTL_DESC = Shared L3
	else ifeq ($(NUMA_SCHEME),different_numa)
		NUMACTL_DESC = Different NUMA nodes
	else ifeq ($(NUMA_SCHEME),none)
		NUMACTL_DESC = Unspecified CPU affinity
	else ifeq ($(NUMA_SCHEME), bind_sequence)
		NUMACTL_CPUS = 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
	endif
else 
	# not OSX
	ifneq ($(NUMA_SCHEME),none)
		ifeq ($(NUMACTL_CPUS),)
$(error NUMA_SCHEME is set to $(NUMA_SCHEME) but NUMACTL_CPUS is blank. You must configure for CPU_MODEL $(CPU_MODEL))
		endif
	endif
endif

ifneq ($(NUMACTL_CPUS),)
#	ifeq ($(RUNTIME), Pure)
		# compute the verbose core names if NUMACTL_CPUS is set
		# NUMACTL_VERBOSE_CORES is in S0-C1-T1 format
		#NUMACTL_VERBOSE_CORES = $(shell $(SUPPORT_PATH)/misc/phys_core_id_to_verbose_name.rb $(NUMACTL_CPUS))
		NUMACTL=numactl --physcpubind=$(NUMACTL_CPUS)
		export NUMACTL_CPUS
		MPIRUN_OPTS += -bind-to none
#	else
#$(warning NUMA MODE IGNORED FOR BASELINE/MPI ON CORI! (unclear if it works elsewhere))
#	endif
endif

export NUMACTL_CPUS NUMACTL_DESC NUMACTL_VERBOSE_CORES ALL_CPUS_ALTERNATING

