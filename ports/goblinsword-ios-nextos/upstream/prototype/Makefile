.DEFAULT_GOAL := all
# Choose the target toolchain explicitly; GNU make's built-in CXX=g++ is a
# host compiler and must never silently build this AArch64-only executable.
TOOLCHAIN ?=
ifneq ($(filter default undefined,$(origin CXX)),)
ifeq ($(strip $(TOOLCHAIN)),)
$(error Set TOOLCHAIN=/path/to/NextOS/toolchain or CXX=/path/to/aarch64-cross-g++)
endif
CXX := $(TOOLCHAIN)/bin/aarch64-libreelec-linux-gnu-g++
endif
COMPILER_SYSROOT := $(shell $(CXX) -print-sysroot)
ifneq ($(strip $(SYSROOT)),)
ifneq ($(strip $(SYSROOT)),$(strip $(COMPILER_SYSROOT)))
$(error SYSROOT must match the selected compiler's -print-sysroot; select a compiler configured for the intended target)
endif
endif
override SYSROOT := $(COMPILER_SYSROOT)
CPPFLAGS := -I$(SYSROOT)/usr/include/SDL2 -I$(SYSROOT)/usr/include/libpng16 -I$(SYSROOT)/usr/include/freetype2
CXXFLAGS := -std=c++17 -O0 -g -fno-omit-frame-pointer -Wall -Wextra -MMD -MP
LDLIBS := -Wl,--export-dynamic -ldl -lpthread -lm -lz -lpng16 -lfreetype -lSDL2 -lavformat -lavcodec -lavutil -lswresample
SOURCES := main.cpp macho_loader.cpp runtime.cpp foundation.cpp graphics.cpp services.cpp audio.cpp darwin_libc.cpp idle.cpp
ASSEMBLY := runtime-arm64.S foundation-arm64.S
OBJECTS := $(SOURCES:%.cpp=build/%.o) $(ASSEMBLY:%.S=build/%.o)

# Optimize the measured hot bridge paths; retain full debug and frame pointers.
# Guest layouts are read through explicit typed views, with strict aliasing off.
build/runtime.o build/graphics.o: CXXFLAGS := $(filter-out -O0,$(CXXFLAGS)) -O2 -fno-strict-aliasing

.PHONY: all
all: build/goblinsword-ios-nextos

build/goblinsword-ios-nextos: $(OBJECTS) Makefile
	$(CXX) $(OBJECTS) $(LDLIBS) -o $@

build/%.o: %.cpp Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/%.o: %.S
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -g -c $< -o $@

-include $(OBJECTS:.o=.d)
