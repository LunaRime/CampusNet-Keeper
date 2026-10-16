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
  src/native/core/Encoding.cpp
  src/native/core/Url.cpp
  src/native/core/Integrity.cpp
  src/native/store/Dpapi.cpp
  src/native/store/Config.cpp
  src/native/auth/Detector.cpp
  src/native/auth/Carrier.cpp
  src/native/auth/AuthFactory.cpp
  src/native/auth/DrcomAuth.cpp
  src/native/auth/CustomTemplateAuth.cpp
  src/native/auth/Stubs.cpp
  src/native/sys/SingleInstance.cpp
  src/native/sys/Autostart.cpp
  src/native/tray/TrayIcon.cpp
  src/native/core/ProbeTarget.cpp
  src/native/core/Probe.cpp
  src/native/core/PortalPage.cpp
  src/native/core/Crypto.cpp
  src/native/core/Scheduler.cpp
  src/native/core/NetworkChange.cpp
  src/native/core/Cli.cpp
  src/native/core/Template.cpp
  src/native/sim/SimServer.cpp
)
target_include_directories(cnk_core PUBLIC src/native third_party)
target_link_libraries(cnk_core PUBLIC
  winhttp crypt32 bcrypt shell32 advapi32 ole32 user32 ws2_32 iphlpapi)

# 控制台子系统：CLI 输出（cmd/管道/重定向）在任何环境下原生可靠。
# 守护形态不依赖子系统差异——由程序自身在"独占控制台"时隐藏窗口
#（见 main.cpp 的 HideConsoleIfStandalone），兼顾无黑窗与输出可靠性。
add_executable(CampusNet-Keeper src/native/main.cpp)
target_link_libraries(CampusNet-Keeper PRIVATE cnk_core)

enable_testing()
add_executable(cnk_tests
  tests/test_main.cpp
  tests/smoke_test.cpp
  tests/log_test.cpp
  tests/sim_test.cpp
  tests/http_test.cpp
  tests/encoding_test.cpp
  tests/url_test.cpp
  tests/config_test.cpp
  tests/config_io_test.cpp
  tests/detector_test.cpp
  tests/carrier_test.cpp
  tests/probe_target_test.cpp
  tests/auth_factory_test.cpp
  tests/drcom_auth_test.cpp
  tests/custom_auth_test.cpp
  tests/probe_test.cpp
  tests/portal_page_test.cpp
  tests/crypto_test.cpp
  tests/scheduler_test.cpp
  tests/cli_test.cpp
  tests/sys_test.cpp
  tests/integrity_test.cpp
)
target_link_libraries(cnk_tests PRIVATE cnk_core)
target_include_directories(cnk_tests PRIVATE tests)
# 忠实性测试直接读实测证据原件，钉住模拟器输出（见 tests/sim_test.cpp）
target_compile_definitions(cnk_tests PRIVATE
  CNK_EVIDENCE_DIR="${CMAKE_SOURCE_DIR}/docs/evidence")
add_test(NAME cnk_tests COMMAND cnk_tests)
