CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
SRC = src/main.cpp src/loader.cpp src/selector.cpp src/agent.cpp src/dataset.cpp src/experiments.cpp src/tests.cpp src/menu.cpp

cbm: $(SRC) src/*.h
	$(CXX) $(CXXFLAGS) -o cbm $(SRC)

all: cbm
	./cbm gen
	./cbm validate
	./cbm test
	./cbm edge
	./cbm eval
	./cbm bench

.PHONY: all
