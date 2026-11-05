cmake_minimum_required(VERSION 3.20)
project(EdgeEcho VERSION 0.4.0 LANGUAGES C CXX)

option(EDGEECHO_BUILD_GUI "Build the Qt 6 desktop player" ON)
option(EDGEECHO_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)

add_library(edgeecho_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(edgeecho_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(edgeecho_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION)
target_link_libraries(edgeecho_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX)
    target_link_libraries(edgeecho_sqlite PUBLIC m)
endif()

add_library(edgeecho_core STATIC
    src/database.cpp src/codec.cpp src/playback.cpp src/demo.cpp
    src/source_identity.cpp src/atomic_output.cpp src/audio.cpp src/build_info.cpp)
target_include_directories(edgeecho_core PUBLIC include)
target_link_libraries(edgeecho_core PRIVATE edgeecho_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(edgeecho_core PUBLIC winmm)
endif()
target_compile_features(edgeecho_core PUBLIC cxx_std_17)

# A reproducible identity follows all production source and build inputs.
file(GLOB_RECURSE edgeecho_identity_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/.github/*.yml"
    "${CMAKE_CURRENT_SOURCE_DIR}/.github/*.yaml")
list(APPEND edgeecho_identity_inputs "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.h")
list(SORT edgeecho_identity_inputs)
set(edgeecho_digest_input "")
foreach(input IN LISTS edgeecho_identity_inputs)
    file(SHA256 "${input}" digest)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${input}")
    string(APPEND edgeecho_digest_input "${relative}:${digest}\n")
endforeach()
string(SHA256 EDGEECHO_SOURCE_ID "${edgeecho_digest_input}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${edgeecho_identity_inputs})
configure_file(src/build_metadata.hpp.in generated/build_metadata.hpp @ONLY)
target_include_directories(edgeecho_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

function(edgeecho_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()
edgeecho_warnings(edgeecho_core)
if(EDGEECHO_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "EDGEECHO_SANITIZERS currently requires GCC or Clang")
    endif()
    foreach(target edgeecho_sqlite edgeecho_core)
        target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()

add_executable(edgeecho_cli tools/cli.cpp)
target_link_libraries(edgeecho_cli PRIVATE edgeecho_core)
edgeecho_warnings(edgeecho_cli)
if(MINGW)
    # Keep the cross-built CLI self-contained apart from Windows system DLLs.
    target_link_options(edgeecho_cli PRIVATE -municode -static)
endif()

if(EDGEECHO_BUILD_GUI)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Concurrent)
    set(edgeecho_gui_sources app/main.cpp)
    foreach(component plot_widget time_range_editor native_theme)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/${component}.cpp")
            list(APPEND edgeecho_gui_sources "app/${component}.cpp")
        endif()
    endforeach()
    add_executable(edgeecho ${edgeecho_gui_sources})
    target_link_libraries(edgeecho PRIVATE edgeecho_core Qt6::Widgets Qt6::Concurrent)
    edgeecho_warnings(edgeecho)
    if(WIN32)
        # Qt supplies the native GUI entry point; console CLI keeps its own wmain.
        set_target_properties(edgeecho PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    install(TARGETS edgeecho RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/validation/runner_tests.py")
        add_test(NAME validation_runner COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/validation/runner_tests.py")
        set_tests_properties(validation_runner PROPERTIES TIMEOUT 30)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/database_tests.cpp")
        add_executable(database_tests tests/database_tests.cpp)
        target_link_libraries(database_tests PRIVATE edgeecho_core edgeecho_sqlite)
        edgeecho_warnings(database_tests)
        add_test(NAME database COMMAND database_tests "${CMAKE_CURRENT_BINARY_DIR}/test-data/database")
        set_tests_properties(database PROPERTIES TIMEOUT 120)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/codec_playback.cpp")
        add_executable(codec_playback_tests tests/codec_playback.cpp)
        target_link_libraries(codec_playback_tests PRIVATE edgeecho_core)
        edgeecho_warnings(codec_playback_tests)
        add_test(NAME codec_playback COMMAND codec_playback_tests)
        set_tests_properties(codec_playback PROPERTIES TIMEOUT 30)
    endif()
    foreach(suite session export browsing)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}_tests.cpp")
            add_executable(${suite}_tests "tests/${suite}_tests.cpp")
            target_link_libraries(${suite}_tests PRIVATE edgeecho_core edgeecho_sqlite)
            edgeecho_warnings(${suite}_tests)
            add_test(NAME ${suite} COMMAND ${suite}_tests "${CMAKE_CURRENT_BINARY_DIR}/test-data/${suite}")
            set_tests_properties(${suite} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-data")
    # A fixture is created from scratch per CTest invocation, permitting repeated runs.
    add_test(NAME cli_demo COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:edgeecho_cli>
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/test-data/demo.db
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateDemo.cmake)
    set_tests_properties(cli_demo PROPERTIES FIXTURES_SETUP demo TIMEOUT 60)
    add_test(NAME cli_inspect COMMAND edgeecho_cli inspect
        "${CMAKE_CURRENT_BINARY_DIR}/test-data/demo.db")
    set_tests_properties(cli_inspect PROPERTIES FIXTURES_REQUIRED demo TIMEOUT 30)
    add_test(NAME cli_invalid_number COMMAND edgeecho_cli demo
        "${CMAKE_CURRENT_BINARY_DIR}/test-data/invalid.db" --records 20garbage)
    set_tests_properties(cli_invalid_number PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
    if(EDGEECHO_BUILD_GUI)
        set(edgeecho_qt_home "${CMAKE_CURRENT_BINARY_DIR}/test-data/qt-home")
        file(MAKE_DIRECTORY "${edgeecho_qt_home}/cache" "${edgeecho_qt_home}/config"
            "${edgeecho_qt_home}/data" "${edgeecho_qt_home}/runtime")
        if(UNIX)
            file(CHMOD "${edgeecho_qt_home}/runtime"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
        endif()
        foreach(component plot_widget time_range_editor native_theme)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui/${component}_tests.cpp")
                add_executable(${component}_tests "tests/gui/${component}_tests.cpp" "app/${component}.cpp")
                target_include_directories(${component}_tests PRIVATE app)
                target_link_libraries(${component}_tests PRIVATE edgeecho_core Qt6::Widgets)
                edgeecho_warnings(${component}_tests)
                add_test(NAME ${component} COMMAND ${component}_tests)
                set_tests_properties(${component} PROPERTIES TIMEOUT 60
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CACHE_HOME=${edgeecho_qt_home}/cache;XDG_CONFIG_HOME=${edgeecho_qt_home}/config;XDG_DATA_HOME=${edgeecho_qt_home}/data;XDG_RUNTIME_DIR=${edgeecho_qt_home}/runtime;TMPDIR=${edgeecho_qt_home}/runtime;TMP=${edgeecho_qt_home}/runtime;TEMP=${edgeecho_qt_home}/runtime")
            endif()
        endforeach()
        add_test(NAME gui_smoke COMMAND edgeecho --smoke
            "${CMAKE_CURRENT_BINARY_DIR}/test-data/demo.db"
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/test-data/gui-smoke.png")
        set_tests_properties(gui_smoke PROPERTIES FIXTURES_REQUIRED demo TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CACHE_HOME=${edgeecho_qt_home}/cache;XDG_CONFIG_HOME=${edgeecho_qt_home}/config;XDG_DATA_HOME=${edgeecho_qt_home}/data;XDG_RUNTIME_DIR=${edgeecho_qt_home}/runtime;TMPDIR=${edgeecho_qt_home}/runtime;TMP=${edgeecho_qt_home}/runtime;TEMP=${edgeecho_qt_home}/runtime")
        if(Python3_Interpreter_FOUND)
            add_test(NAME gui_inputs COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui/smoke.py"
                --exe $<TARGET_FILE:edgeecho>
                --database "${CMAKE_CURRENT_BINARY_DIR}/test-data/demo.db"
                --output "${CMAKE_CURRENT_BINARY_DIR}/test-data/gui-inputs")
            set_tests_properties(gui_inputs PROPERTIES FIXTURES_REQUIRED demo TIMEOUT 240)
        endif()
    endif()
endif()

# Explicit benchmark target; large workloads are not part of ordinary CTest.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/performance/browsing_bench.cpp")
    add_executable(edgeecho_browsing_bench tests/performance/browsing_bench.cpp)
    target_link_libraries(edgeecho_browsing_bench PRIVATE edgeecho_core)
    edgeecho_warnings(edgeecho_browsing_bench)
endif()

install(TARGETS edgeecho_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

install(DIRECTORY docs/ DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs")
install(DIRECTORY third_party/licenses/ DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses")
