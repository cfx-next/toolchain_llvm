##===- tools/llvm-config/Makefile---------------------------*- Makefile -*-===##
# 
#                     The LLVM Compiler Infrastructure
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
# 
##===----------------------------------------------------------------------===##

LEVEL := ../..
TOOLNAME := llvm-config
USEDLIBS := LLVMSupport.a

# We generate sources in the build directory, make sure it is in the include
# paths.
INCLUDE_BUILD_DIR := 1

# This tool has no plugins, optimize startup time.
TOOL_NO_EXPORTS := 1

# Note that we have to use lazy expansion here.
BUILDVARIABLES_SRCPATH = $(PROJ_SRC_ROOT)/tools/$(TOOLNAME)/BuildVariables.inc.in
BUILDVARIABLES_OBJPATH = $(ObjDir)/BuildVariables.inc
BUILT_SOURCES = $(BUILDVARIABLES_OBJPATH)

include $(LEVEL)/Makefile.common

# Combine preprocessor flags (except for -I) and CXX flags.
SUB_CPPFLAGS := ${CPP.BaseFlags}
SUB_CFLAGS   := ${CPP.BaseFlags} ${C.Flags}
SUB_CXXFLAGS := ${CPP.BaseFlags} ${CXX.Flags}

# This is blank for now.  We need to be careful about adding stuff here:
# LDFLAGS tend not to be portable, and we don't currently require the
# user to use libtool when linking against LLVM.
SUB_LDFLAGS :=

$(ObjDir)/BuildVariables.inc: $(BUILDVARIABLES_SRCPATH) Makefile Makefile.buildvars $(ObjDir)/.dir
	$(Echo) "Making llvm-config $@ file."
	$(MAKE) -f Makefile.buildvars CROSS_DEPTH=$(CROSS_DEPTH)

# When cross-compiling, install a version of llvm-config that runs on the host.
ifeq ($(LLVM_CROSS_COMPILING),1)
install:: $(DESTDIR)$(PROJ_bindir)
	$(Echo) Installing llvm-config-host
	$(Verb) $(ProgInstall) $(BuildLLVMToolDir)/llvm-config \
	  $(DESTDIR)$(PROJ_bindir)/$(program_prefix)llvm-config-build
	$(Echo) Installing llvm-config-host
	$(Verb) $(ProgInstall) $(BuildLLVMCrossToolDir)/llvm-config \
		$(DESTDIR)$(PROJ_bindir)/$(program_prefix)llvm-config-host
endif
