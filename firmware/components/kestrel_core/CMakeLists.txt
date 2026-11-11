# Hardware-independent code. It includes no ESP-IDF headers, so firmware/sim
# compiles the same sources with a desktop compiler.
file(GLOB core_srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")

idf_component_register(
    SRCS ${core_srcs}
    INCLUDE_DIRS "include"
)
