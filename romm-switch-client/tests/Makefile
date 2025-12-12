CXX ?= g++
CXX_OK := $(shell which $(CXX) 2>/dev/null)
ifeq ($(CXX_OK),)
# Fallback to clang++ if g++ is missing
CXX := clang++
CXX_OK := $(shell which $(CXX) 2>/dev/null)
endif
ifeq ($(CXX_OK),)
# Build can still proceed if the user sets CXX manually on the command line.
$(warning No host C++ compiler found in PATH. Set CXX=/path/to/compiler or install g++/clang++ (e.g., pacman -S mingw-w64-x86_64-gcc on MSYS2))
endif
CXXFLAGS ?= -std=c++17 -Wall -Wextra -I../include -I../tests/include -DUNIT_TEST

TARGET := romm_tests
SOURCES := ../source/api.cpp \
           test_api_catch.cpp \
           logger_stub.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

.PHONY: clean
clean:
	rm -f $(TARGET)
