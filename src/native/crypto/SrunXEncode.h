#pragma once
#include <string>

namespace cnk {

// 深澜自定义码表的 base64，**无 padding**（码表见实现）。
std::string SrunBase64(const std::string& in);

// 深澜 xEncode：4 字节小端打包 → TEA 变体混淆 → 自定义 base64。
// 输入为空时返回空串。该函数没有官方测试向量，正确性依据是
// tests/crypto_test.cpp 里由独立实现（Node/JS 参考实现）导出的向量。
std::string SrunXEncode(const std::string& data, const std::string& key);

}  // namespace cnk
