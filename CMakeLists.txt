cmake_minimum_required(VERSION 2.8)
project(webserver-libev-httpparser)

#set(CMAKE_C_FLAGS "-g -Wall -std=c99")
set(CMAKE_C_FLAGS "-ggdb -O0 -Wall -std=c99")
#set(CMAKE_VERBOSE_MAKEFILE on)
set(EXECUTABLE_OUTPUT_PATH bin)
set(LIBRARY_OUTPUT_PATH lib)

include_directories(include src test)
set(OBJS
    src/http_parser.c
    src/server_eh.c)

add_library(server_eh SHARED ${OBJS})
add_executable(testsrv test/main.c)

target_link_libraries(testsrv server_eh ev)

