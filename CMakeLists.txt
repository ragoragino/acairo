CMAKE_MINIMUM_REQUIRED(VERSION 3.10)
PROJECT(cairo)

FIND_PACKAGE(Threads REQUIRED)

SET(CPP_STANDARD 20)

SET(CMAKE_CXX_FLAGS "-std=c++1z -Wall -Wextra -Werror -fcoroutines")

ADD_SUBDIRECTORY("third_party/logger")

ADD_LIBRARY(cairo STATIC "src/cairo.cpp" "src/acairo.cpp")

TARGET_LINK_LIBRARIES(cairo PRIVATE Threads::Threads)

TARGET_INCLUDE_DIRECTORIES(cairo 
    PUBLIC "include"
    PUBLIC "third_party/logger"
)

INSTALL(TARGETS cairo
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

ADD_EXECUTABLE(cairomain "src/main.cpp")

TARGET_LINK_LIBRARIES(cairomain 
    PRIVATE cairo
    PRIVATE logger
)

ADD_EXECUTABLE(acairomain "src/amain.cpp")

TARGET_LINK_LIBRARIES(acairomain 
    PRIVATE cairo
    PRIVATE logger
)