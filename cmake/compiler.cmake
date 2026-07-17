foreach(lang IN ITEMS C CXX)
	if(NOT DEFINED CMAKE_${lang}_COMPILER_ID)
		continue()
	endif()

	set(id "${CMAKE_${lang}_COMPILER_ID}")

	if(id STREQUAL "GNU")
		set(lto -flto=auto -ffat-lto-objects)
	elseif(id MATCHES "Clang$")
		set(lto -flto=full)
	else()
		continue()
	endif()

	set(CMAKE_${lang}_COMPILE_OPTIONS_IPO "${lto}")

	string(
		REGEX REPLACE
		"(^|[ \t])-Os([ \t]|$)"
		"\\1-Oz\\2"
		CMAKE_${lang}_FLAGS_MINSIZEREL_INIT
		"${CMAKE_${lang}_FLAGS_MINSIZEREL_INIT}"
	)
endforeach()
