# Host unit tests for the Multi House code (no hardware, no pico-sdk): `make -C tests`.
# Every tests/test_*.cpp is a program; it links all src/mh/*.cpp that avoid pico headers
# through include/mh/platform.h. Add -DMH_HOST_TEST sources only when they are host safe.
CXX ?= c++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -I../include -I..
SRC := $(filter-out %_pico.cpp,$(wildcard ../src/mh/*.cpp))
TESTS := $(patsubst %.cpp,%,$(wildcard test_*.cpp))

all: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

test_%: test_%.cpp $(SRC) $(wildcard ../include/mh/*.h) check.h
	$(CXX) $(CXXFLAGS) $< $(SRC) -o $@

clean:
	rm -f $(TESTS)
.PHONY: all clean
