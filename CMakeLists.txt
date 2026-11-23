cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(bluetooth VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
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
if (APPLE)
    if (IOS)
        set(BLUETOOTH_PLATFORM ios)
    else ()
        set(BLUETOOTH_PLATFORM macos)
    endif ()
    file(GLOB_RECURSE BLUETOOTH_PLATFORM_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/${BLUETOOTH_PLATFORM}/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/${BLUETOOTH_PLATFORM}/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/${BLUETOOTH_PLATFORM}/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/${BLUETOOTH_PLATFORM}/src/*.mm"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${BLUETOOTH_PLATFORM_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_bluetooth
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_bluetooth
)

add_library(HuxerUI::Bluetooth ALIAS huxerui_bluetooth)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_bluetooth PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_include_directories(huxerui_bluetooth PRIVATE src)
target_compile_features(huxerui_bluetooth PUBLIC cxx_std_20)

if (APPLE)
    target_compile_options(huxerui_bluetooth PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(huxerui_bluetooth PRIVATE "-framework CoreBluetooth" "-framework Foundation")
    if (NOT IOS)
        target_link_libraries(huxerui_bluetooth PRIVATE "-framework IOBluetooth")
    endif ()
elseif (WIN32 AND NOT HUXERUI_WINDOWS_7_COMPAT)
    target_compile_definitions(huxerui_bluetooth PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(huxerui_bluetooth PRIVATE runtimeobject)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BLUETOOTH_GIO REQUIRED IMPORTED_TARGET gio-2.0)
    find_path(BLUETOOTH_LINUX_HEADERS bluetooth/l2cap.h REQUIRED)
    target_include_directories(huxerui_bluetooth PRIVATE "${BLUETOOTH_LINUX_HEADERS}")
    target_link_libraries(huxerui_bluetooth PRIVATE PkgConfig::BLUETOOTH_GIO)
elseif (EMSCRIPTEN)
    set(BLUETOOTH_BROWSER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/bluetooth.js")
    set_property(TARGET huxerui_bluetooth APPEND PROPERTY INTERFACE_LINK_DEPENDS "${BLUETOOTH_BROWSER_SCRIPT}")
    target_link_options(huxerui_bluetooth INTERFACE "SHELL:--pre-js \"${BLUETOOTH_BROWSER_SCRIPT}\"" "--bind")
endif ()

if (HUXERUI_WINDOWS_7_COMPAT)
    target_compile_definitions(huxerui_bluetooth PRIVATE HUXERUI_WINDOWS_7_COMPAT=1)
endif ()
