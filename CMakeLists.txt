cmake_minimum_required(VERSION 3.16)
project(kb_core VERSION 0.1.0 LANGUAGES C)

# ── C11 strict mode (§1.1, §7.5) ─────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Strict warnings — the spec (§7.5) states the ABI header compiles
# clean with gcc -std=c11 -Wall -Wextra -pedantic -Werror.
add_compile_options(
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wshadow -Wstrict-prototypes
    -Wdouble-promotion -Wformat=2
    -fstack-protector-strong
)

# Debug by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ── Core library ──────────────────────────────────────────────────
add_library(kb_core STATIC
    src/core/kb_path.c
    src/core/kb_crc32c.c
)
target_include_directories(kb_core PUBLIC src)

# ── Tests ─────────────────────────────────────────────────────────
enable_testing()

add_executable(test_path tests/test_path.c)
target_link_libraries(test_path PRIVATE kb_core)
add_test(NAME path_codec COMMAND test_path)

add_executable(test_crc32c tests/test_crc32c.c)
target_link_libraries(test_crc32c PRIVATE kb_core)
add_test(NAME crc32c COMMAND test_crc32c)
