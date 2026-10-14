#include "core/Integrity.h"
#include <windows.h>
#include <vector>

namespace cnk {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

}  // namespace

std::string ProcessTokenContext() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return "未知";
    std::string s;
    TOKEN_ELEVATION el{};
    DWORD sz = 0;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz)) {
        s = el.TokenIsElevated ? "提升进程" : "普通进程";
    } else {
        s = "未知进程";
    }
    // 完整性级别（TokenIntegrityLevel）
    sz = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &sz);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), sz, &sz)) {
            auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
            const DWORD rid = *GetSidSubAuthority(
                label->Label.Sid,
                (DWORD)(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
            if (rid < SECURITY_MANDATORY_MEDIUM_RID) {
                s += "，完整性=低（受限环境）";
            } else if (rid < SECURITY_MANDATORY_HIGH_RID) {
                s += "，完整性=中";
            } else {
                s += "，完整性=高";
            }
        }
    }
    CloseHandle(tok);
    return s;
}

long long FileIntegrityRid(const std::string& utf8_path) {
    const std::wstring w = Utf8ToWide(utf8_path);
    if (w.empty()) return -2;

    // 先取所需大小：对象存在时即使没有 SACL 也会返回描述符（need > 0）；
    // 只有 ERROR_INSUFFICIENT_BUFFER 之外的失败（不存在/无权限）才算读取失败。
    DWORD need = 0;
    if (!GetFileSecurityW(w.c_str(), LABEL_SECURITY_INFORMATION, nullptr, 0, &need)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return -2;
    }
    if (need == 0) return -1;
    std::vector<BYTE> buf(need);
    if (!GetFileSecurityW(w.c_str(), LABEL_SECURITY_INFORMATION, buf.data(), need, &need))
        return -2;

    // GetFileSecurity 返回自相对格式描述符，可直接用 GetSecurityDescriptorSacl 解析
    auto* sd = reinterpret_cast<SECURITY_DESCRIPTOR*>(buf.data());
    BOOL present = FALSE, defaulted = FALSE;
    PACL sacl = nullptr;
    if (!GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted)) return -2;
    if (!present || sacl == nullptr) return -1;

    for (DWORD i = 0; i < sacl->AceCount; ++i) {
        LPVOID ace = nullptr;
        if (!GetAce(sacl, i, &ace) || ace == nullptr) break;
        auto* hdr = static_cast<ACE_HEADER*>(ace);
        if (hdr->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
        // 标签 ACE 的 SID 内联在 ACE 尾部（SidStart），最后一级子作者即完整性 RID
        auto* mla = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
        PSID sid = reinterpret_cast<PSID>(&mla->SidStart);
        const UCHAR cnt = *GetSidSubAuthorityCount(sid);
        if (cnt == 0) return -2;
        return *GetSidSubAuthority(sid, static_cast<DWORD>(cnt - 1));
    }
    return -1;
}

std::string IntegrityRidName(long long rid) {
    if (rid == -1) return "无标签（常规）";
    if (rid == -2) return "未知（读取失败）";
    if (rid < SECURITY_MANDATORY_MEDIUM_RID) return "低";
    if (rid < SECURITY_MANDATORY_HIGH_RID) return "中";
    return "高";
}

}  // namespace cnk
