cmake_minimum_required(VERSION 3.22)
project(srtview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# without this the executable would land at build/src/srtview.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_subdirectory(src)
