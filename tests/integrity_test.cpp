#include "doctest.h"
#include "core/Integrity.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 目录名带唯一后缀：机器上可能残留上次运行留下的同名目录（例如被沙箱加固过 ACL、
// 当前令牌删不掉的那种），复用固定名字会让 remove_all / create_directories 直接抛
// 异常、整例假失败。用 error_code 重载 + 唯一名，让夹具不受环境残留影响。
fs::path FreshDir(const char* name) {
    static int counter = 0;
    const std::string uniq =
        std::string(name) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "_" + std::to_string(++counter);
    auto d = fs::temp_directory_path() / uniq;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void CleanDir(const fs::path& d) {
    std::error_code ec;
    fs::remove_all(d, ec);
}

// 给对象写一条显式的低完整性标签（测试夹具；对象由本进程创建，故有 WRITE_DAC）
bool SetLabelLow(const fs::path& p) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-16-4096", &sid)) return false;
    std::vector<BYTE> aclBuf(256);
    auto* acl = reinterpret_cast<PACL>(aclBuf.data());
    bool ok = InitializeAcl(acl, (DWORD)aclBuf.size(), ACL_REVISION) &&
              AddMandatoryAce(acl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, sid);
    if (ok) {
        const std::wstring w = p.wstring();
        ok = SetNamedSecurityInfoW(const_cast<wchar_t*>(w.c_str()), SE_FILE_OBJECT,
                                   LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                   nullptr, acl) == ERROR_SUCCESS;
    }
    LocalFree(sid);
    return ok;
}

}  // namespace

TEST_CASE("完整性标签：无标签读回 -1，写低标签后读回低 RID") {
    auto dir = FreshDir("cnk_integrity_1");
    auto file = dir / "probe.bin";
    { std::ofstream ofs(file, std::ios::binary); ofs << "x"; }

    // 常规文件（本机默认标签行为）应无显式标签 ACE
    CHECK(cnk::FileIntegrityRid(file.u8string()) == -1);

    REQUIRE(SetLabelLow(file));
    REQUIRE(cnk::FileIntegrityRid(file.u8string()) == (long long)SECURITY_MANDATORY_LOW_RID);
    CHECK(cnk::IntegrityRidName(SECURITY_MANDATORY_LOW_RID) == "低");

    CleanDir(dir);
}

TEST_CASE("完整性标签：目录同样可读") {
    auto dir = FreshDir("cnk_integrity_2");
    REQUIRE(SetLabelLow(dir));
    REQUIRE(cnk::FileIntegrityRid(dir.u8string()) == (long long)SECURITY_MANDATORY_LOW_RID);
    CleanDir(dir);
}

TEST_CASE("完整性标签：不存在的路径与空路径返回 -2") {
    auto dir = FreshDir("cnk_integrity_3");
    CHECK(cnk::FileIntegrityRid((dir / "missing.bin").u8string()) == -2);
    CHECK(cnk::FileIntegrityRid(std::string{}) == -2);
    CHECK(cnk::IntegrityRidName(-2).find("未知") != std::string::npos);
    CleanDir(dir);
}

TEST_CASE("进程权限上下文包含提升状态与完整性维度") {
    const std::string ctx = cnk::ProcessTokenContext();
    CHECK(ctx.find("进程") != std::string::npos);
    CHECK(ctx.find("完整性=") != std::string::npos);
}
