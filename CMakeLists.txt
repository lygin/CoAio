project(CoAio)
cmake_minimum_required(VERSION 3.16)
enable_language(ASM)
set(CMAKE_BUILD_TYPE Debug)
aux_source_directory(. SRCS)

FILE(GLOB_RECURSE ASM_SRC *.S)


list(APPEND SRCS ${ASM_SRC})

add_library(libcoro ${SRCS})

add_executable(coro_test coro_test.cc)
target_link_libraries(coro_test libcoro pthread aio)