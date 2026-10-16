#pragma once
#include <string>

namespace cnk {

// 管道基名（不含 \\.\pipe\ 前缀）。完整路径规则见 PipeNameForCurrentUser()。
constexpr const char* kPipeName = "CampusNetKeeper";

// 线协议：UTF-8、以 '\n' 分隔的单行 JSON。一个连接内可发多条请求，逐条应答。
//   请求：{"id":<整数>,"cmd":"<字符串>","args":<任意 JSON，缺省 {}>}
//   响应：{"id":<整数>,"ok":true,"data":<任意 JSON>}
//         {"id":<整数>,"ok":false,"error":"<字符串>"}
// 解析一律**不抛异常**：非法输入返回 false 并给出中文 error；
// 类型不符（如 id 是字符串）同样视为非法，绝不把异常放给调用方
// （服务线程里跑的是 IPC 解析，异常逃出去会把整个服务打死）。
struct IpcRequest {
    int id = 0;
    std::string cmd;
    std::string args_json = "{}";
};

struct IpcResponse {
    int id = 0;
    bool ok = false;
    std::string data_json = "{}";
    std::string error;
};

std::string IpcSerialize(const IpcResponse& r);
bool IpcParseRequest(const std::string& line, IpcRequest& out, std::string& error);
bool IpcParseResponse(const std::string& line, IpcResponse& out, std::string& error);

}  // namespace cnk
