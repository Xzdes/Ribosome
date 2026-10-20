cmake_minimum_required(VERSION 3.20)
project(Ribosome C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# The core: no dependencies, no platform API. It must also compile with
# clang --target=wasm32-unknown-unknown -nostdlib for the browser shell.
add_library(ribo_core STATIC
    src/parse.c
    src/check.c
    src/interp.c
    src/fault.c
    src/emit_wasm.c
    src/lift.c
)
target_include_directories(ribo_core PUBLIC include)

add_executable(ribo src/cli.c)
target_link_libraries(ribo PRIVATE ribo_core)

enable_testing()
# Differential suites register here from 0.3 on (see docs/roadmap.md).
