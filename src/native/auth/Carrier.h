#pragma once
#include <string>
#include <vector>

namespace cnk {

struct CarrierOption {
    std::string id;
    std::string name;
    std::string suffix;
};

struct CarrierList {
    std::string title;
    std::vector<CarrierOption> options;
    std::string default_id;
};

// 从页面文本（UTF-8）中提取 carrier='{...}' 并解析。找不到或解析失败返回 false。
// 列表必须来自服务器下发，不得用内置预设替代（spec §4.4）。
bool ParseCarrier(const std::string& utf8_body, CarrierList& out);

// default_id 命中项的下标；未命中返回 0；空列表返回 -1。
int DefaultCarrierIndex(const CarrierList& list);

}  // namespace cnk
