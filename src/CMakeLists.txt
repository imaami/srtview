add_executable(srtview
	main.cpp
	mainwin.cpp
	srtedit.cpp
	searchbar.cpp
	playback.cpp
	prefs.cpp
	trail.cpp
	grabber.cpp
	decoder.cpp
	exporter.cpp
	fundo.c
	knowledge.cpp
	lector.cpp
	list.c
	llm.c
	loop.c
	ocr.cpp
	agenda.cpp
	facts.cpp
	search.cpp
	semantic.cpp
	mpvclient.cpp
	mpvlink.cpp
	srt.cpp
	topics.cpp
	vault.cpp
	discovery.cpp
	palettefix.cpp
	selftest.cpp
)
target_link_libraries(srtview PRIVATE Qt6::Widgets Qt6::Network
                      PkgConfig::FFMPEG PkgConfig::TESSERACT
                      CURL::libcurl Threads::Threads)
target_compile_options(srtview PRIVATE -Wall -Wextra)

install(TARGETS srtview RUNTIME DESTINATION bin)

# The detached OCR pipeline proof: scribe<lector> driven from the
# command line.  Deliberately not part of srtview yet -- phase 1
# builds and evaluates the modular pieces (see ocr.hpp).
add_executable(ocrview
	decoder.cpp
	lector.cpp
	ocr.cpp
	ocrview.cpp
)
target_link_libraries(ocrview PRIVATE PkgConfig::FFMPEG
                      PkgConfig::TESSERACT Threads::Threads)
target_compile_options(ocrview PRIVATE -Wall -Wextra)
