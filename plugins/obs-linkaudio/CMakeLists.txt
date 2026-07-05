cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_LINK_AUDIO "Enable building OBS with Ableton Link Audio support" ON)

if(NOT ENABLE_LINK_AUDIO)
  target_disable(obs-linkaudio)
  return()
endif()

set(LINK_AUDIO_ROOT "${CMAKE_SOURCE_DIR}/deps/ofxAbletonLinkAudio" CACHE PATH "Path to ofxAbletonLinkAudio")
set(ABLETON_LINK_ROOT "${LINK_AUDIO_ROOT}/libs/link")

if(NOT EXISTS "${LINK_AUDIO_ROOT}/libs/core/LinkAudioManager.h"
   OR NOT EXISTS "${ABLETON_LINK_ROOT}/include/ableton/LinkAudio.hpp"
   OR NOT EXISTS "${ABLETON_LINK_ROOT}/modules/asio-standalone/asio/include/asio.hpp"
)
  message(WARNING "ofxAbletonLinkAudio submodule is not available. Building without obs-linkaudio.")
  target_disable(obs-linkaudio)
  return()
endif()

add_library(obs-linkaudio MODULE)
add_library(OBS::linkaudio ALIAS obs-linkaudio)

target_sources(
  obs-linkaudio
  PRIVATE
    "${LINK_AUDIO_ROOT}/libs/core/LinkAudioManager.cpp"
    link-audio-source.cpp
    plugin-main.cpp
)

target_include_directories(
  obs-linkaudio
  PRIVATE
    "${LINK_AUDIO_ROOT}/libs/core"
)

# Mark Ableton Link and ASIO headers as SYSTEM includes to suppress warnings from
# third-party code (e.g. multi-character character constants, unused parameters).
target_include_directories(
  obs-linkaudio
  SYSTEM PRIVATE
    "${ABLETON_LINK_ROOT}/include"
    "${ABLETON_LINK_ROOT}/modules/asio-standalone/asio/include"
)

target_compile_features(obs-linkaudio PRIVATE cxx_std_17)
target_compile_definitions(obs-linkaudio PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)

if(OS_WINDOWS)
  target_compile_definitions(
    obs-linkaudio
    PRIVATE LINK_PLATFORM_WINDOWS=1 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX
  )
elseif(OS_MACOS)
  target_compile_definitions(obs-linkaudio PRIVATE LINK_PLATFORM_MACOSX=1)
elseif(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  target_compile_definitions(obs-linkaudio PRIVATE LINK_PLATFORM_LINUX=1)
endif()

target_link_libraries(
  obs-linkaudio
  PRIVATE
    OBS::libobs
    "$<$<PLATFORM_ID:Windows>:ws2_32>"
    "$<$<PLATFORM_ID:Windows>:iphlpapi>"
    "$<$<PLATFORM_ID:Windows>:winmm>"
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>>"
)

if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  find_package(Threads REQUIRED)
  target_link_libraries(obs-linkaudio PRIVATE Threads::Threads)
endif()

set_target_properties_obs(obs-linkaudio PROPERTIES FOLDER plugins PREFIX "")
