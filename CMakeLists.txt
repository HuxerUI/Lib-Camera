cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(camera VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

if (IOS AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_language(OBJCXX)
endif ()

set(LIBRARY_SOURCE_FILES
        src/camera.cpp
        src/camera_frame_pipeline.cpp
        src/camera_preview.cpp
        src/camera_session.cpp
)
if (ANDROID OR APPLE OR WIN32)
    list(APPEND LIBRARY_SOURCE_FILES src/camera_recording.cpp src/camera_recording_audio.cpp)
else ()
    list(APPEND LIBRARY_SOURCE_FILES src/camera_unavailable.cpp)
endif ()

if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (APPLE AND NOT IOS)
    file(GLOB_RECURSE LIBRARY_MACOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.mm"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_MACOS_SOURCE_FILES})
endif ()
if (IOS)
    list(APPEND LIBRARY_SOURCE_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/src/camera.mm"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/src/output_texture.mm"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/src/recording_frame.mm"
    )
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_camera
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_camera
)

add_library(HuxerUI::Camera ALIAS huxerui_camera)

set(HUXERUI_CAMERA_MEDIA_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Lib-Media"
        CACHE PATH "Lib-Media source checkout required by Camera")
if (TARGET HuxerUI::Media)
    huxerui_use_library(huxerui_camera TARGET HuxerUI::Media)
else ()
    huxerui_use_library(huxerui_camera TARGET HuxerUI::Media PATH "${HUXERUI_CAMERA_MEDIA_PATH}")
endif ()

set(HUXERUI_CAMERA_AUDIO_CAPTURE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Lib-AudioCapture"
        CACHE PATH "Lib-AudioCapture source checkout required by Camera")
if (TARGET HuxerUI::AudioCapture)
    huxerui_use_library(huxerui_camera PUBLIC TARGET HuxerUI::AudioCapture)
else ()
    huxerui_use_library(huxerui_camera PUBLIC
            TARGET HuxerUI::AudioCapture PATH "${HUXERUI_CAMERA_AUDIO_CAPTURE_PATH}")
endif ()

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_camera PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

target_include_directories(huxerui_camera PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if (WIN32)
    target_compile_definitions(huxerui_camera PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(huxerui_camera PRIVATE mfplat mf mfuuid d3d11 d2d1 windowscodecs runtimeobject ole32 oleaut32)
endif ()

if (ANDROID)
    target_link_libraries(huxerui_camera PRIVATE EGL GLESv2)
endif ()

if (APPLE)
    target_compile_options(huxerui_camera PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(huxerui_camera PRIVATE "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
        "-framework CoreImage" "-framework ImageIO" "-framework Accelerate")
endif ()

option(HUXERUI_CAMERA_BUILD_TESTS "Build Camera module tests" OFF)
if (HUXERUI_CAMERA_BUILD_TESTS AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_testing()
    function(camera_add_test name)
        add_executable(${name} "tests/${name}.cpp" tests/camera_test_fixture.cpp)
        target_compile_features(${name} PRIVATE cxx_std_20)
        target_include_directories(${name} PRIVATE src tests)
        target_link_libraries(${name} PRIVATE HuxerUI::Camera HuxerUI::huxerui_static)
        huxerui_enable_codegen(${name})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    camera_add_test(camera_tests)
    if (APPLE AND NOT IOS)
        camera_add_test(camera_recording_tests)
        target_link_libraries(camera_recording_tests PRIVATE HuxerUI::Media)
        get_target_property(camera_audio_capture_source HuxerUI::AudioCapture SOURCE_DIR)
        if (EXISTS "${camera_audio_capture_source}/src/capture_run.h")
            camera_add_test(camera_audio_recording_tests)
            target_link_libraries(camera_audio_recording_tests PRIVATE HuxerUI::Media)
            target_include_directories(camera_audio_recording_tests PRIVATE "${camera_audio_capture_source}/src")
        endif ()
    endif ()
    if (WIN32)
        add_executable(camera_windows_tests tests/windows_camera_tests.cpp)
        target_compile_features(camera_windows_tests PRIVATE cxx_std_20)
        target_compile_definitions(camera_windows_tests PRIVATE NOMINMAX UNICODE _UNICODE)
        target_include_directories(camera_windows_tests PRIVATE src platform/windows/src tests)
        target_link_libraries(camera_windows_tests PRIVATE HuxerUI::Camera HuxerUI::huxerui_static)
        add_test(NAME camera_windows_tests COMMAND camera_windows_tests)
    endif ()
endif ()
