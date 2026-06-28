cmake_minimum_required(VERSION 3.28...3.30)

add_library(mediapipe-segmentation MODULE)
add_library(OBS::mediapipe-segmentation ALIAS mediapipe-segmentation)

set(MEDIAPIPE_SEGMENTATION_RUNTIME_DIR
    ""
    CACHE PATH "Directory containing obs-mediapipe-runtime.dll, dependent DLLs, and model files"
)
set(ONNXRUNTIME_ROOT
    ""
    CACHE PATH "Root directory of the Microsoft.ML.OnnxRuntime.DirectML NuGet package"
)
set(DIRECTML_ROOT
    ""
    CACHE PATH "Root directory of the Microsoft.AI.DirectML NuGet package"
)

target_sources(
  mediapipe-segmentation
  PRIVATE mediapipe-segmentation.cpp
  PUBLIC runtime/mediapipe-runtime.h
)

target_link_libraries(mediapipe-segmentation PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in mediapipe-segmentation.rc)
  target_sources(mediapipe-segmentation PRIVATE mediapipe-segmentation.rc)
endif()

set_target_properties_obs(mediapipe-segmentation PROPERTIES FOLDER plugins PREFIX "")

if(OS_WINDOWS AND ONNXRUNTIME_ROOT)
  if(CMAKE_VS_PLATFORM_NAME STREQUAL "Win32")
    set(_ort_runtime_id win-x86)
    set(_ort_lib_arch x86)
  elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
    set(_ort_runtime_id win-arm64)
    set(_ort_lib_arch arm64)
  else()
    set(_ort_runtime_id win-x64)
    set(_ort_lib_arch x64)
  endif()

  find_path(
    ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_c_api.h onnxruntime_cxx_api.h
    HINTS
      "${ONNXRUNTIME_ROOT}/include"
      "${ONNXRUNTIME_ROOT}/build/native/include"
    NO_DEFAULT_PATH
  )

  find_library(
    ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    HINTS
      "${ONNXRUNTIME_ROOT}/lib"
      "${ONNXRUNTIME_ROOT}/lib/${_ort_lib_arch}"
      "${ONNXRUNTIME_ROOT}/build/native/lib"
      "${ONNXRUNTIME_ROOT}/build/native/lib/${_ort_lib_arch}"
      "${ONNXRUNTIME_ROOT}/runtimes/${_ort_runtime_id}/native"
    NO_DEFAULT_PATH
  )

  if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR "ONNXRUNTIME_ROOT must contain ONNX Runtime DirectML headers and onnxruntime.lib")
  endif()

  if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/dml_provider_factory.h"
     AND NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime/core/providers/dml/dml_provider_factory.h"
  )
    message(FATAL_ERROR "ONNXRUNTIME_ROOT must be a DirectML package with dml_provider_factory.h")
  endif()

  add_library(obs-mediapipe-runtime SHARED runtime/onnx-directml/obs-mediapipe-runtime.cpp)
  target_include_directories(
    obs-mediapipe-runtime
    PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
  )
  target_link_libraries(obs-mediapipe-runtime PRIVATE "${ONNXRUNTIME_LIBRARY}")
  target_compile_features(obs-mediapipe-runtime PRIVATE cxx_std_17)
  target_compile_definitions(obs-mediapipe-runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  set_target_properties(
    obs-mediapipe-runtime
    PROPERTIES
      FOLDER plugins/mediapipe-segmentation
      OUTPUT_NAME obs-mediapipe-runtime
  )
  add_dependencies(mediapipe-segmentation obs-mediapipe-runtime)

  file(
    GLOB _ort_runtime_dlls
    "${ONNXRUNTIME_ROOT}/runtimes/${_ort_runtime_id}/native/*.dll"
    "${ONNXRUNTIME_ROOT}/build/native/bin/${_ort_lib_arch}/*.dll"
    "${ONNXRUNTIME_ROOT}/bin/*.dll"
  )

  if(DIRECTML_ROOT)
    file(
      GLOB _directml_runtime_dlls
      "${DIRECTML_ROOT}/bin/${_ort_lib_arch}-win/DirectML.dll"
      "${DIRECTML_ROOT}/bin/${_ort_runtime_id}/DirectML.dll"
    )
    list(APPEND _ort_runtime_dlls ${_directml_runtime_dlls})
  endif()

  set(_mediapipe_runtime_destination "${OBS_DATA_DESTINATION}/obs-plugins/mediapipe-segmentation")

  install(
    TARGETS obs-mediapipe-runtime
    RUNTIME DESTINATION "${_mediapipe_runtime_destination}"
    COMPONENT Runtime
  )

  add_custom_command(
    TARGET obs-mediapipe-runtime
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E echo "Copy DirectML segmentation runtime"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${OBS_OUTPUT_DIR}/$<CONFIG>/${_mediapipe_runtime_destination}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:obs-mediapipe-runtime>"
            "${OBS_OUTPUT_DIR}/$<CONFIG>/${_mediapipe_runtime_destination}"
    COMMENT ""
    VERBATIM
  )

  if(_ort_runtime_dlls)
    install(FILES ${_ort_runtime_dlls} DESTINATION "${_mediapipe_runtime_destination}" COMPONENT Runtime)

    add_custom_command(
      TARGET obs-mediapipe-runtime
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_ort_runtime_dlls}
              "${OBS_OUTPUT_DIR}/$<CONFIG>/${_mediapipe_runtime_destination}"
      COMMENT ""
      VERBATIM
      COMMAND_EXPAND_LISTS
    )
  endif()
endif()

if(MEDIAPIPE_SEGMENTATION_RUNTIME_DIR)
  if(NOT IS_DIRECTORY "${MEDIAPIPE_SEGMENTATION_RUNTIME_DIR}")
    message(FATAL_ERROR "MEDIAPIPE_SEGMENTATION_RUNTIME_DIR must point to an existing directory")
  endif()

  install(
    DIRECTORY "${MEDIAPIPE_SEGMENTATION_RUNTIME_DIR}/"
    DESTINATION "${OBS_DATA_DESTINATION}/obs-plugins/mediapipe-segmentation"
    USE_SOURCE_PERMISSIONS
    COMPONENT Runtime
  )

  add_custom_command(
    TARGET mediapipe-segmentation
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E echo "Copy MediaPipe segmentation runtime files"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_directory "${MEDIAPIPE_SEGMENTATION_RUNTIME_DIR}"
      "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_DATA_DESTINATION}/obs-plugins/mediapipe-segmentation"
    COMMENT ""
    VERBATIM
  )
endif()
