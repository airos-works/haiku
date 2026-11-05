# Builds the emulator tests on the build host:  make -C src/tests/apps/terminal
EMULATOR = ../../../apps/terminal/emulator
SOURCES = $(wildcard $(EMULATOR)/*.cpp) EmulatorTest.cpp
CXXFLAGS = -std=c++17 -O1 -g -Wall -Wno-unused-function -Ishim -I$(EMULATOR) \
	-fsanitize=address,undefined
OUT ?= .

$(OUT)/emulator_test: $(SOURCES) $(wildcard $(EMULATOR)/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) -lz -lpthread

test: $(OUT)/emulator_test
	$(OUT)/emulator_test

# runs a program with the emulator answering it, see TermRun.cpp
$(OUT)/termrun: $(wildcard $(EMULATOR)/*.cpp) TermRun.cpp $(wildcard $(EMULATOR)/*.h)
	$(CXX) $(CXXFLAGS) -o $@ $(wildcard $(EMULATOR)/*.cpp) TermRun.cpp -lz -lpthread -lutil
