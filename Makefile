CXX      = g++
CXXFLAGS = -O2 -Wall -std=c++17 -I../miniaudio -I../cpp-httplib
LDFLAGS  = -lpthread -lm -ldl -lasound

TARGET   = cm5audio
SRCS     = main.cpp miniaudio_impl.cpp web_server.cpp
OBJS     = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
