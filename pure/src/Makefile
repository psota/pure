# This Makefile is responsible for building libpure. It can be 
# invoked by other makefiles to run tests.

SHELL := /bin/bash

export RUNTIME = Pure
export DEBUG ?= 1
export PROFILE ?= 0
export ENABLE_THREAD_LOGGER ?= 0
export TRACE_COMM ?= 0
export ASAN ?= 0
export MSAN ?= 0
export TSAN ?= 0
export UBSAN ?= 0
export MPIP ?= 0
export VALGRIND_MODE ?= 0

SHELL := /bin/bash

#CXXFLAGS = -Wall -std=c++11

INCLUDES = -I../include -I../include/pure/common 
SUPPORT_PATH = $(CPL)/support

include $(SUPPORT_PATH)/Makefile_includes/Makefile.build_setup.mk
include $(SUPPORT_PATH)/Makefile_includes/Makefile.libpure_path.mk

#############################################################################
# all cflags and cxxflags for the program must be defined above this point
# must be included in this order
include $(SUPPORT_PATH)/Makefile_includes/Makefile.sanitizer_opts.mk
VERBOSE_OBJ_DIR=1
include $(SUPPORT_PATH)/Makefile_includes/Makefile.misc.mk

JSON_HEADER_SOURCE_FILES = 3rd_party/jsoncpp/dist/jsoncpp.cpp 3rd_party/jsoncpp/dist/json/json.h

PURE_LIB_SOURCES = runtime/pure_process.cpp \
				   runtime/pure_thread.cpp \
				   runtime/pure_pipeline.cpp \
				   runtime/pure_comm.cpp \
				   runtime/pure.cpp \
				   runtime/mpi_comm_split_manager.cpp \
				   support/helpers.cpp \
				   transport/send_channel.cpp \
				   transport/recv_channel.cpp \
				   transport/bundle_channel_endpoint.cpp  \
				   transport/reduce_channel.cpp \
                   transport/reduce_process_channel.cpp \
                   transport/bcast_channel.cpp \
                   transport/bcast_process_channel.cpp \
				   transport/direct_mpi_channel.cpp \
				   transport/rdma_mpi_channel.cpp \
				   transport/all_reduce_channel.cpp \
				   transport/experimental/direct_channel_batcher.cpp \
				   transport/experimental/direct_channel_batcher_manual.cpp \
				   transport/experimental/send_flat_batcher.cpp \
				   transport/experimental/send_flat_batch_channel.cpp \
				   support/zed_debug.cpp \
                   transport/mpi_pure_helpers.cpp \
                   3rd_party/low-overhead-timers/low_overhead_timers.cpp \
				   support/stats_generator.cpp \
				   support/benchmark_timer.cpp \
#				   3rd_party/jsoncpp/dist/jsoncpp.cpp 


ifeq ($(FLAT_BATCHER_RECV_VERSION), 1)
	PURE_LIB_SOURCES += transport/experimental/v1/recv_flat_batcher_v1.cpp \
				        transport/experimental/v1/recv_flat_batch_channel_v1.cpp
endif

ifeq ($(FLAT_BATCHER_RECV_VERSION), 2)
	PURE_LIB_SOURCES += transport/experimental/v2/recv_flat_batcher_v2.cpp \
				        transport/experimental/v2/recv_flat_batch_channel_v2.cpp
endif


# add in extra debug sources
ifeq ($(DEBUG), 1)
PURE_LIB_SOURCES += transport/sender_channel_state.cpp \
                    transport/recv_channel_state.cpp \
                    support/thread_logger.cpp
endif


######################################################
# Special Process Channel Version source inclusion based on flags
# We could just include all of them, but this serves as an additional sanity check.
######################################################

# here we conditionally include different files depending on environment options
# for example, if PROCESS_CHANNEL_VERSION is 21, the major version is 2
ifeq ($(PROCESS_CHANNEL_MAJOR_VERSION),1)
PURE_LIB_SOURCES += transport/process_channel_v1.cpp transport/shared_queue_entry.cpp
endif
ifeq ($(PROCESS_CHANNEL_MAJOR_VERSION),2)
PURE_LIB_SOURCES += transport/process_channel_v2.cpp transport/shared_queue_entry.cpp
CXXFLAGS += -DPROCESS_CHANNEL_BUFFERED_MSG_SIZE=$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)
endif
ifeq ($(PROCESS_CHANNEL_MAJOR_VERSION),3)
PURE_LIB_SOURCES += transport/process_channel_v3.cpp
CXXFLAGS += -DPROCESS_CHANNEL_BUFFERED_MSG_SIZE=$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)
endif
ifeq ($(PROCESS_CHANNEL_MAJOR_VERSION),4)
PURE_LIB_SOURCES += transport/process_channel_v4.cpp
CXXFLAGS += -DPROCESS_CHANNEL_BUFFERED_MSG_SIZE=$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)
endif


ifeq ($(LINK_TYPE), dynamic)
$(warning DOING DYNAMIC LINKING Of libpure)
    CFLAGS   += -fPIC
    CXXFLAGS += -fPIC
endif

######################################################
LFLAGS += $(USER_LFLAGS) -L$(BOOST_LIB_DIR)
# build libjsoncpp using $(CPL)/support/misc/build_jsoncpp.sh
LIBS += -lpthread $(USER_LIBS) -ljsoncpp

# BUILD_DIR := $(CPL)/build
# BUILD_PREFIX := $(BUILD_DIR)/$(OBJ_DIR_PREFIX)
PURE_LIB_OBJS := $(patsubst %.cpp, $(BUILD_PREFIX)/%.cpp.o, $(PURE_LIB_SOURCES))


# Automatic dependency generation
# autodep article: http://make.mad-scientist.net/papers/advanced-auto-dependency-generation/#tldr
DEPDIR := $(BUILD_PREFIX)/.d

# TODO: keep this updated if you add other src subdirs
$(shell mkdir -p $(DEPDIR)/runtime   >/dev/null)
$(shell mkdir -p $(DEPDIR)/support   >/dev/null)
$(shell mkdir -p $(DEPDIR)/transport >/dev/null)
$(shell mkdir -p $(DEPDIR)/transport/experimental >/dev/null)
$(shell mkdir -p $(DEPDIR)/transport/experimental/v1 >/dev/null)
$(shell mkdir -p $(DEPDIR)/transport/experimental/v2 >/dev/null)
$(shell mkdir -p $(DEPDIR)/3rd_party/folly >/dev/null)
$(shell mkdir -p $(DEPDIR)/3rd_party/pthread >/dev/null)
$(shell mkdir -p $(DEPDIR)/3rd_party/low-overhead-timers >/dev/null)
$(shell mkdir -p $(DEPDIR)/3rd_party/jsoncpp/dist >/dev/null)

DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td

# via http://stackoverflow.com/questions/21985492/recursively-change-file-extensions-in-bash
#DEPENDENCY_POSTCOMPILE = find $(DEPDIR) -name "*.Td" -exec bash -c 'mv "$$1" "$${1%.Td}".d' - '{}' \;
#DEP_POSTCOMPILE = if [ -f $(DEPDIR)/$*.Td ]; then mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d && touch $@; fi
DEP_POSTCOMPILE = mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d || echo "Already moved it" && touch $@


#########################################################################################################

# these rules should be treated as always out of date
.PHONY: clean clean_all main vars analyze check-valgrind-thread-config config-completion .verify_obj_dir .verify_pcv_vars

main: $(JSON_HEADER_SOURCE_FILES) $(LIBPURE_FULL_PATH)
	@echo
	@echo "  using libpure: $(LIBPURE_FULL_PATH)"

$(BUILD_PREFIX)/%.cpp.o: %.cpp $(DEPDIR)/%.d
	mkdir -p $(shell dirname $@)
	$(CXX) $(DEPFLAGS) $(CXXFLAGS) $(INCLUDES) -c $<  -o $@
	$(DEP_POSTCOMPILE)

$(JSON_HEADER_SOURCE_FILES):
	$(warn Recreating amalgamated jsoncpp header files)
	(cd 3rd_party/jsoncpp; python amalgamate.py)

# N.B. specifying order-only prerequiesite for .verify_obj_dir so that this target only runs if $(PURE_LIB_OBJS) are new
# dynamic only supported on linux
$(LIBPURE_FULL_PATH): $(PURE_LIB_OBJS) | .verify_obj_dir .verify_pcv_vars
	mkdir -p $(shell dirname $(LIBPURE_FULL_PATH))
	@echo about to try to make libpure with link type $(LINK_TYPE)
ifeq ($(LINK_TYPE),static)
	rm -f $@
	$(AR) -cvrs $@ $(PURE_LIB_OBJS)
	@echo -e "$(KLTGREEN)                                         ⇨ ⇨ ⇨ ⇨     REBUILT STATIC $(LIBPURE_NAME)     ⇦ ⇦ ⇦ ⇦$(KRESET)"
else 
	$(CXX) -shared -Wl,-soname,$(LIBPURE_NAME) -o $@ $(PURE_LIB_OBJS) $(LFLAGS) $(LIBS)
	@echo -e "$(KRED)                                         ⇨ ⇨ ⇨ ⇨     REBUILT DYNAMIC $(LIBPURE_NAME)     ⇦ ⇦ ⇦ ⇦$(KRESET)"
endif

check-valgrind-thread-config:
	$(SUPPORT_PATH)/misc/check_valgrind_thread_config.rb $(PURE_LIB_SOURCES)

vars:
	@echo "INCLUDES": $(INCLUDES)
	@echo "PURE_LIB_SOURCES": $(PURE_LIB_SOURCES)
	@echo "PURE_LIB_OBJS": $(PURE_LIB_OBJS)
	@echo "OBJ_DIR_PREFIX: $(OBJ_DIR_PREFIX)"
	@echo "OS: $(OS)"
	@echo "INCLUDES: $(INCLUDES)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "LFLAGS: $(LFLAGS)"
	@echo "LIBS: $(LIBS)"
	@echo "MPI_ENABLED: $(MPI_ENABLED)
	@echo "PROCESS_CHANNEL_MAJOR_VERSION: $(PROCESS_CHANNEL_MAJOR_VERSION)"
	@echo "LIBPURE_FULL_PATH: $(LIBPURE_FULL_PATH)"

# See all warnings
analyze: CXXFLAGS += $(CXXFLAGS_FOR_ANALYZE)
analyze: clean_libpure main

tidy: SRCS_FOR_TIDY = $(shell echo $(PURE_LIB_SOURCES) | sed -e 's/3rd_party\/jsoncpp\/dist\/jsoncpp.cpp//g')
tidy:
	@echo "Tidying these sources (took out jsoncpp file): $(SRCS_FOR_TIDY)"
	$(SUPPORT_PATH)/misc/clang_tidy_many.rb $(SRCS_FOR_TIDY)
	@echo "See clang-tidy output in $(TIDY_LOG) in src dir"

tidy_howto:
	@echo "clang-tidy -header-filter=../include/pure <srcs> -- $(CXXFLAGS) $(INCLUDES) > $(TIDY_LOG)"
	@echo "Or better yet use support/misc/clang_tidy_many.rb"

clean:
	@echo -e "$$KYEL""Cleaning configuration $(OBJ_DIR_PREFIX) $$KRESET"
	$(RM_RF) $(BUILD_PREFIX) 
	$(RM_RF) $(LIB_PREFIX)	

clean_all:
	$(RM_RF) $(BUILD_DIR)/*
	$(RM_RF) $(LIB_DIR)/*

bloaty: BLOATY_OPTS = -n 30 -s vm -w
bloaty: $(LIBPURE_FULL_PATH)
	@echo Profiling the libpure binary...
	bloaty $(BLOATY_OPTS) $(LIBPURE_FULL_PATH)
ifeq ($(LINK_TYPE), static)
	@echo LINK_TYPE is currently $(LINK_TYPE) so not showing compile units. Change to dynamic if you want to see the object file profile.
else
	@echo
	bloaty $(BLOATY_OPTS) $(LIBPURE_FULL_PATH) -d compileunits
endif
	@echo


docs:
	doxygen Doxyfile
	@echo See docs stored in doc/doxygen directory

docs_web: docs
	scp -r $(CPL)/doc/doxygen/html draco1.csail.mit.edu:/afs/csail.mit.edu/u/j/jim/public_html/pure/doxygen
	@echo Docs are at http://people.csail.mit.edu/jim/pure/doxygen/html/

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d

-include $(patsubst %,$(DEPDIR)/%.d,$(basename $(PURE_LIB_SOURCES)))	