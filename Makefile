# Plain Makefile so the project builds without CMake. C++17 is the minimum:
# std::variant, std::optional, std::from_chars, structured bindings.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow
SRC      := matching_engine.cpp cli.cpp

me: $(SRC) matching_engine.hpp
	$(CXX) $(CXXFLAGS) -Iinclude $(SRC) -o $@

clean:
	rm -f me 

.PHONY: test clean
