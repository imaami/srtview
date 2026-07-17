# The srt module is standard C++; this target deliberately links no
# Qt at all.
add_executable(srt_tests
	parse_test.cpp
	${CMAKE_SOURCE_DIR}/src/srt.cpp
)
target_include_directories(srt_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(srt_tests PRIVATE -Wall -Wextra)

add_test(NAME srt_tests COMMAND srt_tests)
