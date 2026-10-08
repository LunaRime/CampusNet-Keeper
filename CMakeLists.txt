cmake_minimum_required(VERSION 3.20)
project(CampusNetKeeper CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_library(cnk_core STATIC
  src/native/core/Version.h
  src/native/core/Version.cpp
  src/native/core/Paths.cpp
  src/native/core/Log.cpp
  src/native/core/Http.cpp
  src/native/sim/SimServer.cpp
)
target_include_directories(cnk_core PUBLIC src/native third_party)
target_link_libraries(cnk_core PUBLIC
  winhttp crypt32 bcrypt shell32 advapi32 ole32 user32 ws2_32)

add_executable(CampusNet-Keeper src/native/main.cpp)
target_link_libraries(CampusNet-Keeper PRIVATE cnk_core)

enable_testing()
add_executable(cnk_tests
  tests/test_main.cpp
  tests/smoke_test.cpp
  tests/log_test.cpp
  tests/sim_test.cpp
  tests/http_test.cpp
)
target_link_libraries(cnk_tests PRIVATE cnk_core)
target_include_directories(cnk_tests PRIVATE tests)
# 忠实性测试直接读实测证据原件，钉住模拟器输出（见 tests/sim_test.cpp）
target_compile_definitions(cnk_tests PRIVATE
  CNK_EVIDENCE_DIR="${CMAKE_SOURCE_DIR}/docs/evidence")
add_test(NAME cnk_tests COMMAND cnk_tests)
