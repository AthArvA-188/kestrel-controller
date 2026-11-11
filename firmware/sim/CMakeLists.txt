# Native build of the hardware-independent code: unit tests and UI screenshots.
cmake_minimum_required(VERSION 3.20)
project(kestrel_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CORE ${CMAKE_CURRENT_LIST_DIR}/../components/kestrel_core)
file(GLOB CORE_SRCS ${CORE}/src/*.c)
file(GLOB TEST_SRCS ${CMAKE_CURRENT_LIST_DIR}/tests/*.c)

add_library(kestrel_core STATIC ${CORE_SRCS})
target_include_directories(kestrel_core PUBLIC ${CORE}/include)
if(MSVC)
    target_compile_options(kestrel_core PUBLIC /W4 /utf-8)
    target_compile_definitions(kestrel_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(kestrel_core PUBLIC -Wall -Wextra)
    target_link_libraries(kestrel_core PUBLIC m)
endif()

add_executable(kestrel_tests ${TEST_SRCS})
target_link_libraries(kestrel_tests PRIVATE kestrel_core)

add_executable(kestrel_render render_main.c)
target_link_libraries(kestrel_render PRIVATE kestrel_core)

enable_testing()
add_test(NAME unit COMMAND kestrel_tests ${CMAKE_CURRENT_LIST_DIR}/out)
