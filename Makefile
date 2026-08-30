CXX = g++
CXXFLAGS = -std=c++17 -O3 -Iinclude -I/usr/local/include -Wall -pthread
LDFLAGS = -L/usr/local/lib -lixwebsocket -lssl -lcrypto -lz -lpthread

SRCS = src/main.cpp src/miniaudio_impl.cpp src/web_server.cpp
OBJS = $(SRCS:src/%.cpp=build/%.o)
TARGET = cm5audio

all: build $(TARGET)

build:
	mkdir -p build

build/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf build $(TARGET)

.PHONY: all clean
