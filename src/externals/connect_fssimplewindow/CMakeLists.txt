set(TARGET_NAME fssimplewindow_connection)

add_library(${TARGET_NAME} fssimplewindow_connection.h fssimplewindow_connection.cpp)
target_link_libraries(${TARGET_NAME} outside_world fssimplewindow yssimplesound cpputil ysgamepad)
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
	target_link_libraries(${TARGET_NAME} yssimplesound_dsound)
elseif(APPLE)
	find_library(COCOA_LIB Cocoa)
	mark_as_advanced(COCOA_LIB)
	target_link_libraries(${TARGET_NAME} yssimplesound_macosx ${COCOA_LIB})
elseif(UNIX)
	# yssimplesound_platform is the alias the top-level yssimplesound
	# CMakeLists.txt points at whichever backend it actually selected
	# (SDL3, falling back to ALSA, falling back to nownd - see that
	# file) - link against that instead of hardcoding one backend's
	# target name, so this stays correct regardless of which backend
	# got picked.
	target_link_libraries(${TARGET_NAME} yssimplesound_platform)
endif()
