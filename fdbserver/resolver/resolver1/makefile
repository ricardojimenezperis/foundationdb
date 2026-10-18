CXX := g++
CPPFLAGS := -I.
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -g

TARGET := tests.exe
SOURCES := pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp $(wildcard tests/*.cpp)
HEADERS := $(wildcard *.h) $(wildcard tests/*.h)

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $@

test: $(TARGET)
	./$(TARGET)

clean:
	$(RM) $(TARGET)