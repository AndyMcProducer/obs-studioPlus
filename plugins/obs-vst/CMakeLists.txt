cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_VST "Enable building OBS with VST plugin" ON)
option(ENABLE_VST3 "Enable VST3 plug-in hosting in the OBS VST filter" ON)

if(NOT ENABLE_VST)
  target_disable(obs-vst)
  return()
endif()

add_library(obs-vst MODULE)
add_library(OBS::vst ALIAS obs-vst)

find_package(Qt6 REQUIRED Widgets)

set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/deps/vst3sdk" CACHE PATH "Path to the Steinberg VST3 SDK")

if(ENABLE_VST3)
  if(NOT EXISTS "${VST3_SDK_ROOT}/pluginterfaces/vst/ivstaudioprocessor.h")
    message(WARNING "VST3 SDK was not found at '${VST3_SDK_ROOT}'. Building obs-vst without VST3 support.")
    set(ENABLE_VST3 OFF)
  endif()
endif()

if(ENABLE_VST3)
  add_library(
    obs-vst3-sdk STATIC
    "${VST3_SDK_ROOT}/base/source/baseiids.cpp"
    "${VST3_SDK_ROOT}/base/source/fbuffer.cpp"
    "${VST3_SDK_ROOT}/base/source/fdebug.cpp"
    "${VST3_SDK_ROOT}/base/source/fdynlib.cpp"
    "${VST3_SDK_ROOT}/base/source/fobject.cpp"
    "${VST3_SDK_ROOT}/base/source/fstreamer.cpp"
    "${VST3_SDK_ROOT}/base/source/fstring.cpp"
    "${VST3_SDK_ROOT}/base/source/timer.cpp"
    "${VST3_SDK_ROOT}/base/source/updatehandler.cpp"
    "${VST3_SDK_ROOT}/base/thread/source/fcondition.cpp"
    "${VST3_SDK_ROOT}/base/thread/source/flock.cpp"
    "${VST3_SDK_ROOT}/pluginterfaces/base/conststringtable.cpp"
    "${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp"
    "${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp"
    "${VST3_SDK_ROOT}/pluginterfaces/base/ustring.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/processdata.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp"
    "${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp"
  )

  if(OS_MACOS)
    target_sources(obs-vst3-sdk PRIVATE "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm")
  elseif(OS_WINDOWS)
    target_sources(obs-vst3-sdk PRIVATE "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp")
  elseif(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
    target_sources(obs-vst3-sdk PRIVATE "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp")
  endif()

  target_include_directories(obs-vst3-sdk PUBLIC "${VST3_SDK_ROOT}")
  target_compile_features(obs-vst3-sdk PUBLIC cxx_std_17)
  target_compile_definitions(obs-vst3-sdk PUBLIC OBS_VST3_ENABLED=1)
  set_target_properties(obs-vst3-sdk PROPERTIES POSITION_INDEPENDENT_CODE ON COMPILE_WARNING_AS_ERROR OFF)

  if(OS_MACOS)
    set_source_files_properties(
      "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm"
      PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
  endif()
endif()

target_sources(
  obs-vst
  PRIVATE
    $<$<PLATFORM_ID:Darwin>:mac/EditorWidget-osx.mm>
    $<$<PLATFORM_ID:Darwin>:mac/VSTPlugin-osx.mm>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:linux/EditorWidget-linux.cpp>
    $<$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>:linux/VSTPlugin-linux.cpp>
    $<$<PLATFORM_ID:Windows>:win/EditorWidget-win.cpp>
    $<$<PLATFORM_ID:Windows>:win/VSTPlugin-win.cpp>
    EditorWidget.cpp
    headers/EditorWidget.h
    headers/vst-plugin-callbacks.hpp
    headers/VSTPlugin.h
    $<$<BOOL:${ENABLE_VST3}>:headers/VST3Plugin.h>
    obs-vst.cpp
    vst_header/aeffectx.h
    VSTPlugin.cpp
    $<$<BOOL:${ENABLE_VST3}>:VST3Plugin.cpp>
)

target_include_directories(obs-vst PRIVATE vst_header)

target_link_libraries(
  obs-vst
  PRIVATE
    OBS::libobs
    Qt::Widgets
    $<$<BOOL:${ENABLE_VST3}>:obs-vst3-sdk>
    "$<$<AND:$<BOOL:${ENABLE_VST3}>,$<PLATFORM_ID:Linux,FreeBSD,OpenBSD>>:${CMAKE_DL_LIBS}>"
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,Cocoa.framework>>"
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,Foundation.framework>>"
    "$<$<AND:$<BOOL:${ENABLE_VST3}>,$<PLATFORM_ID:Darwin>>:$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>>"
)

if(ENABLE_VST3)
  target_compile_definitions(obs-vst PRIVATE OBS_VST3_ENABLED=1)
endif()

if(OS_WINDOWS)
  set_property(TARGET obs-vst APPEND PROPERTY AUTORCC_OPTIONS --format-version 1)
endif()

set_target_properties_obs(
  obs-vst
  PROPERTIES FOLDER plugins
             PREFIX ""
             AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON
)
