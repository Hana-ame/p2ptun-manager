CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

.PHONY: all test clean

all: test

test:
	$(CXX) $(CXXFLAGS) src/core.cpp src/process_manager_win32.cpp tests/test_core.cpp -o build_test_core
	./build_test_core
	rm -f build_test_core

clean:
	rm -rf build build_test_core *.o *.exe
