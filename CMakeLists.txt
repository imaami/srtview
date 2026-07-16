cmake_minimum_required(VERSION 3.22)
project(srtview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(srtview srtview.cpp)
target_link_libraries(srtview PRIVATE Qt6::Widgets Qt6::Network)
target_compile_options(srtview PRIVATE -Wall -Wextra)

install(TARGETS srtview RUNTIME DESTINATION bin)
