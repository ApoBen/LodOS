cmake_minimum_required(VERSION 3.22.1)
project("lodos_native" C)

add_library(
    lodos_native
    SHARED
    lodos_native.c
)

find_library(log-lib log)

target_link_libraries(
    lodos_native
    ${log-lib}
)
