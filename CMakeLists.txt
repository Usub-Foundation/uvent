cmake_minimum_required(VERSION 3.22..3.29)
project(uvent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -rdynamic -fPIC" CACHE STRING "Debug flags" FORCE)
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -rdynamic -fPIC" CACHE STRING "Debug flags" FORCE)
set(UVENT_CHANNEL_DEBUG 1)

option(UVENT_ENABLE_REUSEADDR "Use SO_REUSEADDR (portable)" ON)
option(UVENT_PIN_THREADS "Use thread PINNING" ON)
option(UVENT_ENABLE_IO_URING "Use io_uring backend on Linux" OFF)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

file(GLOB_RECURSE UVENT_HEADERS include/*.h include/*.hpp)
file(GLOB_RECURSE UVENT_SOURCES src/*.cpp src/*.c)

if (CMAKE_SYSTEM_NAME MATCHES ".*BSD")
    set(BSD TRUE)
endif ()

if (APPLE OR BSD)
    # Pollers
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/EPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/EPoller\\.cpp$")
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/IocpPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/IocpPoller\\.cpp$")
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/IOUringPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/IOUringPoller\\.cpp$")

    # Sockets
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinux\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinux\\.cpp$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinuxIOUring\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinuxIOUring\\.cpp$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketWindows\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketWindows\\.cpp$")

elseif (UNIX) # Linux
    # Pollers
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/KPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/KPoller\\.cpp$")
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/IocpPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/IocpPoller\\.cpp$")

    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketBSD\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketBSD\\.cpp$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketWindows\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketWindows\\.cpp$")

    if (UVENT_ENABLE_IO_URING)
        list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/EPoller\\.h$")
        list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/EPoller\\.cpp$")
        list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinux\\.h$")
        list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinux\\.cpp$")
    else ()
        list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/IOUringPoller\\.h$")
        list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/IOUringPoller\\.cpp$")
        list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinuxIOUring\\.h$")
        list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinuxIOUring\\.cpp$")
    endif ()

elseif (WIN32)
    # Pollers
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/KPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/KPoller\\.cpp$")
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/EPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/EPoller\\.cpp$")
    list(FILTER UVENT_HEADERS EXCLUDE REGEX ".*/IOUringPoller\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/IOUringPoller\\.cpp$")

    # Sockets
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketBSD\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketBSD\\.cpp$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinux\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinux\\.cpp$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinuxIOUring\\.h$")
    list(FILTER UVENT_SOURCES EXCLUDE REGEX ".*/SocketLinuxIOUring\\.cpp$")
endif ()

foreach (header ${UVENT_HEADERS})
    if (header MATCHES "${CMAKE_BINARY_DIR}")
        list(REMOVE_ITEM UVENT_HEADERS ${header})
    endif ()
endforeach ()

message(STATUS "UVENT sources:")
foreach (src ${UVENT_SOURCES})
    message(STATUS "  ${src}")
endforeach ()

add_library(uvent ${UVENT_SOURCES})
add_library(usub::uvent ALIAS uvent)

target_include_directories(uvent
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/uvent>
)

target_compile_definitions(uvent PUBLIC
        $<$<BOOL:${UVENT_PIN_THREADS}>:UVENT_PIN_THREADS>
        $<$<BOOL:${UVENT_ENABLE_REUSEADDR}>:UVENT_ENABLE_REUSEADDR>
        $<$<BOOL:${UVENT_ENABLE_IO_URING}>:UVENT_ENABLE_IO_URING>
)

if (UNIX AND NOT APPLE AND UVENT_ENABLE_IO_URING)
    find_library(URING_LIB
            NAMES uring liburing
            PATHS
            /usr/lib
            /usr/lib/x86_64-linux-gnu
            /usr/local/lib
    )

    if (NOT URING_LIB)
        message(FATAL_ERROR "liburing not found. Install liburing-dev or set URING_LIB manually")
    endif ()

    target_link_libraries(uvent PUBLIC ${URING_LIB})
endif ()

configure_package_config_file(
        cmake/uventConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/uventConfig.cmake
        INSTALL_DESTINATION lib/cmake/uvent
)

install(TARGETS uvent
        EXPORT uventTargets
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uvent
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/uvent
)

foreach (header ${UVENT_HEADERS})
    get_filename_component(header_path ${header} DIRECTORY)
    file(RELATIVE_PATH header_relative_path "${CMAKE_CURRENT_SOURCE_DIR}/include" ${header_path})
    install(FILES ${header} DESTINATION include/server/${header_relative_path})
endforeach ()

install(EXPORT uventTargets
        FILE uventConfig.cmake
        NAMESPACE usub::
        DESTINATION lib/cmake/uvent
)

option(UVENT_BUILD_EXAMPLES "Uvent build main executable for testing" OFF)
option(UVENT_ENABLE_SANITIZERS "Uvent build sanitizer executables" OFF)

if (UVENT_BUILD_EXAMPLES)
    include(FetchContent)
    FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.15.1
    )
    FetchContent_MakeAvailable(spdlog)

    # 1
    add_executable(uvent_exe examples/main.cpp)
    target_compile_definitions(uvent_exe PRIVATE
            $<$<CONFIG:Debug>:UVENT_DEBUG>
    )
    target_include_directories(uvent_exe
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(uvent_exe PRIVATE
            uvent
            $<$<CONFIG:Debug>:spdlog::spdlog>
    )

    # 2
    add_executable(uvent_example_timers examples/main_timers.cpp)
    target_compile_definitions(uvent_example_timers PRIVATE
            $<$<CONFIG:Debug>:UVENT_DEBUG>
    )
    target_include_directories(uvent_example_timers
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(uvent_example_timers PRIVATE
            uvent
            $<$<CONFIG:Debug>:spdlog::spdlog>
    )

    # 3
    add_executable(uvent_example_channels examples/main_channels_example.cpp)
    target_compile_definitions(uvent_example_channels PRIVATE
            $<$<CONFIG:Debug>:UVENT_DEBUG>
    )
    target_include_directories(uvent_example_channels
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(uvent_example_channels PRIVATE
            uvent
            $<$<CONFIG:Debug>:spdlog::spdlog>
    )

    # 4
    add_executable(uvent_example_select examples/main_select_example.cpp)
    target_compile_definitions(uvent_example_select PRIVATE
            $<$<CONFIG:Debug>:UVENT_DEBUG>
            $<$<CONFIG:Debug>:UVENT_CHANNEL_DEBUG>
    )
    target_include_directories(uvent_example_select
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(uvent_example_select PRIVATE
            uvent
            $<$<CONFIG:Debug>:spdlog::spdlog>
    )

    if (UVENT_ENABLE_SANITIZERS)
        add_executable(uvent_asan_ubsan examples/main.cpp)
        target_link_libraries(uvent_asan_ubsan PRIVATE uvent $<$<CONFIG:Debug>:spdlog::spdlog>)
        target_compile_definitions(uvent_asan_ubsan PRIVATE
                $<$<CONFIG:Debug>:UVENT_DEBUG>
        )
        target_compile_options(uvent_asan_ubsan PRIVATE
                -fsanitize=address,undefined
                -fno-sanitize-recover=all
                -fno-omit-frame-pointer
                -g
        )
        target_link_options(uvent_asan_ubsan PRIVATE
                -fsanitize=address,undefined
        )

        add_executable(uvent_tsan examples/main.cpp)
        target_link_libraries(uvent_tsan PRIVATE uvent $<$<CONFIG:Debug>:spdlog::spdlog>)
        target_compile_definitions(uvent_tsan PRIVATE
                $<$<CONFIG:Debug>:UVENT_DEBUG>
        )
        target_compile_options(uvent_tsan PRIVATE -fsanitize=thread -g)
        target_link_options(uvent_tsan PRIVATE -fsanitize=thread)

        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            add_executable(uvent_msan_ubsan examples/main.cpp)
            target_link_libraries(uvent_msan_ubsan PRIVATE uvent $<$<CONFIG:Debug>:spdlog::spdlog>)
            target_compile_definitions(uvent_msan_ubsan PRIVATE
                    $<$<CONFIG:Debug>:UVENT_DEBUG>
            )
            target_compile_options(uvent_msan_ubsan PRIVATE
                    -fsanitize=memory,undefined
                    -fsanitize-memory-track-origins=2
                    -fno-sanitize-recover=all
                    -fno-omit-frame-pointer
                    -g
            )
            target_link_options(uvent_msan_ubsan PRIVATE
                    -fsanitize=memory,undefined
            )
        endif ()
    endif ()
endif ()
