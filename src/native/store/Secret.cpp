#include "store/Secret.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace cnk {

namespace {

bool EnsureParentDir(const std::string& path) {
    std::error_code ec;
    const fs::path parent = fs::u8path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    return !ec;
}

}  // namespace

std::string SecretPath() {
    const std::string base = LocalAppDataPath();
    if (base.empty()) return {};
    return (fs::u8path(base) / "secret.json").u8string();
}

bool SecretLoadFrom(const std::string& path, SecretMap& out, std::string& error) {
    out.clear();
    error.clear();
    if (path.empty()) {
        error = "密码库路径为空";
        return false;
    }

    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) return true;  // 首次运行：空库

    std::ifstream ifs(fs::u8path(path), std::ios::binary);
    if (!ifs) {
        error = "无法读取密码库";
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) return true;  // 空文件按空库处理（等价于没写过）

    try {
        const json j = json::parse(content);
        if (!j.is_object()) {
            error = "密码库根节点不是对象";
            return false;
        }
        if (j.contains("profiles") && j["profiles"].is_object()) {
            for (auto it = j["profiles"].begin(); it != j["profiles"].end(); ++it) {
                if (!it.value().is_object()) continue;
                const std::string enc =
                    it.value().value("enc", std::string{});
                if (!enc.empty()) out[it.key()] = enc;
            }
        }
    } catch (const std::exception& e) {
        error = std::string("密码库解析失败: ") + e.what();
        out.clear();
        return false;
    }
    return true;
}

bool SecretSaveTo(const std::string& path, const SecretMap& secrets,
                  std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "密码库路径为空";
        return false;
    }
    if (!EnsureParentDir(path)) {
        error = "无法创建密码库目录";
        return false;
    }

    json j;
    j["version"] = 1;
    j["profiles"] = json::object();
    for (const auto& [name, enc] : secrets) {
        if (enc.empty()) continue;
        j["profiles"][name] = {{"enc", enc}};
    }

    std::string text;
    try {
        text = j.dump(2);
    } catch (const std::exception& e) {
        error = std::string("密码库序列化失败: ") + e.what();
        return false;
    }

    // 与配置文件同样的原子替换：先 .tmp 再 rename，中断不会留半截文件
    const fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::error_code e2;
        fs::remove(tmp, e2);
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            error = "无法写入临时密码库";
            return false;
        }
        ofs << text;
        ofs.flush();
        if (!ofs) {
            std::error_code e3;
            fs::remove(tmp, e3);
            error = "写入临时密码库失败";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, fs::u8path(path), ec);
    if (ec) {
        std::error_code e2;
        fs::remove(tmp, e2);
        error = "替换密码库失败: " + ec.message();
        return false;
    }
    return true;
}

bool SecretLoad(SecretMap& out, std::string& error) {
    return SecretLoadFrom(SecretPath(), out, error);
}

bool SecretSave(const SecretMap& secrets, std::string& error) {
    return SecretSaveTo(SecretPath(), secrets, error);
}

}  // namespace cnk
