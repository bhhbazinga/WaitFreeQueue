CXX = clang++
CXXFLAGS = -Wall -Wextra -pedantic -std=c++2a -g -o3
#-fsanitize=thread
#-fsanitize=address -fsanitize=leak
#-fsanitize=thread

SRC = test.cc
EXEC = test

all: $(EXEC)

$(EXEC): test.cc waitfree_queue.h HazardPointer/reclaimer.h
	$(CXX) $(CXXFLAGS) -o $(EXEC) test.cc

HazardPointer/reclaimer.h:
	git submodule update --init --recursive --remote

clean:
	rm -rf *.o $(EXEC)