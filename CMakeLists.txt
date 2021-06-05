CMAKE_MINIMUM_REQUIRED(VERSION 3.10)
PROJECT(cairo)

FIND_PACKAGE(Threads REQUIRED)

SET(CPP_STANDARD 20)

SET(GCC_COVERAGE_COMPILE_FLAGS "-fcoroutines")
SET(CMAKE_CXX_FLAGS "-std=c++1z -Wall -Wextra -Werror")

ADD_LIBRARY(cairo STATIC "src/cairo.cpp")

TARGET_LINK_LIBRARIES(cairo PRIVATE Threads::Threads)

TARGET_INCLUDE_DIRECTORIES(cairo 
    PUBLIC "include"
)

INSTALL(TARGETS cairo
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

ADD_EXECUTABLE(cairomain "src/main.cpp")

TARGET_LINK_LIBRARIES(cairomain 
    PRIVATE cairo
)

# https://thomastrapp.com/blog/signal-handler-for-multithreaded-c++/
# https://cafbit.com/post/tokio_internals/
# https://cor3ntin.github.io/posts/executors/
# https://www.modernescpp.com/index.php/a-short-detour-executors
