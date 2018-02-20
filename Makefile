all: build run

build:
	$(CXX) -std=c++11 -O3 -Wall -Wextra -g -pthread main.cpp -o lock-test

run: build
	./lock-test 100000 32 
