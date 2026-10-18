CXX = g++
NLOHMANN_JSON_INCLUDE ?= ../json/include
CXXFLAGS = -std=c++17 -O3 -Iinclude -I/usr/local/include -I$(NLOHMANN_JSON_INCLUDE) -Wall -pthread
LDFLAGS = -L/usr/local/lib -lasound -lixwebsocket -lssl -lcrypto -lz -lpthread

# Separate fast-changing app sources from rarely-changed miniaudio
APP_SRCS = src/main.cpp src/web_server.cpp src/device_manager.cpp src/dsp_engine.cpp
APP_OBJS = $(APP_SRCS:src/%.cpp=build/%.o)

MINIAUDIO_SRC = src/miniaudio_impl.cpp
MINIAUDIO_OBJ = build/miniaudio_impl.o

OBJS = $(APP_OBJS) $(MINIAUDIO_OBJ)
TARGET = cm5audio

CAPS_OBJ = build/alsa_caps.o
CAPS_TARGET = alsa_caps

TEST_OBJ = build/alsa_bridge_test.o
TEST_TARGET = alsa_bridge_test

PLUGIN_TEST_OBJ = build/dsp_plugins_test.o
PLUGIN_TEST_TARGET = build/dsp_plugins_test
PLUGIN_TEST_DEPS = $(filter-out build/main.o,$(APP_OBJS)) $(MINIAUDIO_OBJ)

all: build $(TARGET)

build:
	mkdir -p build

# Track headers so editing/registering a C++ processor rebuilds every affected object.
build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Optional tool targets (only built when explicitly invoked)
alsatools: $(CAPS_TARGET) $(TEST_TARGET)

$(CAPS_TARGET): $(CAPS_OBJ)
	$(CXX) $(CAPS_OBJ) -o $@ $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJ)
	$(CXX) $(TEST_OBJ) -o $@ $(LDFLAGS)

$(PLUGIN_TEST_OBJ): tests/dsp_plugins_test.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(PLUGIN_TEST_TARGET): $(PLUGIN_TEST_OBJ) $(PLUGIN_TEST_DEPS)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Starts a test-only server on 28182/28183; never activates physical audio devices.
test-plugins: $(PLUGIN_TEST_TARGET)
	./$(PLUGIN_TEST_TARGET)
	node tests/dashboard_plugins_test.js

clean:
	rm -f $(APP_OBJS) $(APP_OBJS:.o=.d) $(TARGET) $(PLUGIN_TEST_OBJ) $(PLUGIN_TEST_OBJ:.o=.d) $(PLUGIN_TEST_TARGET)

clean-miniaudio:
	rm -f $(MINIAUDIO_OBJ) $(MINIAUDIO_OBJ:.o=.d)

clean-alsatools:
	rm -f $(CAPS_OBJ) $(CAPS_OBJ:.o=.d) $(TEST_OBJ) $(TEST_OBJ:.o=.d) $(CAPS_TARGET) $(TEST_TARGET)

-include $(wildcard build/*.d)

.PHONY: all clean clean-miniaudio clean-alsatools alsatools test-plugins
