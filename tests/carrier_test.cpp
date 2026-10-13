#include "doctest.h"
#include "auth/Carrier.h"

// 与实测环境一致的 carrier 配置
static const char* kPage =
    "<script>carrier='{\"yys\":{\"title\": \"服务类型\",\"mode\":\"radiobutton\","
    "\"type\":\"0\",\"data\":[{\"id\":\"1\",\"name\":\"校园用户\",\"suffix\":\"\"},"
    "{\"id\":\"2\",\"name\":\"校园电信\",\"suffix\":\"@dx\"},"
    "{\"id\":\"3\",\"name\":\"校园联通\",\"suffix\":\"@lt\"},"
    "{\"id\":\"4\",\"name\":\"校园其他\",\"suffix\":\"\"}],"
    "\"defaultID\":\"1\"}}';</script>";

TEST_CASE("解析实测的 carrier 配置") {
    cnk::CarrierList list;
    REQUIRE(cnk::ParseCarrier(kPage, list));
    CHECK(list.title == "服务类型");
    REQUIRE(list.options.size() == 4);
    CHECK(list.options[0].name == "校园用户");
    CHECK(list.options[0].suffix == "");
    CHECK(list.options[1].name == "校园电信");
    CHECK(list.options[1].suffix == "@dx");
    CHECK(list.options[2].name == "校园联通");
    CHECK(list.options[2].suffix == "@lt");
    CHECK(list.options[3].name == "校园其他");
    CHECK(list.default_id == "1");
}

TEST_CASE("无 carrier 时返回 false 而非崩溃") {
    cnk::CarrierList list;
    CHECK_FALSE(cnk::ParseCarrier("<html>nothing</html>", list));
    CHECK(list.options.empty());
}

TEST_CASE("carrier 内容为非法 JSON 时返回 false") {
    cnk::CarrierList list;
    CHECK_FALSE(cnk::ParseCarrier("carrier='{not json';", list));
}

TEST_CASE("DefaultCarrierIndex 返回默认项下标") {
    cnk::CarrierList list;
    REQUIRE(cnk::ParseCarrier(kPage, list));
    CHECK(cnk::DefaultCarrierIndex(list) == 0);
}

TEST_CASE("数值型 defaultID 与 id 不崩溃且被转为字符串") {
    // 服务器可能下发数值型字段（JSON 合法变体）；早期实现会因
    // nlohmann value() 的类型检查抛出未捕获异常并终止进程（审阅实测复现）
    cnk::CarrierList list;
    REQUIRE(cnk::ParseCarrier(
        "carrier='{\"yys\":{\"title\":\"T\",\"data\":[{\"id\":1,"
        "\"name\":\"A\",\"suffix\":\"\"}],\"defaultID\":1}}';", list));
    REQUIRE(list.options.size() == 1);
    CHECK(list.options[0].id == "1");
    CHECK(list.default_id == "1");
    CHECK(cnk::DefaultCarrierIndex(list) == 0);
}
