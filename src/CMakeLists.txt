add_executable(srtview
	main.cpp
	mainwin.cpp
	srtedit.cpp
	searchbar.cpp
	mpvlink.cpp
	srt.cpp
	discovery.cpp
	palettefix.cpp
	selftest.cpp
)
target_link_libraries(srtview PRIVATE Qt6::Widgets Qt6::Network)
target_compile_options(srtview PRIVATE -Wall -Wextra)

install(TARGETS srtview RUNTIME DESTINATION bin)
