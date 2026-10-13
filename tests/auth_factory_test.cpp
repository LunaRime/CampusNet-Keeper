#include "doctest.h"
#include "auth/AuthFactory.h"

TEST_CASE("按厂商创建对应策略") {
    cnk::Profile p;
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Drcom, p)->Name()) == "DrcomAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Eportal, p)->Name()) == "EportalAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Srun, p)->Name()) == "SrunAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Huawei, p)->Name()) == "GenericFormAuth");
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Unknown, p)->Name()) == "GenericFormAuth");
}

TEST_CASE("vendor=custom 时创建自定义模板策略") {
    cnk::Profile p;
    p.vendor = "custom";
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Drcom, p)->Name()) == "CustomTemplateAuth");
}

TEST_CASE("vendor=generic 时强制通用表单策略") {
    // spec §5.1 的 vendor 取值含 "generic"，语义是用户强制走通用表单；
    // 它必须覆盖自动识别结果，而非被当作无法解析而回退到 detected。
    cnk::Profile p;
    p.vendor = "generic";
    CHECK(std::string(cnk::CreateAuth(cnk::Vendor::Drcom, p)->Name()) == "GenericFormAuth");
}

TEST_CASE("CreateAuth 永不返回 nullptr") {
    cnk::Profile p;
    for (auto v : {cnk::Vendor::Unknown, cnk::Vendor::Drcom, cnk::Vendor::Eportal,
                   cnk::Vendor::Srun, cnk::Vendor::Huawei})
        CHECK(cnk::CreateAuth(v, p) != nullptr);
}
