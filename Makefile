#
# This Makefile is provided purely as a convenience for running CMake
#

QUIET		:= @

all: build/Makefile
	$(QUIET)cmake --build build

%:: build/Makefile
	$(QUIET)cmake --build build --target $(@)

build/Makefile:
	$(QUIET)[ -d build ] || mkdir build
	$(QUIET)cd build && cmake ..

distclean:
	$(QUIET)rm -rf build
