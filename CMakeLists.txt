cmake_minimum_required(VERSION 3.5)

project(partfs)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY bin)

add_executable(
  partfs

  partfs.c
)

target_compile_options(
  partfs

  PUBLIC
  -Wall -Wextra -Wno-unused-parameter -O2
)
target_compile_definitions(
  partfs

  PUBLIC
  _FILE_OFFSET_BITS=64
)
target_link_libraries(
  partfs

  fdisk
  fuse
)
