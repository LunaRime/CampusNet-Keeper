#pragma once
#include <string>
#include "core/Http.h"

namespace cnk {

// 从 Content-Type 中提取字符集名（小写）。无 charset 参数时返回空串。
// 支持 `charset=gbk`、`charset="gb2312"`、混合大小写与多余空白。
std::string ParseCharset(const std::string& content_type);

// 把 bytes 从 charset 编码转为 UTF-8。空 charset 按 UTF-8 处理。
// 非法字节序列不崩溃：解码失败时原样返回输入。
std::string ToUtf8(const std::string& bytes, const std::string& charset);

// 便捷函数：解析响应头中的 charset 并转 UTF-8。
std::string DecodeBody(const HttpResponse& resp);

}  // namespace cnk
