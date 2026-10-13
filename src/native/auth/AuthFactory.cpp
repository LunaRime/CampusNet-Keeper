#include "auth/AuthFactory.h"

namespace cnk {

// 各策略的构造器：任务 10 先由 Stubs.cpp 提供最小桩，
// 任务 11-21 逐个替换为真实实现（Stubs.cpp 随之收缩直至删除）。
std::unique_ptr<IAuth> MakeDrcomAuth();
std::unique_ptr<IAuth> MakeEportalAuth();
std::unique_ptr<IAuth> MakeSrunAuth();
std::unique_ptr<IAuth> MakeGenericFormAuth();
std::unique_ptr<IAuth> MakeCustomTemplateAuth();

std::unique_ptr<IAuth> CreateAuth(Vendor detected, const Profile& profile) {
    if (profile.vendor == "custom") return MakeCustomTemplateAuth();
    if (profile.vendor == "generic") return MakeGenericFormAuth();

    Vendor v = detected;
    if (profile.vendor != "auto") {
        Vendor forced = VendorFromString(profile.vendor);
        if (forced != Vendor::Unknown) v = forced;
    }

    switch (v) {
        case Vendor::Drcom:   return MakeDrcomAuth();
        case Vendor::Eportal: return MakeEportalAuth();
        case Vendor::Srun:    return MakeSrunAuth();
        case Vendor::Huawei:
        case Vendor::Unknown:
        default:              return MakeGenericFormAuth();
    }
}

}  // namespace cnk
