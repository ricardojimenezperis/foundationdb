CXX := g++
CPPFLAGS := -I.
# make BLOCK_SIZE=4096 builds with 4 KiB blocks; 2 KiB by default.
ifdef BLOCK_SIZE
CPPFLAGS += -DRESOLVER1_BLOCK_SIZE=$(BLOCK_SIZE)
endif
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