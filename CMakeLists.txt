cmake_minimum_required(VERSION 3.13)
project(assignment_4 C)

set(CMAKE_C_STANDARD 11)

include_directories(.)
include_directories(/usr/local/include/osxfuse)

add_executable(assignment_4
        blkdev.h
        fsx600.h
        homework.c
        image.c
        image.h
        misc.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_FILE_OFFSET_BITS=64")

target_link_libraries(assignment_4 "/usr/local/lib/libosxfuse_i64.dylib")