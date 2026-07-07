# Makefile

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread -Iinclude

SRC = 
src/14_server.cpp 
src/avl.cpp 
src/hashtable.cpp 
src/heap.cpp 
src/thread_pool.cpp 
src/zset.cpp

OBJ = $(SRC:.cpp=.o)

TARGET = bin/redislite

all: $(TARGET)

$(TARGET): $(OBJ)
mkdir -p bin
$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ)

src/%.o: src/%.cpp
$(CXX) $(CXXFLAGS) -c $< -o $@

run: all
./$(TARGET)

clean:
rm -f src/*.o
rm -rf bin

.PHONY: all run clean

