CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread -Iinclude

SRC = \
	src/server.cpp \
	src/avl.cpp \
	src/hashtable.cpp \
	src/heap.cpp \
	src/thread_pool.cpp \
	src/zset.cpp

OBJ = $(SRC:.cpp=.o)

TARGET     = bin/redislite
TEST_TARGET = bin/test_client

all: $(TARGET)

$(TARGET): $(OBJ)
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Build the test driver (standalone, no server objects needed)
$(TEST_TARGET): tests/test_client.cpp
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $(TEST_TARGET) tests/test_client.cpp

test: $(TEST_TARGET)
	./$(TEST_TARGET)

run: all
	./$(TARGET)

clean:
	rm -f src/*.o
	rm -rf bin

.PHONY: all run test clean
