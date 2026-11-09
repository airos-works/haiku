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

KIT_SOURCES := $(KIT)/GraphData.cpp $(KIT)/TFLiteLoader.cpp \
	$(KIT)/Interpreter.cpp $(wildcard $(KIT)/ops/*.cpp)
# the speech recognizer's core: all of it but the class applications use
SPEECH_SOURCES := $(KIT)/SpeechAudio.cpp $(KIT)/SpeechEngine.cpp \
	$(KIT)/SpeechModel.cpp $(KIT)/SpeechWhisper.cpp
KIT_OBJECTS := $(patsubst %.cpp,$(OUT)/%.o,$(notdir $(KIT_SOURCES)))
SPEECH_OBJECTS := $(patsubst %.cpp,$(OUT)/%.o,$(notdir $(SPEECH_SOURCES)))
OBJECTS := $(KIT_OBJECTS) $(OUT)/neural_host.o
VPATH := $(KIT):$(KIT)/ops:.

all: $(OUT)/neural_host $(OUT)/speech_host

$(OUT)/%.o: %.cpp $(wildcard $(KIT)/ops/*.h) \
		$(wildcard $(TOP)/headers/private/neural/*.h) \
		$(wildcard $(TOP)/headers/os/neural/*.h) \
		$(wildcard $(TOP)/headers/os/add-ons/neural/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/neural_host: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

$(OUT)/speech_host: $(KIT_OBJECTS) $(SPEECH_OBJECTS) $(OUT)/speech_host.o
	$(CXX) $(CXXFLAGS) $^ -o $@ -lm

clean:
	rm -rf $(OUT)

.PHONY: all clean
