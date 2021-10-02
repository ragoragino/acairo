An implementation of a TCP server using epoll and coroutines.

#### Prerequisites
A C++ compiler supporting C++20 (primarily coroutines).
Also, one must install this project containing custom logger implementation: https://github.com/ragoragino/ragolib/tree/master/cpp

#### Valgrind
Running Valgrind:
Add -ggdb3 flag for compilation, compile and execute: "valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=acairomain.txt ./acairomain"