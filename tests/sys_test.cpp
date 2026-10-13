#include "doctest.h"
#include "sys/SingleInstance.h"
#include "sys/Autostart.h"
#include <filesystem>

TEST_CASE("ExecutablePath 返回存在的文件") {
    std::string p = cnk::ExecutablePath();
    CHECK(p.size() > 0);
    CHECK(p.find(".exe") != std::string::npos);
    CHECK(std::filesystem::exists(std::filesystem::u8path(p)));
}

TEST_CASE("互斥体可获取与释放") {
    // 用独立互斥体名，避免与真实守护实例（或其它测试进程）互相干扰
    const wchar_t* kTestMutex = L"Local\\CampusNetKeeper.SingleInstance.Test";

    cnk::SingleInstance a(kTestMutex);
    CHECK(a.Acquire());
    // 同一进程内再取一次应失败
    cnk::SingleInstance b(kTestMutex);
    CHECK_FALSE(b.Acquire());
    a.Release();
    cnk::SingleInstance c(kTestMutex);
    CHECK(c.Acquire());
    c.Release();
}

TEST_CASE("IsAutostartEnabled 不崩溃") {
    bool v = cnk::IsAutostartEnabled();
    (void)v;   // 只要不抛异常即可
}
