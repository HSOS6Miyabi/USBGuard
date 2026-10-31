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
        "#define RT_MANIFEST 24\n101 ICON \"${ICON_OUTPUT_DIR}/usb-guard.ico\"\n1 RT_MANIFEST \"${CMAKE_CURRENT_SOURCE_DIR}/app.manifest\"\n")
    file(WRITE "${ICON_OUTPUT_DIR}/mini.generated.rc"
        "#define RT_MANIFEST 24\n101 ICON \"${ICON_OUTPUT_DIR}/usb-guard.ico\"\n1 RT_MANIFEST \"${CMAKE_CURRENT_SOURCE_DIR}/mini.manifest\"\n")

set(COMMON_SOURCES engine.cpp engine.hpp command.cpp command.hpp policy.hpp)
add_executable(USBGuardUI WIN32 ui_main.cpp ${COMMON_SOURCES} "${ICON_OUTPUT_DIR}/ui.generated.rc")
add_executable(USBGuardMini WIN32 mini_main.cpp ${COMMON_SOURCES} "${ICON_OUTPUT_DIR}/mini.generated.rc")
foreach(target IN ITEMS USBGuardUI USBGuardMini)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC AND CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        # The same x86 EXEs run on XP SP3 and modern Windows. Modern APIs
        # are resolved dynamically and used when present.
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE
            WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0501 WINVER=0x0501)
        target_link_options(${target} PRIVATE "/SUBSYSTEM:WINDOWS,5.01")
    else()
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE
            WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)
    endif()
    target_link_libraries(${target} PRIVATE comctl32 shell32 ole32 propsys advapi32)
    if(MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE /W4 /utf-8 /EHsc /permissive-)
        target_link_options(${target} PRIVATE /MANIFEST:NO)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
# Integration executable is never packaged in Release. It is the only build that
# can inspect file-backed virtual disks. Production applications only accept USB.
if(BUILD_TESTING AND NOT (MSVC AND CMAKE_GENERATOR_PLATFORM STREQUAL "Win32"))
    add_executable(storage_integration tests/storage_integration.cpp engine.cpp engine.hpp policy.hpp)
    target_compile_features(storage_integration PRIVATE cxx_std_17)
    target_include_directories(storage_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(storage_integration PRIVATE USBGUARD_VIRTUAL_DISK_TEST
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)
    if(MSVC)
        target_compile_options(storage_integration PRIVATE /W4 /utf-8 /EHsc /permissive-)
    endif()
endif()
# The compact window is implemented using native Win32 and GDI.
set_target_properties(USBGuardUI PROPERTIES OUTPUT_NAME "USBGuardUI")
set_target_properties(USBGuardMini PROPERTIES OUTPUT_NAME "USBGuardMini")
else()
    include(CTest)
    add_subdirectory(linux)
endif()

include(CTest)
if(WIN32 AND BUILD_TESTING)
    add_test(NAME mini_ui_smoke COMMAND $<TARGET_FILE:USBGuardMini> --ui-self-test)
    set_tests_properties(mini_ui_smoke PROPERTIES TIMEOUT 20)
endif()
if(BUILD_TESTING)
    add_executable(policy_tests tests/policy_tests.cpp)
    target_compile_features(policy_tests PRIVATE cxx_std_17)
    add_test(NAME policy_tests COMMAND policy_tests)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    if(WIN32)
        add_test(NAME source_and_resources COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspect_source.py
                 "${ICON_OUTPUT_DIR}/usb-guard.ico")
    else()
        add_test(NAME source_and_resources COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspect_source.py)
    endif()
endif()
