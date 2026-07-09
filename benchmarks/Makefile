CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread

TARGET = benchmark

all: $(TARGET)

$(TARGET): benchmark.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) benchmark.cpp

run: all
	@echo "Make sure ../bin/redislite is running on port 1234 first."
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
