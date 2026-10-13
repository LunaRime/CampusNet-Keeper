#pragma once
#include <string>
#include <map>

namespace cnk {

// 把 tpl 中的 {key} 替换为 vars 中对应值。
// vars 中不存在的占位符保持原样（便于用户发现拼写错误）。
// 注意：替换值原样写入，不做 URL/表单编码——编码格式由模板作者自行控制
// （如需编码值，请在模板中提供已编码的占位符或自行拼装）。
std::string ExpandTemplate(const std::string& tpl,
                           const std::map<std::string, std::string>& vars);

}  // namespace cnk
