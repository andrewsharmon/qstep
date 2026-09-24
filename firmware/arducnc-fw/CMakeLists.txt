# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(arducnc_fw)

# Record neutral paths (no local directory/user names) in the binaries.
zephyr_compile_options(
  -ffile-prefix-map=${ZEPHYR_BASE}/=zephyr/
  -ffile-prefix-map=${ZEPHYR_BASE}/../=
  -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/../=firmware/
  -ffile-prefix-map=${CMAKE_BINARY_DIR}/=build/
  -ffile-prefix-map=$ENV{HOME}/=
)

target_include_directories(app PRIVATE ../common)
target_sources(app PRIVATE src/main.c src/stepgen.c src/matrix.c)
