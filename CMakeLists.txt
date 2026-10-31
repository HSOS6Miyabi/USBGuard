cmake_minimum_required(VERSION 3.21)
project(USBReadOnlySwitch VERSION 1.1.0 LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # Generate an anti-aliased, multi-resolution icon without external image libraries.
    set(ICON_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${ICON_OUTPUT_DIR}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/assets/generate_icon.py"
                "${ICON_OUTPUT_DIR}/usb-guard.ico"
        RESULT_VARIABLE ICON_GENERATION_RESULT
        ERROR_VARIABLE ICON_GENERATION_ERROR)
    if(NOT ICON_GENERATION_RESULT EQUAL 0)
        message(FATAL_ERROR "USBGuard icon generation failed: ${ICON_GENERATION_ERROR}")
    endif()

    file(WRITE "${ICON_OUTPUT_DIR}/ui.generated.rc"
        "#include <winuser.h>\n101 ICON \"${ICON_OUTPUT_DIR}/usb-guard.ico\"\n1 RT_MANIFEST \"${CMAKE_CURRENT_SOURCE_DIR}/app.manifest\"\n")
    file(WRITE "${ICON_OUTPUT_DIR}/toast.generated.rc"
        "#include <winuser.h>\n101 ICON \"${ICON_OUTPUT_DIR}/usb-guard.ico\"\n1 RT_MANIFEST \"${CMAKE_CURRENT_SOURCE_DIR}/toast.manifest\"\n")

set(COMMON_SOURCES engine.cpp engine.hpp command.cpp command.hpp policy.hpp)
add_executable(USBGuardUI WIN32 ui_main.cpp ${COMMON_SOURCES} "${ICON_OUTPUT_DIR}/ui.generated.rc")
add_executable(USBGuardToast WIN32 toast_main.cpp ${COMMON_SOURCES} "${ICON_OUTPUT_DIR}/toast.generated.rc")
foreach(target IN ITEMS USBGuardUI USBGuardToast)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)
    target_link_libraries(${target} PRIVATE comctl32 shell32 ole32 propsys advapi32)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE /W4 /utf-8 /EHsc /permissive-)
        target_link_options(${target} PRIVATE /MANIFEST:NO)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
# Runtime Windows Runtime entry points are dynamically resolved for Windows 7 compatibility.
set_target_properties(USBGuardUI PROPERTIES OUTPUT_NAME "USBGuardUI")
set_target_properties(USBGuardToast PROPERTIES OUTPUT_NAME "USBGuardToast")
else()
    message(STATUS "Non-Windows host: testing platform-independent logic only")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(policy_tests tests/policy_tests.cpp)
    target_compile_features(policy_tests PRIVATE cxx_std_17)
    add_test(NAME policy_tests COMMAND policy_tests)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(NAME source_and_resources COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspect_source.py)
endif()
