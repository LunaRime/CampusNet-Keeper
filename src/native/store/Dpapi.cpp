#include "store/Dpapi.h"
#include <windows.h>
#include <dpapi.h>

namespace cnk {

namespace {

// 固定 entropy：使密文绑定本程序，拷贝到别处也无法解密
const char kEntropy[] = "CampusNetKeeper.v1";

}  // namespace

bool DpapiProtect(const std::string& plain, std::string& blob) {
    blob.clear();
    if (plain.empty()) return true;

    // CryptProtectData 不修改输入，但 API 声明为非 const，故去 const
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain.data()));
    in.cbData = (DWORD)plain.size();

    DATA_BLOB entropy{};
    entropy.pbData = (BYTE*)kEntropy;
    entropy.cbData = (DWORD)(sizeof(kEntropy) - 1);

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"CampusNetKeeper", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    blob.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool DpapiUnprotect(const std::string& blob, std::string& plain) {
    plain.clear();
    if (blob.empty()) return true;

    // CryptUnprotectData 不修改输入，但 API 声明为非 const，故去 const
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(blob.data()));
    in.cbData = (DWORD)blob.size();

    DATA_BLOB entropy{};
    entropy.pbData = (BYTE*)kEntropy;
    entropy.cbData = (DWORD)(sizeof(kEntropy) - 1);

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    plain.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

}  // namespace cnk
