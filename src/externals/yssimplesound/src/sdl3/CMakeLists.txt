set(TARGET_NAME yssimplesound_sdl3)



find_package(SDL3 QUIET)
if(TARGET SDL3::SDL3)
  set(LIBS SDL3::SDL3)
  set(SRCS yssimplesound_sdl3.cpp)
else()
  message("SDL3 not found.")
  message("No sound will be played with yssimplesound library.")
  set(LIBS)
  set(SRCS ../nownd/yssimplesound_nownd.cpp)
endif()



add_library(${TARGET_NAME} ${SRCS})
target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(${TARGET_NAME} yssimplesound ${LIBS})

add_library(yssimplesound_platform ALIAS ${TARGET_NAME})
