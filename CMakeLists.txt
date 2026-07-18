cmake_minimum_required(VERSION 3.24)

# needs to be simplified
set(
	CMAKE_USER_MAKE_RULES_OVERRIDE
	"${CMAKE_CURRENT_LIST_DIR}/cmake/compiler.cmake"
)

project(srtview LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CheckIPOSupported)
check_ipo_supported(
	RESULT lto_ok
	OUTPUT no_lto
	LANGUAGES C CXX
)
if(lto_ok)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
else()
	message(STATUS "Link-time optimization disabled: ${no_lto}")
endif()

# prevent the executable from being output to build/src/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

include(CTest)

add_subdirectory(src)
if(BUILD_TESTING)
	add_subdirectory(tests)
endif()
