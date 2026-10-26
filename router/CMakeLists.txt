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
