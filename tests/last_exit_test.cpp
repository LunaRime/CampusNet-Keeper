// 守护进程"退出画像"：正常退出写标记、启动时取走。
//
// 规矩：测试必须把自己的标记目录重定向到临时目录（SetLastExitDirForTest），
// 绝不能碰 %APPDATA% 里那份——那属于**正在运行的守护进程**：
// 测试一旦把它取走/改写，"上次是否正常退出"这个判据就被搅乱了
//（本项目踩过"测试与常驻进程共享命名资源"的假失败，见 PipeServer::SetName）。
#include "doctest.h"
#include "core/LastExit.h"
#include <windows.h>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring TempDirW() {
    wchar_t buf[MAX_PATH + 1] = {0};
    GetTempPathW(MAX_PATH, buf);
    return std::wstring(buf) + L"cnk_last_exit_test";
}

// 用**宽字符**构造 fs::path：%TEMP% 可能含中文用户名，
// fs::path(const char*) 在 Windows 上按 ANSI 解释会乱码。
struct ScopedDir {
    std::string dir;
    explicit ScopedDir(const std::wstring& w) : dir(WideToUtf8(w)) {
        cnk::SetLastExitDirForTest(dir);
        std::error_code ec;
        fs::create_directories(fs::path(w), ec);
        fs::remove(fs::path(w) / L"last_exit.txt", ec);
    }
    ~ScopedDir() { cnk::SetLastExitDirForTest(""); }
};

}  // namespace

TEST_CASE("默认标记文件落在 %APPDATA%\\CampusNet-Keeper 下") {
    cnk::SetLastExitDirForTest("");
    const std::string p = cnk::LastExitFilePath();
    CHECK(p.find("CampusNet-Keeper") != std::string::npos);
    CHECK(p.find("last_exit.txt") != std::string::npos);
}

TEST_CASE("没有标记时读到 recorded=false（= 上次不是正常退出）") {
    ScopedDir sd(TempDirW());
    cnk::LastExit le = cnk::TakeLastExit();
    CHECK_FALSE(le.recorded);
    CHECK(le.reason.empty());
}

TEST_CASE("写入后能读到来源与时间；取走后标记消失") {
    ScopedDir sd(TempDirW());

    CHECK(cnk::RecordExit("托盘菜单「退出」"));

    cnk::LastExit first = cnk::TakeLastExit();
    CHECK(first.recorded);
    CHECK(first.reason == "托盘菜单「退出」");
    CHECK_FALSE(first.at.empty());          // 时间串非空
    CHECK(first.at.size() == 19);           // "YYYY-MM-DD HH:MM:SS"

    // 取走即删除：再取一次必须回到"没有记录"（下次启动的判据靠这个）
    cnk::LastExit second = cnk::TakeLastExit();
    CHECK_FALSE(second.recorded);
}

TEST_CASE("来源可以区分：WM_CLOSE 与关机不会被记成托盘菜单") {
    ScopedDir sd(TempDirW());
    CHECK(cnk::RecordExit("外部 WM_CLOSE（脚本或其它程序请求关闭窗口）"));
    cnk::LastExit le = cnk::TakeLastExit();
    CHECK(le.reason.find("WM_CLOSE") != std::string::npos);
    CHECK(le.reason.find("托盘") == std::string::npos);
}

TEST_CASE("目录不可写时 RecordExit 返回 false 且不抛异常") {
    // 用一个不存在的盘符下的路径：既写不进去，也不能让测试崩溃
    cnk::SetLastExitDirForTest("Z:\\no-such-dir-for-cnk-test");
    bool ok = true;
    CHECK_NOTHROW(ok = cnk::RecordExit("不可能写进去"));
    CHECK_FALSE(ok);
    cnk::LastExit le;
    CHECK_NOTHROW(le = cnk::TakeLastExit());
    CHECK_FALSE(le.recorded);
    cnk::SetLastExitDirForTest("");
}
