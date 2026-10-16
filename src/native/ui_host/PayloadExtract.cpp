#include "ui_host/PayloadExtract.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace cnk {

// ---------------------------------------------------------------- 编码工具

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

namespace {

// ---------------------------------------------------------------- ZIP 常量与读取

constexpr uint32_t kLocalSig = 0x04034b50u;
constexpr uint32_t kCdSig    = 0x02014b50u;
constexpr uint32_t kEocdSig  = 0x06054b50u;
constexpr uint32_t kZip64EocdLocatorSig = 0x07064b50u;

// 防御性上限：载荷是我们自己打的（约 30MB），异常值说明文件坏了/被构造了
constexpr uint32_t kMaxEntries = 20000;
constexpr unsigned long long kMaxTotalBytes = 512ull * 1024 * 1024;

uint16_t Rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t Crc32Init() { return 0xFFFFFFFFu; }
uint32_t Crc32Update(uint32_t state, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        state ^= p[i];
        for (int k = 0; k < 8; ++k)
            state = (state >> 1) ^ (0xEDB88320u & (uint32_t) - (int32_t)(state & 1u));
    }
    return state;
}
uint32_t Crc32Final(uint32_t state) { return state ^ 0xFFFFFFFFu; }

struct CdEntry {
    std::string name;
    uint32_t crc = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint32_t local_off = 0;
    uint32_t ext_attr = 0;
    uint16_t method = 0;
    uint16_t flags = 0;
};

// 解析中央目录（只读，不解压）。任何结构性问题都返回明确错误。
bool ParseCentralDirectory(const uint8_t* z, size_t n, std::vector<CdEntry>& out,
                           uint32_t& cd_off, uint32_t& cd_size, std::string& error) {
    out.clear();
    if (!z || n == 0) {
        error = "UI 载荷为空";
        return false;
    }
    // EOCD 在文件末尾（可能有注释），从后往前找最近的合法 EOCD
    if (n < 22) {
        error = "UI 载荷不是合法 ZIP：长度不足，找不到 EOCD";
        return false;
    }
    const size_t scan = std::min<size_t>(n, 22 + 65535);
    size_t eocd = SIZE_MAX;
    for (size_t back = 0; back + 22 <= n && back < scan; ++back) {
        const size_t pos = n - 22 - back;
        if (Rd32(z + pos) != kEocdSig) continue;
        const uint16_t comment_len = Rd16(z + pos + 20);
        if (pos + 22 + comment_len <= n) {
            eocd = pos;
            break;
        }
    }
    if (eocd == SIZE_MAX) {
        error = "UI 载荷不是合法 ZIP：找不到 EOCD（中央目录缺失或载荷被截断）";
        return false;
    }
    // ZIP64：本项目只支持经典 ZIP（载荷 < 4GB、条目 < 65535），必须明确拒绝而不是猜
    if (eocd >= 20 && Rd32(z + eocd - 20) == kZip64EocdLocatorSig) {
        error = "UI 载荷是 ZIP64 格式，本实现不支持（请用 scripts/embed_ui.ps1 重新打包）";
        return false;
    }
    const uint16_t disk_no = Rd16(z + eocd + 4);
    const uint16_t cd_disk_no = Rd16(z + eocd + 6);
    const uint16_t entries_disk = Rd16(z + eocd + 8);
    const uint16_t entries_total = Rd16(z + eocd + 10);
    cd_size = Rd32(z + eocd + 12);
    cd_off = Rd32(z + eocd + 16);
    if (disk_no != 0 || cd_disk_no != 0 || entries_disk != entries_total) {
        error = "UI 载荷是分卷 ZIP，本实现不支持";
        return false;
    }
    if (entries_total == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        error = "UI 载荷是 ZIP64 格式，本实现不支持（请用 scripts/embed_ui.ps1 重新打包）";
        return false;
    }
    if (entries_total > kMaxEntries) {
        error = "UI 载荷条目数异常（" + std::to_string(entries_total) + "）";
        return false;
    }
    if ((size_t)cd_off > n || (size_t)cd_size > n - (size_t)cd_off) {
        error = "UI 载荷中央目录越界（载荷损坏）";
        return false;
    }
    if ((size_t)cd_off > eocd) {
        error = "UI 载荷中央目录位置非法（载荷损坏）";
        return false;
    }

    size_t pos = cd_off;
    const size_t cd_end = (size_t)cd_off + (size_t)cd_size;
    for (uint16_t i = 0; i < entries_total; ++i) {
        if (pos + 46 > cd_end) {
            error = "UI 载荷中央目录条目越界（载荷损坏）";
            return false;
        }
        const uint8_t* e = z + pos;
        if (Rd32(e) != kCdSig) {
            error = "UI 载荷中央目录条目签名非法（载荷损坏）";
            return false;
        }
        CdEntry ce;
        ce.flags = Rd16(e + 8);
        ce.method = Rd16(e + 10);
        ce.crc = Rd32(e + 16);
        ce.comp_size = Rd32(e + 20);
        ce.uncomp_size = Rd32(e + 24);
        const uint16_t name_len = Rd16(e + 28);
        const uint16_t extra_len = Rd16(e + 30);
        const uint16_t comment_len = Rd16(e + 32);
        ce.ext_attr = Rd32(e + 38);
        ce.local_off = Rd32(e + 42);
        const size_t rec_len = (size_t)46 + name_len + extra_len + comment_len;
        if (pos + rec_len > cd_end) {
            error = "UI 载荷中央目录条目长度越界（载荷损坏）";
            return false;
        }
        ce.name.assign((const char*)(e + 46), name_len);
        out.push_back(std::move(ce));
        pos += rec_len;
    }
    return true;
}

// 条目名安全检查：拒绝绝对路径/盘符/`..` 穿越/Windows 非法字符。
// `\` 归一为 `/`；输出去掉冗余空段（连续斜杠、`./`）。
bool SafeEntryName(const std::string& raw, std::string& out, bool& is_dir,
                   std::string& error) {
    out.clear();
    is_dir = false;
    if (raw.empty()) {
        error = "UI 载荷含空名条目（载荷损坏）";
        return false;
    }
    is_dir = (raw.back() == '/' || raw.back() == '\\');
    if (raw.find('\0') != std::string::npos) {
        error = "UI 载荷条目名含 NUL 字符（拒绝）";
        return false;
    }
    std::string name = raw;
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    if (name[0] == '/') {
        error = "UI 载荷条目使用绝对路径（拒绝）：" + raw;
        return false;
    }
    if (name.size() >= 2 && name[1] == ':') {
        error = "UI 载荷条目使用盘符路径（拒绝）：" + raw;
        return false;
    }
    for (char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
            error = "UI 载荷条目名含 Windows 非法字符（拒绝）：" + raw;
            return false;
        }
    }
    // 逐段检查：`..` 一律拒绝（解压目录必须完全落在目标目录内）
    size_t i = 0;
    while (i <= name.size()) {
        const size_t slash = name.find('/', i);
        const std::string seg =
            name.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (seg == "..") {
            error = "UI 载荷条目含路径穿越（拒绝）：" + raw;
            return false;
        }
        if (!seg.empty() && seg != ".") {
            if (!out.empty()) out += '/';
            out += seg;
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    if (out.empty()) {
        error = "UI 载荷条目名为空或只有分隔符（拒绝）：" + raw;
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 内嵌载荷

EmbeddedPayload LoadEmbeddedPayload(std::string& error) {
    error.clear();
    EmbeddedPayload p;
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, L"UI_PAYLOAD", RT_RCDATA);
    if (!res) {
        error = "本可执行文件没有内嵌设置界面（精简单文件版）。"
                "完整版请用 build.ps1 -SingleExe 构建。";
        return p;
    }
    const DWORD bytes = SizeofResource(self, res);
    HGLOBAL glob = LoadResource(self, res);
    const void* data = glob ? LockResource(glob) : nullptr;
    if (!data || bytes == 0) {
        error = "读取内嵌 UI 载荷失败：资源存在但内容为空";
        return p;
    }
    p.data = data;
    p.size = bytes;
    p.present = true;

    // 版本戳：打包脚本写进 UI_PAYLOAD_ID 的「版本+载荷指纹」。
    // 没有它时退化为版本号（仍能在升级后重新解压，只是同一版本换了载荷不会失效）。
    HRSRC id_res = FindResourceW(self, L"UI_PAYLOAD_ID", RT_RCDATA);
    if (id_res) {
        const DWORD id_bytes = SizeofResource(self, id_res);
        HGLOBAL id_glob = LoadResource(self, id_res);
        const char* id_data = id_glob ? (const char*)LockResource(id_glob) : nullptr;
        if (id_data && id_bytes > 0) {
            std::string id(id_data, id_bytes);
            const size_t b = id.find_first_not_of(" \t\r\n");
            const size_t e = id.find_last_not_of(" \t\r\n");
            p.id = (b == std::string::npos) ? std::string() : id.substr(b, e - b + 1);
        }
    }
    if (p.id.empty()) p.id = VersionString();
    return p;
}

// ---------------------------------------------------------------- 中央目录

bool ListStoredZip(const void* zip, size_t size, std::vector<ZipEntryInfo>& out,
                   std::string& error) {
    out.clear();
    uint32_t cd_off = 0, cd_size = 0;
    std::vector<CdEntry> entries;
    if (!ParseCentralDirectory((const uint8_t*)zip, size, entries, cd_off, cd_size, error))
        return false;
    out.reserve(entries.size());
    for (const auto& e : entries) {
        ZipEntryInfo info;
        info.name = e.name;
        info.size = e.uncomp_size;
        info.stored = (e.method == 0) && ((e.flags & 0x0001u) == 0);
        out.push_back(std::move(info));
    }
    return true;
}

bool ExtractStoredZip(const void* zip, size_t size, const fs::path& dest_dir,
                      std::string& error) {
    error.clear();
    const uint8_t* z = (const uint8_t*)zip;
    std::vector<CdEntry> entries;
    uint32_t cd_off = 0, cd_size = 0;
    if (!ParseCentralDirectory(z, size, entries, cd_off, cd_size, error)) return false;

    std::error_code ec;
    if (!fs::is_directory(dest_dir, ec)) {
        error = "解压目标目录不存在：" + WideToUtf8(dest_dir.wstring());
        return false;
    }

    unsigned long long total = 0;
    for (const auto& e : entries) {
        std::string name;
        bool is_dir = false;
        if (!SafeEntryName(e.name, name, is_dir, error)) return false;

        if (e.flags & 0x0001u) {
            error = "UI 载荷含加密条目（拒绝）：" + name;
            return false;
        }
        if (e.method != 0) {
            error = "UI 载荷含压缩条目（method=" + std::to_string(e.method) +
                    "，拒绝）：" + name +
                    "；本构建只支持存储型 ZIP，请用 scripts/embed_ui.ps1 重新打包";
            return false;
        }
        if (((e.ext_attr >> 16) & 0xF000u) == 0xA000u) {
            error = "UI 载荷含符号链接条目（拒绝）：" + name;
            return false;
        }
        if (e.comp_size != e.uncomp_size) {
            error = "UI 载荷条目大小自相矛盾（拒绝）：" + name;
            return false;
        }
        total += e.uncomp_size;
        if (total > kMaxTotalBytes) {
            error = "UI 载荷解压后过大（超过 512MB），拒绝解压";
            return false;
        }

        const fs::path target = dest_dir / fs::u8path(name);
        if (is_dir) {
            fs::create_directories(target, ec);
            if (ec) {
                error = "无法创建目录：" + name + "（" + ec.message() + "）";
                return false;
            }
            continue;
        }

        // 本地头的额外字段长度与中央目录里的可能不同，必须以本地头为准算数据偏移
        const size_t lo = e.local_off;
        if (lo > size || size - lo < 30) {
            error = "UI 载荷条目本地头越界（载荷损坏）：" + name;
            return false;
        }
        if (Rd32(z + lo) != kLocalSig) {
            error = "UI 载荷条目本地头签名非法（载荷损坏）：" + name;
            return false;
        }
        const uint16_t local_name_len = Rd16(z + lo + 26);
        const uint16_t local_extra_len = Rd16(z + lo + 28);
        const size_t data_off = lo + 30 + (size_t)local_name_len + (size_t)local_extra_len;
        if (data_off > size || (size_t)e.comp_size > size - data_off) {
            error = "UI 载荷条目数据越界（载荷损坏）：" + name;
            return false;
        }

        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "无法创建目录：" + WideToUtf8(target.parent_path().wstring()) +
                    "（" + ec.message() + "）";
            return false;
        }
        std::ofstream ofs(target, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            error = "无法写入文件：" + name + "（" + WideToUtf8(target.wstring()) + "）";
            return false;
        }
        uint32_t crc = Crc32Init();
        size_t written = 0;
        const size_t chunk_max = 256 * 1024;
        while (written < (size_t)e.comp_size) {
            const size_t chunk = std::min(chunk_max, (size_t)e.comp_size - written);
            ofs.write((const char*)(z + data_off + written), (std::streamsize)chunk);
            if (!ofs) {
                error = "写入 UI 载荷条目失败：" + name;
                return false;
            }
            crc = Crc32Update(crc, z + data_off + written, chunk);
            written += chunk;
        }
        ofs.close();
        if (Crc32Final(crc) != e.crc) {
            // 半截文件不能留给用户当成品用：删掉它再报错
            std::error_code e2;
            fs::remove(target, e2);
            error = "UI 载荷条目 CRC 校验失败（载荷损坏）：" + name;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- 版本戳 / 释放

std::string ReadUiPayloadMarker(const fs::path& ui_dir) {
    std::ifstream ifs(ui_dir / L".version", std::ios::binary);
    if (!ifs) return {};
    std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, e - b + 1);
}

bool UiPayloadUsable(const fs::path& ui_dir, const std::string& payload_id) {
    if (payload_id.empty()) return false;
    if (ReadUiPayloadMarker(ui_dir) != payload_id) return false;
    std::error_code ec;
    if (!fs::is_regular_file(ui_dir / L"CampusNet.UI.dll", ec)) return false;
    if (!fs::is_regular_file(ui_dir / L"CampusNet.UI.runtimeconfig.json", ec)) return false;
    return true;
}

int CleanStaleUiDirs(const fs::path& base_dir) {
    int removed = 0;
    std::error_code ec;
    if (!fs::is_directory(base_dir, ec)) return 0;
    fs::directory_iterator it(base_dir, ec);
    if (ec) return 0;
    for (const auto& entry : it) {
        std::error_code e1;
        if (!entry.is_directory(e1)) continue;
        const std::string name = entry.path().filename().u8string();
        if (name.rfind("ui.tmp-", 0) != 0 && name.rfind("ui.old-", 0) != 0) continue;
        std::error_code e2;
        fs::remove_all(entry.path(), e2);
        if (!e2) ++removed;
    }
    return removed;
}

namespace {

// 失败路径也要把临时目录清掉（不留半成品让人误用）
struct TmpDirGuard {
    fs::path path;
    ~TmpDirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string UniqueSuffix() {
    return std::to_string((unsigned long)GetCurrentProcessId()) + "-" +
           std::to_string((unsigned long long)GetTickCount64());
}

}  // namespace

bool EnsureUiPayloadFromBytes(const void* zip, size_t size, const std::string& payload_id,
                              const fs::path& base_dir, std::string& ui_dir,
                              std::string& error) {
    error.clear();
    ui_dir.clear();
    if (base_dir.empty()) {
        error = "UI 载荷基准目录为空";
        return false;
    }
    if (payload_id.empty()) {
        error = "UI 载荷缺少版本戳（打包脚本未生成 UI_PAYLOAD_ID）";
        return false;
    }
    if (!zip || size == 0) {
        error = "UI 载荷为空";
        return false;
    }

    const fs::path dir = base_dir / L"ui";
    std::error_code ec;
    if (UiPayloadUsable(dir, payload_id)) {   // 版本戳命中：直接复用，连 zip 都不读
        ui_dir = dir.u8string();
        return true;
    }
    if (fs::exists(dir, ec) && !fs::is_directory(dir, ec)) {
        error = "UI 目标路径被同名文件占住：" + dir.u8string() + "（请删除该文件后重试）";
        return false;
    }
    if (!fs::is_directory(base_dir, ec)) {
        fs::create_directories(base_dir, ec);
        if (!fs::is_directory(base_dir, ec)) {
            error = "无法创建 UI 目录：" + base_dir.u8string() + "（" + ec.message() + "）";
            return false;
        }
    }

    const std::string suffix = UniqueSuffix();
    const fs::path tmp = base_dir / fs::u8path("ui.tmp-" + suffix);
    const fs::path old = base_dir / fs::u8path("ui.old-" + suffix);
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (!fs::is_directory(tmp, ec)) {
        error = "无法创建临时解压目录：" + tmp.u8string() + "（" + ec.message() + "）";
        return false;
    }
    TmpDirGuard guard{tmp};

    if (!ExtractStoredZip(zip, size, tmp, error)) return false;

    {
        std::ofstream ofs(tmp / L".version", std::ios::binary | std::ios::trunc);
        if (!ofs) {
            error = "无法写入版本戳：" + (tmp / L".version").u8string();
            return false;
        }
        ofs << payload_id;
        ofs.flush();
        if (!ofs) {
            error = "写入版本戳失败";
            return false;
        }
    }

    // 先解到临时目录、最后一步才改名：任何失败都不会留下"半个界面"
    const bool had_old = fs::is_directory(dir, ec);
    if (had_old) {
        std::error_code e2;
        fs::remove_all(old, e2);   // 理论上不存在，防御性清理
        fs::rename(dir, old, e2);
        if (e2) {
            error = "无法替换 UI 目录（旧版本界面可能正在运行）：" + dir.u8string() +
                    "（" + e2.message() + "）；请关闭设置界面后重试";
            return false;
        }
    }
    std::error_code e3;
    fs::rename(tmp, dir, e3);
    if (e3) {
        if (had_old) {
            std::error_code e4;
            fs::rename(old, dir, e4);   // 尽量回滚成原来那份可用的
        }
        error = "无法就位 UI 目录：" + dir.u8string() + "（" + e3.message() + "）";
        return false;
    }
    if (had_old) {
        std::error_code e5;
        fs::remove_all(old, e5);
        if (e5)
            LogWarn("旧 UI 目录未能删除（下次启动会重试）：" + old.u8string() +
                    "（" + e5.message() + "）");
    }
    ui_dir = dir.u8string();
    return true;
}

bool EnsureUiPayload(std::string& ui_dir, std::string& error) {
    error.clear();
    ui_dir.clear();

    std::string load_error;
    const EmbeddedPayload payload = LoadEmbeddedPayload(load_error);
    if (!payload.present) {
        error = load_error;
        return false;
    }

    const std::string base = LocalAppDataDir();
    if (base.empty()) {
        error = "无法创建 %LOCALAPPDATA%\\CampusNet-Keeper 目录（磁盘空间或权限问题），"
                "设置界面无法释放";
        return false;
    }
    const fs::path base_dir = fs::u8path(base);
    const fs::path dir = base_dir / L"ui";

    if (UiPayloadUsable(dir, payload.id)) {
        ui_dir = dir.u8string();
        LogInfo("复用已解压的 UI 载荷（戳 " + payload.id + "）");
        return true;
    }

    // 两个界面同时启动时，只让一个负责解压（避免互相改名）
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CampusNet-Keeper-UiPayload");
    bool owned = false;
    if (mutex) {
        const DWORD w = WaitForSingleObject(mutex, 120000);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
            owned = true;
        } else if (w == WAIT_TIMEOUT) {
            error = "等待其它界面进程释放 UI 载荷超时（120 秒）";
            CloseHandle(mutex);
            return false;
        } else {
            LogWarn("等待 UI 载荷互斥体失败（错误码 " + std::to_string(GetLastError()) +
                    "），继续但可能与其它实例竞争");
        }
    } else {
        LogWarn("创建 UI 载荷互斥体失败，继续但可能与其它实例竞争");
    }
    // 拿到锁后再看一次：等锁期间别人可能已经解好了
    if (UiPayloadUsable(dir, payload.id)) {
        ui_dir = dir.u8string();
        LogInfo("复用已解压的 UI 载荷（戳 " + payload.id + "）");
        if (owned) ReleaseMutex(mutex);
        if (mutex) CloseHandle(mutex);
        return true;
    }

    const int stale = CleanStaleUiDirs(base_dir);
    if (stale > 0) LogInfo("清理上次残留的 UI 临时目录：" + std::to_string(stale) + " 个");
    LogInfo("解压 UI 载荷到 " + dir.u8string() + "（戳 " + payload.id + "，" +
            std::to_string(payload.size) + " 字节）");

    const bool ok = EnsureUiPayloadFromBytes(payload.data, payload.size, payload.id,
                                             base_dir, ui_dir, error);
    if (ok) LogInfo("UI 载荷已就绪：" + ui_dir);
    if (owned) ReleaseMutex(mutex);
    if (mutex) CloseHandle(mutex);
    return ok;
}

}  // namespace cnk
