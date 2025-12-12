CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -I../include -I../tests/include -DUNIT_TEST

TARGET := romm_tests
SOURCES := ../source/api.cpp \
           test_api.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

.PHONY: clean
clean:
	rm -f $(TARGET)
