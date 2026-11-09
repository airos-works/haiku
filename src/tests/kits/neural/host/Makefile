# Builds the Neural Kit's graph, loader and CPU interpreter on the build
# host (Linux) for testing against a reference; see compare.py.
#
#	make -C src/tests/kits/neural/host OUT=/some/dir

TOP := ../../../../..
KIT := $(TOP)/src/kits/neural
OUT ?= /tmp/neural-host

CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-multichar \
	-Ishim -I$(TOP)/headers/os -I$(TOP)/headers/private/neural \
	-I$(KIT)/ops -pthread

SOURCES := $(KIT)/GraphData.cpp $(KIT)/TFLiteLoader.cpp \
	$(KIT)/Interpreter.cpp $(wildcard $(KIT)/ops/*.cpp) neural_host.cpp
OBJECTS := $(patsubst %.cpp,$(OUT)/%.o,$(notdir $(SOURCES)))
VPATH := $(KIT):$(KIT)/ops:.

all: $(OUT)/neural_host

$(OUT)/%.o: %.cpp $(wildcard $(KIT)/ops/*.h) \
		$(wildcard $(TOP)/headers/private/neural/*.h) \
		$(wildcard $(TOP)/headers/os/neural/*.h) \
		$(wildcard $(TOP)/headers/os/add-ons/neural/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/neural_host: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

clean:
	rm -rf $(OUT)

.PHONY: all clean
