cmake_minimum_required(VERSION 3.25)

# Reject in-source builds before CMake creates compiler artifacts.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/cmake")
endif()

project(ClassicTaskManager VERSION 2.0.0 LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR "Classic Task Manager requires Windows (Win32 APIs and resources).")
endif()
enable_language(RC)

option(TASKMAN_STRICT "Treat application compiler warnings as errors" OFF)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

add_library(taskman_platform INTERFACE)
target_include_directories(taskman_platform INTERFACE
    "$<$<COMPILE_LANGUAGE:C>:${PROJECT_SOURCE_DIR}/include>")
target_compile_definitions(taskman_platform INTERFACE
    UNICODE _UNICODE $<$<CONFIG:Debug>:_DEBUG>)
target_link_libraries(taskman_platform INTERFACE
    comctl32 comdlg32 ole32 shlwapi shell32 psapi uxtheme dwmapi
    iphlpapi wtsapi32 version advapi32 gdi32 user32 kernel32 pdh dxgi
    # dxguid supplies IID_IDXGIFactory1. MinGW's libdxgi.a already carries
    # the interface GUIDs; the MSVC import library does not.
    dxguid)
if(MSVC)
    target_include_directories(taskman_platform INTERFACE
        "$<$<COMPILE_LANGUAGE:RC>:${PROJECT_SOURCE_DIR}/res>")
    target_compile_options(taskman_platform INTERFACE "$<$<COMPILE_LANGUAGE:C>:/utf-8>")
elseif(MINGW)
    # windres finds assets beside the input RC. Avoid absolute -I arguments:
    # some Windows binutils versions misquote spaces in preprocessor paths.
    # Match the original release scripts; CMake's GNU default is -O3.
    target_compile_options(taskman_platform INTERFACE
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Release>>:-O2>")
endif()

set(TASKMAN_PAGE_SOURCES
    src/tabs/tab_apps.c
    src/tabs/tab_processes.c
    src/tabs/tab_services.c
    src/tabs/tab_perf.c
    src/tabs/tab_network.c
    src/tabs/tab_users.c
    src/tabs/proc_tree.c)
set(TASKMAN_SUPPORT_SOURCES src/ui.c src/settings.c src/sysinfo.c src/gpu.c)
set(TASKMAN_RESOURCE "${PROJECT_SOURCE_DIR}/res/resource.rc")
# RC compilers do not consistently emit dependency files for embedded assets.
set(TASKMAN_RESOURCE_DEPENDENCIES
    "${PROJECT_SOURCE_DIR}/res/app.manifest"
    "${PROJECT_SOURCE_DIR}/res/app.ico"
    "${PROJECT_SOURCE_DIR}/include/resource.h")
set_property(SOURCE "${TASKMAN_RESOURCE}" PROPERTY OBJECT_DEPENDS
    "${TASKMAN_RESOURCE_DEPENDENCIES}")

add_executable(taskman WIN32
    src/main.c ${TASKMAN_SUPPORT_SOURCES} ${TASKMAN_PAGE_SOURCES}
    "${TASKMAN_RESOURCE}"
    include/app.h include/ui.h include/ntapi.h include/gdiplusapi.h
    include/proc_tree.h include/gpu.h include/resource.h)
target_link_libraries(taskman PRIVATE taskman_platform)
if(MSVC)
    target_compile_options(taskman PRIVATE "$<$<COMPILE_LANGUAGE:C>:/W4>"
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<BOOL:${TASKMAN_STRICT}>>:/WX>")
    target_link_options(taskman PRIVATE /ENTRY:wWinMainCRTStartup /MANIFEST:NO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT taskman_ipo_supported OUTPUT taskman_ipo_error LANGUAGES C)
    if(taskman_ipo_supported)
        set_property(TARGET taskman PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
elseif(MINGW)
    target_compile_options(taskman PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wshadow;-Wcast-qual;-Wpointer-arith;-Wstrict-prototypes;-Wmissing-prototypes;-Wwrite-strings;-Wconversion;-Wsign-conversion;-Wredundant-decls;-Wundef>"
        "$<$<AND:$<COMPILE_LANGUAGE:C>,$<BOOL:${TASKMAN_STRICT}>>:-Werror>")
    target_link_options(taskman PRIVATE -municode "$<$<CONFIG:Release>:-s>")
else()
    message(FATAL_ERROR "Use MSVC or a MinGW-w64 compiler.")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS taskman RUNTIME DESTINATION bin)
