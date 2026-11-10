#include "doctest.h"
#include "tray/TrayIcon.h"
#include "core/Version.h"
#include <cwchar>
#include <string>

TEST_CASE("托盘状态标签与设置界面 v2 的六态语义一致") {
    CHECK(std::wcscmp(cnk::TrayStateLabel(cnk::RunState::Online), L"在线") == 0);
    CHECK(std::wcscmp(cnk::TrayStateLabel(cnk::RunState::Probing), L"探测中") == 0);
    CHECK(std::wcscmp(cnk::TrayStateLabel(cnk::RunState::PortalDetected), L"需要认证") == 0);
    CHECK(std::wcscmp(cnk::TrayStateLabel(cnk::RunState::Authenticating), L"认证中") == 0);
    CHECK(std::wcscmp(cnk::TrayStateLabel(cnk::RunState::AuthFailed), L"认证失败") == 0);
    CHECK(std::wcscmp(cnk::TrayStateLabel(cnk::RunState::NotCampusNetwork), L"非校园网") == 0);
    CHECK(std::wcscmp(cnk::TrayStateLabel(cnk::RunState::Disconnected), L"未连接") == 0);
}

TEST_CASE("托盘版本行能给出版本号与构建短哈希") {
    // 菜单第二行灰显「版本 x.y.z · git xxxxxxx」，排障时可直接对照 --version
    CHECK(std::string(cnk::VersionString()) == "0.1.7");
    CHECK(std::string(cnk::BuildCommitString()).length() >= 4);
}
