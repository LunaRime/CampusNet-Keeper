#pragma once

// 构建戳由 CMake 注入（见根 CMakeLists.txt）；未注入时留 unknown，绝不编造来源。
#ifndef CNK_BUILD_COMMIT
#define CNK_BUILD_COMMIT "unknown"
#endif
#ifndef CNK_BUILD_TIME
#define CNK_BUILD_TIME "unknown"
#endif

namespace cnk {

inline const char* VersionString() { return "0.1.9"; }

// 托盘菜单只展示短哈希，构建时间放在 --version 里，避免菜单项过长
inline const char* BuildCommitString() { return CNK_BUILD_COMMIT; }

// 可追溯性：对齐 router 端的 CNK_BUILD_ID（2026-11-10）。
// --version 第一行仍是纯版本号，package.ps1 的包含式匹配与 ci.yml 的
// VersionString 正则都不受影响；第二行给出 git 短哈希与 UTC 构建时间。
inline const char* BuildStampString() {
    return "git " CNK_BUILD_COMMIT ", built " CNK_BUILD_TIME;
}

}  // namespace cnk
