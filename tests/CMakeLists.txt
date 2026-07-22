# The srt module is standard C++; this target deliberately links no
# Qt at all.
add_executable(srt_tests
	parse_test.cpp
	${CMAKE_SOURCE_DIR}/src/srt.cpp
)
target_include_directories(srt_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(srt_tests PRIVATE -Wall -Wextra)

add_test(NAME srt_tests COMMAND srt_tests)

# The topics module is standard C++; no Qt here either.
add_executable(topics_tests
	topics_test.cpp
	${CMAKE_SOURCE_DIR}/src/topics.cpp
)
target_include_directories(topics_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(topics_tests PRIVATE -Wall -Wextra)

add_test(NAME topics_tests COMMAND topics_tests)

# The fundo undo-tree core is plain C; its test links no C++ either.
add_executable(fundo_tests
	fundo_test.c
	${CMAKE_SOURCE_DIR}/src/fundo.c
	${CMAKE_SOURCE_DIR}/src/list.c
)
target_include_directories(fundo_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(fundo_tests PRIVATE -Wall -Wextra)

add_test(NAME fundo_tests COMMAND fundo_tests)

# The llm client is plain C (POSIX sockets + pthread); its test
# drives it against a fake in-process HTTP server, no C++ involved.
add_executable(llm_tests
	llm_test.c
	${CMAKE_SOURCE_DIR}/src/llm.c
	${CMAKE_SOURCE_DIR}/src/list.c
)
target_include_directories(llm_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(llm_tests PRIVATE -Wall -Wextra)
target_link_libraries(llm_tests PRIVATE Threads::Threads)

add_test(NAME llm_tests COMMAND llm_tests)
