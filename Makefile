CXX = g++
NLOHMANN_JSON_INCLUDE ?= ../json/include
CXXFLAGS = -std=c++17 -O3 -Iinclude -I/usr/local/include -I$(NLOHMANN_JSON_INCLUDE) -Wall -pthread
LDFLAGS = -L/usr/local/lib -lasound -lixwebsocket -lssl -lcrypto -lz -lpthread

SRCS = src/main.cpp src/miniaudio_impl.cpp src/web_server.cpp src/device_manager.cpp src/dsp_engine.cpp
OBJS = $(SRCS:src/%.cpp=build/%.o)
TARGET = cm5audio

CAPS_OBJ = build/alsa_caps.o
CAPS_TARGET = alsa_caps

TEST_OBJ = build/alsa_bridge_test.o
TEST_TARGET = alsa_bridge_test

all: build $(TARGET)

build:
		mkdir -p build

build/%.o: src/%.cpp
		$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
		$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Optional tool targets (only built when explicitly invoked)
alsatools: $(CAPS_TARGET) $(TEST_TARGET)

$(CAPS_TARGET): $(CAPS_OBJ)
		$(CXX) $(CAPS_OBJ) -o $@ $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJ)
		$(CXX) $(TEST_OBJ) -o $@ $(LDFLAGS)

clean:
		rm -rf $(OBJS) $(TARGET)

clean-alsatools:
		rm -rf $(CAPS_OBJ) $(TEST_OBJ) $(CAPS_TARGET) $(TEST_TARGET)

.PHONY: all clean clean-alsatools alsatools
