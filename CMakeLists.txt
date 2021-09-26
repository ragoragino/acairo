CMAKE_MINIMUM_REQUIRED(VERSION 3.10)
PROJECT(cairo)

SET(CPP_STANDARD 20)

SET(CMAKE_CXX_FLAGS "-std=c++1z -Wall -Wextra -Werror -fcoroutines")

FIND_PACKAGE(Threads REQUIRED)
FIND_PACKAGE(ragolibcpp REQUIRED)

INCLUDE_DIRECTORIES(${ragolibcpp_INCLUDE_DIRS})

ADD_LIBRARY(acairo STATIC "src/acairo.cpp")

TARGET_LINK_LIBRARIES(acairo 
    PUBLIC Threads::Threads
    PUBLIC ragolibcpp
)

TARGET_INCLUDE_DIRECTORIES(acairo 
    PUBLIC "include"
)

INSTALL(TARGETS acairo
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

ADD_EXECUTABLE(acairomain "src/amain.cpp")

TARGET_LINK_LIBRARIES(acairomain 
    PRIVATE acairo
    PRIVATE ragolibcpp
)

# TODO: 
# move to separate files
# flag: -ggdb3 and command: valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --log-file=valgrind.txt ./acairomain