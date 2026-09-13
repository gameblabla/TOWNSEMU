# I am sick and tired of ld.  ld should learn two-pass link or be burnt in the hellfire.



if(MSVC)
	set(DUMMY_LIBRARY_NAME yssimplesound_dsound)
	set(SUBDIR windows_dsound)
	set(SRCS ${SUBDIR}/yssimplesound_dsound.cpp)
	set(LIBS "")
elseif(APPLE)
	find_library(AVFOUNDATION_LIB AVFoundation)
	mark_as_advanced(AVFOUNDATION_LIB)

	set(DUMMY_LIBRARY_NAME yssimplesound_macosx)
	set(SUBDIR macosx)
	set(SRCS ${SUBDIR}/yssimplesound_macosx_cpp.cpp ${SUBDIR}/yssimplesound_macosx_objc.m)
	set(LIBS ${AVFOUNDATION_LIB})
elseif(UNIX)
	# SDL3 is preferred here over the raw ALSA backend: ALSA's draining
	# is only ever driven synchronously by this emulator's single VM
	# thread (see linux_alsa/yssimplesound_linux_alsa.cpp's own
	# comments), through what was originally a hard 2-segment buffer
	# that silently dropped audio the instant both segments were full -
	# a real, reproduced problem for TownsSound's continuous multi-
	# second FM_PCM/DAC streaming. SDL3's audio streams are drained by
	# SDL's own dedicated thread at the real device rate, independent of
	# this emulator's call cadence, which fixes that structurally rather
	# than by further tuning buffer sizes.
	# Miniaudio (option YSSIMPLESOUND_USE_MINIAUDIO, default off) is an
	# alternative full-duplex backend used by the Qt front end.
	option(YSSIMPLESOUND_USE_MINIAUDIO "Use miniaudio for Linux audio" OFF)
	if(YSSIMPLESOUND_USE_MINIAUDIO)
		set(DUMMY_LIBRARY_NAME yssimplesound_linux_miniaudio)
		set(SUBDIR linux_miniaudio)
		set(SRCS ${SUBDIR}/yssimplesound_linux_miniaudio.cpp)
		set(LIBS pthread dl m)
	elseif(TRUE)
		find_package(SDL3 QUIET)
		if(TARGET SDL3::SDL3)
			set(DUMMY_LIBRARY_NAME yssimplesound_sdl3)
			set(SUBDIR sdl3)
			set(LIBS SDL3::SDL3)
			set(SRCS ${SUBDIR}/yssimplesound_sdl3.cpp)
		else()
			message("SDL3 not found - falling back to ALSA.")
			find_library(ALSALIB NAMES asound)
			set(DUMMY_LIBRARY_NAME yssimplesound_linux_alsa)
			set(SUBDIR linux_alsa)
			if(NOT "${ALSALIB}" STREQUAL "ALSALIB-NOTFOUND")
				set(LIBS ${ALSALIB})
				set(SRCS ${SUBDIR}/yssimplesound_linux_alsa.cpp)
			else()
				message("ALSA library not found.")
				message("No sound will be played with yssimplesound library.")
				set(LIBS)
				set(SRCS nownd/yssimplesound_nownd.cpp)
			endif()
		endif()
	endif()
endif()



set(TARGET_NAME yssimplesound)
add_library(${TARGET_NAME} yssimplesound.cpp yssimplesound.h ${SRCS})
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET_NAME} PRIVATE ${SUBDIR})
if(UNIX AND YSSIMPLESOUND_USE_MINIAUDIO)
	target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/miniaudio)
endif()
target_link_libraries(${TARGET_NAME} ${LIBS})

if(APPLE)
	set_property(TARGET ${TARGET_NAME} APPEND_STRING PROPERTY COMPILE_FLAGS "-fobjc-arc")
endif()

add_library(yssimplesound_platform ALIAS ${TARGET_NAME})



add_library(yssimplesound_nownd yssimplesound.cpp yssimplesound.h nownd/yssimplesound_nownd.cpp)
target_include_directories(yssimplesound_nownd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(unittest)
add_subdirectory(playwav)


set(YS_ALL_BATCH_TEST ${YS_ALL_BATCH_TEST} PARENT_SCOPE)

# For legacy build.
add_library(${DUMMY_LIBRARY_NAME} empty.cpp)
