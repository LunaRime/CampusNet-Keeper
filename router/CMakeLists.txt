cmake_minimum_required(VERSION 3.20)
project(cnkrouter C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(cnkrouter
  src/crypto.c
  src/net.c
  src/portal.c
  src/main.c)
target_include_directories(cnkrouter PRIVATE src)

# 构建身份，与 Makefile 一致：CI / SDK 之外的本地构建也能追溯提交。
# WORKING_DIRECTORY 是 router/，git 会自动向上找到仓库根的 .git。
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE CNK_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT CNK_GIT_COMMIT)
  set(CNK_GIT_COMMIT "unknown")
endif()
string(TIMESTAMP CNK_BUILD_TS "%Y-%m-%dT%H:%M:%SZ" UTC)
target_compile_definitions(cnkrouter PRIVATE
  CNK_BUILD_COMMIT="${CNK_GIT_COMMIT}"
  CNK_BUILD_TIME="${CNK_BUILD_TS}")

if(MSVC)
  target_compile_options(cnkrouter PRIVATE /utf-8 /W3)
  target_compile_definitions(cnkrouter PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
  target_link_libraries(cnkrouter PRIVATE ws2_32)
endif()

# 交叉校验程序（仅 Windows：主项目的 AES 实现走 BCrypt）
if(WIN32)
  add_executable(aes_crosscheck
    tests/aes_crosscheck.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native/core/Crypto.cpp
    src/crypto.c)
  target_include_directories(aes_crosscheck PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/native
    src)
  target_compile_options(aes_crosscheck PRIVATE /utf-8 /EHsc)
  target_compile_definitions(aes_crosscheck PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(aes_crosscheck PRIVATE bcrypt)
endif()
