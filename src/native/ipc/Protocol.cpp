#include "ipc/Protocol.h"

#include "json.hpp"

using nlohmann::json;

namespace cnk {

namespace {

// 取整数：只接受 JSON 整数。浮点/字符串/布尔都算类型不符——
// 用 j.get<int>() 或 value() 会在类型不符时抛异常，异常逃出服务线程
// 会把整个 IPC 服务打死（本地 UI 一次畸形请求即可）。
bool TakeInt(const json& j, const char* key, int& out, std::string& error) {
    const auto it = j.find(key);
    if (it == j.end()) {
        error = std::string("请求缺少 ") + key;
        return false;
    }
    if (!it->is_number_integer()) {
        error = std::string(key) + " 必须是整数";
        return false;
    }
    out = it->get<int>();
    return true;
}

}  // namespace

std::string IpcSerialize(const IpcResponse& r) {
    json j;
    j["id"] = r.id;
    j["ok"] = r.ok;
    if (r.ok) {
        // data 允许是非法 JSON：降级为 {} 而不是抛异常（handler 拼错也必须能应答）
        json data = json::parse(r.data_json, nullptr, false);
        j["data"] = data.is_discarded() ? json::object() : data;
    } else {
        j["error"] = r.error;
    }
    return j.dump(-1, ' ', false, json::error_handler_t::replace);  // 单行输出
}

bool IpcParseRequest(const std::string& line, IpcRequest& out, std::string& error) {
    error.clear();
    out = IpcRequest{};

    const json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "请求不是合法 JSON 对象";
        return false;
    }
    if (!TakeInt(j, "id", out.id, error)) return false;

    const auto cmd = j.find("cmd");
    if (cmd == j.end() || !cmd->is_string()) {
        error = "请求缺少字符串 cmd";
        return false;
    }
    out.cmd = cmd->get<std::string>();
    if (j.contains("args")) out.args_json = j["args"].dump();
    return true;
}

bool IpcParseResponse(const std::string& line, IpcResponse& out, std::string& error) {
    error.clear();
    out = IpcResponse{};

    const json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "响应不是合法 JSON 对象";
        return false;
    }
    // 逐字段显式判类型：键存在但类型不符时取默认值。
    // 这里刻意不用 value()（它会对类型不符抛 type_error）
    if (const auto it = j.find("id"); it != j.end() && it->is_number_integer())
        out.id = it->get<int>();
    if (const auto it = j.find("ok"); it != j.end() && it->is_boolean()) out.ok = it->get<bool>();
    if (const auto it = j.find("data"); it != j.end()) out.data_json = it->dump();
    if (const auto it = j.find("error"); it != j.end() && it->is_string())
        out.error = it->get<std::string>();
    return true;
}

}  // namespace cnk
