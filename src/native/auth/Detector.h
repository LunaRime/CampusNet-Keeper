#pragma once
#include <string>
#include "core/Http.h"

namespace cnk {

enum class Vendor { Unknown, Drcom, Eportal, Srun, Huawei };

// 按 spec §7 的顺序判定：Dr.COM 必须排在锐捷之前，
// 因为两者共用 /eportal/ 路径。响应体按 Content-Type 声明的编码
// 解码为 UTF-8 后再匹配中文指纹。
Vendor DetectVendor(const HttpResponse& resp);

const char* VendorName(Vendor v);
Vendor VendorFromString(const std::string& s);

}  // namespace cnk
