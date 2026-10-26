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
  src/native/auth/EportalAuth.cpp
  src/native/auth/SrunAuth.cpp
  src/native/auth/GenericFormAuth.cpp
  src/native/auth/CustomTemplateAuth.cpp
  src/native/sys/SingleInstance.cpp
  src/native/sys/Autostart.cpp
  src/native/tray/TrayIcon.cpp
  src/native/core/ProbeTarget.cpp
  src/native/core/Probe.cpp
  src/native/core/PortalPage.cpp
  src/native/core/Crypto.cpp
  src/native/core/LocalIp.cpp
  src/native/core/HtmlForm.cpp
  src/native/crypto/Hash.cpp
  src/native/crypto/SrunXEncode.cpp
  src/native/ipc/Protocol.cpp
  src/native/ipc/PipeServer.cpp
  src/native/ipc/DaemonCommands.cpp
  src/native/core/ConfigForUi.cpp
  src/native/core/Scheduler.cpp
  src/native/core/NetworkChange.cpp
  src/native/core/Cli.cpp
  src/native/core/Template.cpp
  src/native/sim/SimServer.cpp
  src/native/ui_host/PayloadExtract.cpp
  src/native/ui_host/Hostfxr.cpp
)
target_include_directories(cnk_core PUBLIC src/native third_party)
target_link_libraries(cnk_core PUBLIC
  winhttp crypt32 bcrypt shell32 advapi32 ole32 user32 ws2_32 iphlpapi)

# 控制台子系统：CLI 输出（cmd/管道/重定向）在任何环境下原生可靠。
# 守护形态不依赖子系统差异——由程序自身在"独占控制台"时隐藏窗口
#（见 main.cpp 的 HideConsoleIfStandalone），兼顾无黑窗与输出可靠性。
add_executable(CampusNet-Keeper src/native/main.cpp src/native/app.rc)
target_link_libraries(CampusNet-Keeper PRIVATE cnk_core)

# 任务 26：UI 载荷资源（scripts/embed_ui.ps1 生成的 .rc）存在就编进 exe → 单 exe 双形态。
# 用 file(GLOB CONFIGURE_DEPENDS) 而不是 EXISTS：载荷的「出现/消失」都必须触发重新配置，
# 否则在已有构建树里会静默地不嵌（或残留上一版的旧载荷）——这正是本项目最怕的
# "跑的不是本次构建"。
set(CNK_UI_PAYLOAD_DIR "${CMAKE_SOURCE_DIR}/build/generated"
    CACHE PATH "UI 载荷资源生成目录（scripts/embed_ui.ps1 -OutDir）")
file(GLOB CNK_UI_PAYLOAD_RC CONFIGURE_DEPENDS "${CNK_UI_PAYLOAD_DIR}/ui_payload.rc")
if(CNK_UI_PAYLOAD_RC)
  target_sources(CampusNet-Keeper PRIVATE ${CNK_UI_PAYLOAD_RC})
  message(STATUS "已嵌入 UI 载荷 → 完整版单 exe（含设置界面）")
else()
  message(STATUS "未找到 UI 载荷 → 构建精简版（不含设置界面；见 build.ps1 -SingleExe）")
endif()

# 测试工具（不随产品发布）：让任务 23 的 C# 客户端能对着**真实的** PipeServer
# 跑跨语言契约自检（受限沙箱禁止打开命名管道，须在非沙箱下运行）。
add_executable(cnk_ipc_probe tools/ipc_probe_server.cpp)
target_link_libraries(cnk_ipc_probe PRIVATE cnk_core)

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
  tests/eportal_auth_test.cpp
  tests/srun_auth_test.cpp
  tests/generic_form_test.cpp
  tests/local_ip_test.cpp
  tests/localip_route_test.cpp
  tests/custom_auth_test.cpp
  tests/probe_test.cpp
  tests/portal_page_test.cpp
  tests/crypto_test.cpp
  tests/scheduler_test.cpp
  tests/cli_test.cpp
  tests/sys_test.cpp
  tests/integrity_test.cpp
  tests/ipc_test.cpp
  tests/config_for_ui_test.cpp
  tests/payload_extract_test.cpp
)
target_link_libraries(cnk_tests PRIVATE cnk_core)
target_include_directories(cnk_tests PRIVATE tests)
# 完整版构建（build.ps1 -SingleExe）会打开它：测试里"真实载荷"那一条缺载荷时算**失败**，
# 而不是静默跳过——否则 CI/本地都可能"表面全绿、覆盖其实没跑"（2026-10-17 审阅 P2-3）。
option(CNK_REQUIRE_REAL_PAYLOAD "缺少 build/generated 的真实 UI 载荷时让测试失败" OFF)
# 忠实性测试直接读实测证据原件，钉住模拟器输出（见 tests/sim_test.cpp）
target_compile_definitions(cnk_tests PRIVATE
  CNK_EVIDENCE_DIR="${CMAKE_SOURCE_DIR}/docs/evidence"
  CNK_UI_PAYLOAD_ZIP="${CNK_UI_PAYLOAD_DIR}/ui_payload.zip")
if(CNK_REQUIRE_REAL_PAYLOAD)
  target_compile_definitions(cnk_tests PRIVATE CNK_REQUIRE_REAL_PAYLOAD=1)
endif()
add_test(NAME cnk_tests COMMAND cnk_tests)
