# Third-party code is compiled without our warning policy (not ours to fix).

add_library(miniaudio STATIC miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio)

find_package(Threads REQUIRED)
target_link_libraries(miniaudio PUBLIC Threads::Threads)

if(ANDROID)
  # miniaudio runtime-links AAudio/OpenSL|ES; dl, m, and the NDK log lib suffice at link
  # time (build plan targets AAudio — minSdk 26+).
  target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} m log)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # miniaudio dlopens ALSA/PulseAudio at runtime; only dl and m are needed at link time.
  target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} m)
elseif(APPLE)
  target_link_libraries(miniaudio PUBLIC
    "-framework CoreFoundation"
    "-framework CoreAudio"
    "-framework AudioUnit"
    "-framework AudioToolbox")
endif()
