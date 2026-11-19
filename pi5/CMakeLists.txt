add_executable(pi5
    pi5.c
)

target_include_directories(pi5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pi5 PRIVATE
    shared
)

target_compile_options(pi5 PRIVATE
    -Wall
    -Wextra
    -O2
)