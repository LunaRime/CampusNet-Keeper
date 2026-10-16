// 任务 26：内嵌 UI 载荷（存储型 ZIP）解析与释放。
//
// 规矩：本文件里的 ZIP 字节流由**测试自己**逐字节构造（不复用产品代码），
// 否则"写 ZIP 的代码"与"读 ZIP 的代码"同错同过，等于没验证。
#include "doctest.h"
#include "core/Paths.h"
#include "ui_host/PayloadExtract.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- 测试侧工具

// 独立实现的 CRC32（IEEE 802.3，ZIP 用的那个）。有 KAT 钉住它本身是对的。
uint32_t Crc32Of(const std::string& s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : s) {
        crc ^= c;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) - (int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFFu));
    v.push_back((uint8_t)((x >> 8) & 0xFFu));
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFFu));
}

// 逐字段构造 ZIP：默认全走"存储"（method=0），可注入压缩/加密/越界等形态。
struct ZipBuilder {
    struct Item {
        std::string name;
        std::string content;   // method=0 时的真实内容
        std::string raw;       // method!=0 时的原始字节（内容随便，反正必须被拒）
        std::string local_extra;   // 只写进本地头的额外字段（中央目录仍写 0）
        uint16_t method = 0;
        uint16_t flags = 0;
        uint32_t ext_attr = 0;
        bool crc_mismatch = false;  // 中央目录里故意写错 CRC
    };

    std::vector<Item> items;
    std::string comment;       // EOCD 之后的归档注释

    void Add(const std::string& name, const std::string& content) {
        Item it;
        it.name = name;
        it.content = content;
        items.push_back(it);
    }
    void AddRaw(const std::string& name, uint16_t method, const std::string& raw) {
        Item it;
        it.name = name;
        it.raw = raw;
        it.method = method;
        items.push_back(it);
    }
    void AddDir(const std::string& name) { Add(name, ""); }

    // 本地头的 extra 长度与中央目录不同（合法的 ZIP 形态）：数据偏移必须以本地头为准
    void AddWithLocalExtra(const std::string& name, const std::string& content,
                           const std::string& extra) {
        Item it;
        it.name = name;
        it.content = content;
        it.local_extra = extra;
        items.push_back(it);
    }

    // 只改中央目录里的 CRC（数据不动）→ 校验和必然对不上
    void AddWithBadCrc(const std::string& name, const std::string& content) {
        Item it;
        it.name = name;
        it.content = content;
        it.crc_mismatch = true;
        items.push_back(it);
    }

    std::vector<uint8_t> Build() const {
        struct Rec {
            uint32_t off = 0;
            uint32_t crc = 0;
            uint32_t size = 0;
            uint16_t method = 0;
            uint16_t flags = 0;
            uint32_t ext_attr = 0;
            std::string name;
        };
        std::vector<uint8_t> out;
        std::vector<Rec> recs;

        for (const auto& it : items) {
            const std::string& data = (it.method == 0) ? it.content : it.raw;
            Rec r;
            r.off = (uint32_t)out.size();
            r.crc = it.crc_mismatch ? (Crc32Of(data) ^ 0xDEADBEEFu) : Crc32Of(data);
            r.size = (uint32_t)data.size();
            r.method = it.method;
            r.flags = it.flags;
            r.ext_attr = it.ext_attr;
            r.name = it.name;

            Put32(out, 0x04034b50u);   // local file header
            Put16(out, 20);            // version needed
            Put16(out, it.flags);
            Put16(out, it.method);
            Put16(out, 0);             // time
            Put16(out, 0);             // date
            Put32(out, r.crc);
            Put32(out, r.size);        // compressed size
            Put32(out, r.size);        // uncompressed size
            Put16(out, (uint16_t)it.name.size());
            Put16(out, (uint16_t)it.local_extra.size());
            out.insert(out.end(), it.name.begin(), it.name.end());
            out.insert(out.end(), it.local_extra.begin(), it.local_extra.end());
            out.insert(out.end(), data.begin(), data.end());
            recs.push_back(r);
        }

        const uint32_t cd_off = (uint32_t)out.size();
        for (const auto& r : recs) {
            Put32(out, 0x02014b50u);   // central directory header
            Put16(out, 20);            // version made by
            Put16(out, 20);            // version needed
            Put16(out, r.flags);
            Put16(out, r.method);
            Put16(out, 0);
            Put16(out, 0);
            Put32(out, r.crc);
            Put32(out, r.size);
            Put32(out, r.size);
            Put16(out, (uint16_t)r.name.size());
            Put16(out, 0);             // extra
            Put16(out, 0);             // comment
            Put16(out, 0);             // disk start
            Put16(out, 0);             // internal attrs
            Put32(out, r.ext_attr);
            Put32(out, r.off);
            out.insert(out.end(), r.name.begin(), r.name.end());
        }
        const uint32_t cd_size = (uint32_t)out.size() - cd_off;

        Put32(out, 0x06054b50u);       // EOCD
        Put16(out, 0);
        Put16(out, 0);
        Put16(out, (uint16_t)recs.size());
        Put16(out, (uint16_t)recs.size());
        Put32(out, cd_size);
        Put32(out, cd_off);
        Put16(out, (uint16_t)comment.size());   // comment len
        out.insert(out.end(), comment.begin(), comment.end());
        return out;
    }
};

fs::path FreshDir(const char* name) {
    auto d = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

void CleanDir(const fs::path& d) {
    std::error_code ec;
    fs::remove_all(d, ec);
}

std::string ReadAll(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// base 下除 ui/ 之外是否还有残留（ui.tmp-*/ui.old-*）
std::vector<std::string> Leftovers(const fs::path& base) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        const std::string n = e.path().filename().u8string();
        if (n == "ui") continue;
        out.push_back(n);
    }
    return out;
}

std::string JoinLeftovers(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += x + " ";
    return s;
}

}  // namespace

// ---------------------------------------------------------------- 用例

TEST_CASE("测试自身的 CRC32 与已知向量一致") {
    // 防止"测试的 CRC 也算错"让 CRC 用例变成假绿
    CHECK(Crc32Of("123456789") == 0xCBF43926u);
    CHECK(Crc32Of("") == 0x00000000u);
}

TEST_CASE("载荷：正常解压多条目（含子目录、空文件、中文名）") {
    ZipBuilder b;
    b.Add("CampusNet.UI.dll", "FAKE-DLL-BYTES");
    b.Add("CampusNet.UI.runtimeconfig.json", "{\"runtimeOptions\":{}}");
    b.Add("sub/dir/inner.txt", "inner");
    b.Add("empty.txt", "");
    b.Add("中文目录/说明.txt", "中文内容");
    auto zip = b.Build();

    auto base = FreshDir("cnk_payload_ok");
    std::string ui_dir, err;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "0.1.0+abc", base, ui_dir, err));
    INFO("err=" << err);

    CHECK(ui_dir == (base / "ui").u8string());
    CHECK(ReadAll(base / "ui" / "CampusNet.UI.dll") == "FAKE-DLL-BYTES");
    CHECK(ReadAll(base / "ui" / "sub" / "dir" / "inner.txt") == "inner");
    CHECK(ReadAll(base / "ui" / "empty.txt").empty());
    // 中文条目名必须按 UTF-8 还原：MSVC 下 fs::path("中文…") 会按系统 ANSI 代码页解释
    // 窄字符串，所以测试这一侧也要走 u8path（产品侧同样用 u8path）。
    CHECK(ReadAll(base / "ui" / fs::u8path("中文目录") / fs::u8path("说明.txt")) == "中文内容");
    CHECK(cnk::ReadUiPayloadMarker(base / "ui") == "0.1.0+abc");
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：压缩条目必须明确报错而不是解出坏数据") {
    ZipBuilder b;
    b.Add("ok.txt", "ok");
    // method=8（deflate），内容故意不是合法 deflate 流：必须被"明确拒绝"，
    // 而不是写出垃圾文件（本项目的原则：宁缺勿错）
    b.AddRaw("compressed.bin", 8, "\x01\x02\x03\x04not-a-deflate-stream");
    auto zip = b.Build();

    auto base = FreshDir("cnk_payload_deflate");
    std::string ui_dir, err;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(Contains(err, "压缩"));
    CHECK(Contains(err, "compressed.bin"));
    CHECK_FALSE(fs::exists(base / "ui"));            // 不得留下半成品
    CHECK(Leftovers(base).empty());                  // 临时目录也要清掉
    CleanDir(base);
}

TEST_CASE("载荷：CRC 不符时拒绝，且旧目录保持原样") {
    auto base = FreshDir("cnk_payload_crc");

    ZipBuilder good;
    good.Add("CampusNet.UI.dll", "v1-dll");
    good.Add("keep.txt", "v1");
    auto zip1 = good.Build();
    std::string ui_dir, err;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip1.data(), zip1.size(), "v1", base, ui_dir, err));

    ZipBuilder bad;
    bad.Add("CampusNet.UI.dll", "v2-dll");
    bad.AddWithBadCrc("broken.txt", "v2");
    auto zip2 = bad.Build();
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip2.data(), zip2.size(), "v2", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(Contains(err, "CRC"));

    // 关键：失败不得破坏"现在能用的那份"——否则用户点一次设置界面反而把界面弄坏
    CHECK(cnk::ReadUiPayloadMarker(base / "ui") == "v1");
    CHECK(ReadAll(base / "ui" / "CampusNet.UI.dll") == "v1-dll");
    CHECK(ReadAll(base / "ui" / "keep.txt") == "v1");
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：缺中央目录/截断输入报明确错误") {
    auto base = FreshDir("cnk_payload_trunc");

    ZipBuilder b;
    b.Add("a.txt", "a");
    auto zip = b.Build();

    std::string ui_dir, err;
    // ① 空输入
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(nullptr, 0, "v1", base, ui_dir, err));
    CHECK_FALSE(err.empty());
    // ② 只剩本地头（没有中央目录与 EOCD）
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), 40, "v1", base, ui_dir, err));
    INFO("err(截断)=" << err);
    CHECK(Contains(err, "EOCD"));
    // ③ 完整长度但 EOCD 签名被破坏
    auto broken = zip;
    for (size_t i = broken.size() - 22; i < broken.size() - 18; ++i) broken[i] = 0;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(broken.data(), broken.size(), "v1", base, ui_dir, err));
    CHECK_FALSE(fs::exists(base / "ui"));
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：ZIP64 明确不支持") {
    auto base = FreshDir("cnk_payload_zip64");
    ZipBuilder b;
    b.Add("a.txt", "a");
    auto zip = b.Build();
    // 把 EOCD 里的中央目录偏移改成 0xFFFFFFFF（zip64 哨兵值）→ 必须明确拒绝
    const size_t eocd = zip.size() - 22;
    const size_t cd_off_field = eocd + 16;
    for (int i = 0; i < 4; ++i) zip[cd_off_field + (size_t)i] = 0xFF;

    std::string ui_dir, err;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(Contains(err, "ZIP64"));
    CHECK_FALSE(fs::exists(base / "ui"));
    CleanDir(base);
}

TEST_CASE("载荷：路径穿越与绝对路径条目被拒绝且不落盘") {
    const char* kBad[] = {
        "../evil.txt",
        "a/../../evil2.txt",
        "/abs.txt",
        "\\abs2.txt",
        "C:\\evil3.txt",
        "C:/evil4.txt",
    };
    for (const char* name : kBad) {
        auto base = FreshDir("cnk_payload_traversal");
        ZipBuilder b;
        b.Add(name, "evil");
        auto zip = b.Build();

        std::string ui_dir, err;
        CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
        INFO("name=" << name << " err=" << err);
        CHECK(Contains(err, "路径"));
        CHECK_FALSE(fs::exists(base / "evil.txt"));
        CHECK_FALSE(fs::exists(base / "evil2.txt"));
        CHECK_FALSE(fs::exists(base / "ui"));
        CHECK(Leftovers(base).empty());
        CleanDir(base);
    }
}

TEST_CASE("载荷：加密条目被拒绝") {
    auto base = FreshDir("cnk_payload_enc");
    ZipBuilder b;
    // 逐字段赋值而不是位置初始化：Item 加字段时位置初始化会把 0 绑到 std::string 上
    //（实测直接 SIGSEGV），这里刻意写得笨一点
    ZipBuilder::Item it;
    it.name = "secret.txt";
    it.content = "x";
    it.flags = 0x0001;   // bit0 = 加密
    b.items.push_back(it);
    auto zip = b.Build();

    std::string ui_dir, err;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(Contains(err, "加密"));
    CHECK_FALSE(fs::exists(base / "ui"));
    CleanDir(base);
}

TEST_CASE("载荷：版本戳一致时直接复用（连坏 ZIP 都不读）") {
    auto base = FreshDir("cnk_payload_reuse");

    ZipBuilder b;
    b.Add("CampusNet.UI.dll", "dll");
    b.Add("CampusNet.UI.runtimeconfig.json", "{}");
    auto zip = b.Build();
    std::string ui_dir, err;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "0.1.0+id1", base, ui_dir, err));
    // 用户/自检留下的哨兵：复用路径必须不动目录内容
    std::ofstream(base / "ui" / "sentinel.txt") << "keep-me";

    const std::string garbage = "this is definitely not a zip";
    std::string ui_dir2, err2;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(garbage.data(), garbage.size(), "0.1.0+id1",
                                          base, ui_dir2, err2));
    CHECK(ui_dir2 == (base / "ui").u8string());
    CHECK(ReadAll(base / "ui" / "sentinel.txt") == "keep-me");
    CHECK(cnk::ReadUiPayloadMarker(base / "ui") == "0.1.0+id1");
    CHECK(Leftovers(base).empty());

    // 版本戳不同但关键文件缺失（有人删了 dll）→ 不能复用
    std::error_code ec;
    fs::remove(base / "ui" / "CampusNet.UI.dll", ec);
    CHECK_FALSE(cnk::UiPayloadUsable(base / "ui", "0.1.0+id1"));
    CHECK(cnk::UiPayloadUsable(base / "ui", "other") == false);  // 戳不符也不复用
    CleanDir(base);
}

TEST_CASE("载荷：版本戳变化时重解并替换旧目录") {
    auto base = FreshDir("cnk_payload_upgrade");

    ZipBuilder v1;
    v1.Add("CampusNet.UI.dll", "dll-v1");
    v1.Add("CampusNet.UI.runtimeconfig.json", "{}");
    v1.Add("stale.txt", "old");
    auto zip1 = v1.Build();
    std::string ui_dir, err;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip1.data(), zip1.size(), "0.1.0+a", base, ui_dir, err));

    ZipBuilder v2;
    v2.Add("CampusNet.UI.dll", "dll-v2");
    v2.Add("CampusNet.UI.runtimeconfig.json", "{}");
    v2.Add("new.txt", "new");
    auto zip2 = v2.Build();
    std::string ui_dir2, err2;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip2.data(), zip2.size(), "0.1.0+b", base, ui_dir2, err2));

    CHECK(cnk::ReadUiPayloadMarker(base / "ui") == "0.1.0+b");
    CHECK(ReadAll(base / "ui" / "CampusNet.UI.dll") == "dll-v2");
    CHECK(fs::exists(base / "ui" / "new.txt"));
    CHECK_FALSE(fs::exists(base / "ui" / "stale.txt"));   // 旧内容必须被替换掉
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：目标被同名文件占住时报错且不改动它") {
    auto base = FreshDir("cnk_payload_blocked");
    std::ofstream(base / "ui") << "i am a file, not a directory";

    ZipBuilder b;
    b.Add("CampusNet.UI.dll", "dll");
    auto zip = b.Build();
    std::string ui_dir, err;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(Contains(err, "文件"));
    CHECK(fs::is_regular_file(base / "ui"));               // 不能把挡路的文件删掉
    CHECK(ReadAll(base / "ui") == "i am a file, not a directory");
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：CleanStaleUiDirs 只清 ui.tmp-*/ui.old-*") {
    auto base = FreshDir("cnk_payload_stale");
    fs::create_directories(base / "ui.tmp-123-456");
    fs::create_directories(base / "ui.old-123-456");
    fs::create_directories(base / "ui");
    std::ofstream(base / "ui" / "CampusNet.UI.dll") << "keep";
    std::ofstream(base / "unrelated.txt") << "keep";

    CHECK(cnk::CleanStaleUiDirs(base) == 2);
    CHECK(fs::exists(base / "ui" / "CampusNet.UI.dll"));   // 当前目录不能误删
    CHECK(fs::exists(base / "unrelated.txt"));
    CHECK(Leftovers(base).size() == 1);                    // 只剩 unrelated.txt
    CleanDir(base);
}

TEST_CASE("载荷：非法 UTF-8 条目名返回明确错误而不是抛异常（审阅 P1 回归）") {
    // MSVC 的 fs::u8path 遇到非法字节会抛 filesystem_error，而 --ui 是 CREATE_NO_WINDOW
    // 子进程、这条路径上没人接得住 → 用户"点了设置没反应、日志也没有一行"。
    auto base = FreshDir("cnk_payload_utf8");
    ZipBuilder b;
    b.Add("good.txt", "ok");
    b.Add(std::string("bad\xFF\xFE.txt", 9), "boom");
    b.Add(std::string("sub/\xC3.txt", 9), "truncated-multibyte");   // 截断的多字节序列
    auto zip = b.Build();

    std::string ui_dir, err;
    bool ok = true;
    REQUIRE_NOTHROW(ok = cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1",
                                                       base, ui_dir, err));
    CHECK_FALSE(ok);
    INFO("err=" << err);
    CHECK(Contains(err, "UTF-8"));
    CHECK_FALSE(fs::exists(base / "ui"));
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：NUL 名/尾点/保留设备名被拒绝（Windows 会改写它们）") {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {std::string("a\0b.txt", 7), "NUL"},      // 条目名里的 NUL
        {"trail.", "点或空格"},                    // 尾点：Windows 会悄悄去掉
        {"sub/trail ", "点或空格"},                // 尾空格：同上
        {"NUL", "设备名"},                         // 设备名：写到空设备上
        {"con.txt", "设备名"},                     // 设备名 + 扩展名同样是设备
        {"a/COM1.bin", "设备名"},
    };
    for (const auto& c : cases) {
        auto base = FreshDir("cnk_payload_badname");
        ZipBuilder b;
        b.Add(c.first, "x");
        auto zip = b.Build();
        std::string ui_dir, err;
        CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1",
                                                 base, ui_dir, err));
        INFO("name=" << c.first << " expect=" << c.second << " err=" << err);
        CHECK(Contains(err, c.second));
        CHECK_FALSE(fs::exists(base / "ui"));
        CHECK(Leftovers(base).empty());
        CleanDir(base);
    }
}

TEST_CASE("载荷：0 条目的 ZIP 被拒绝（干跑与解压都拒）") {
    ZipBuilder b;   // 一个条目都不加：只有 EOCD
    auto zip = b.Build();

    std::string err;
    CHECK_FALSE(cnk::VerifyStoredZip(zip.data(), zip.size(), err));
    INFO("err=" << err);
    CHECK(Contains(err, "没有任何条目"));

    auto base = FreshDir("cnk_payload_zero");
    std::string ui_dir, err2;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err2));
    CHECK(Contains(err2, "没有任何条目"));
    CHECK_FALSE(fs::exists(base / "ui"));
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：VerifyStoredZip 干跑校验（结构与 CRC，且不落盘）") {
    auto base = FreshDir("cnk_payload_dryrun");
    ZipBuilder good;
    good.Add("CampusNet.UI.dll", "dll");
    good.Add("CampusNet.UI.runtimeconfig.json", "{}");
    auto zip = good.Build();
    std::string err;
    CHECK(cnk::VerifyStoredZip(zip.data(), zip.size(), err));
    INFO("err=" << err);
    CHECK(err.empty());
    CHECK_FALSE(fs::exists(base / "ui"));      // 干跑不落盘

    ZipBuilder bad;
    bad.AddWithBadCrc("broken.txt", "x");
    auto zip2 = bad.Build();
    CHECK_FALSE(cnk::VerifyStoredZip(zip2.data(), zip2.size(), err));
    CHECK(Contains(err, "CRC"));

    ZipBuilder deflate;
    deflate.AddRaw("c.bin", 8, "xxxx");
    auto zip3 = deflate.Build();
    CHECK_FALSE(cnk::VerifyStoredZip(zip3.data(), zip3.size(), err));
    CHECK(Contains(err, "压缩"));
    CleanDir(base);
}

TEST_CASE("载荷：本地头额外字段与中央目录不同时，按本地头取数据") {
    auto base = FreshDir("cnk_payload_localextra");
    ZipBuilder b;
    b.AddWithLocalExtra("a.txt", "hello-local-extra", "EXTRA1234");
    auto zip = b.Build();
    std::string ui_dir, err;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(ReadAll(base / "ui" / "a.txt") == "hello-local-extra");
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：EOCD 之后的归档注释不影响解析") {
    auto base = FreshDir("cnk_payload_comment");
    ZipBuilder b;
    b.Add("a.txt", "with-comment");
    b.comment = "hello, this is an archive comment";
    auto zip = b.Build();
    std::string ui_dir, err;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(ReadAll(base / "ui" / "a.txt") == "with-comment");
    CleanDir(base);
}

TEST_CASE("载荷：注释里藏假 EOCD 时 fail-closed（报错，不解出坏数据）") {
    // 从后往前找 EOCD 的算法会先撞上注释里的假签名（字段全 0）→ 必须明确报错
    auto base = FreshDir("cnk_payload_fakeeocd");
    ZipBuilder b;
    b.Add("a.txt", "real");
    b.comment = std::string("\x50\x4b\x05\x06", 4) + std::string(18, '\0');
    auto zip = b.Build();
    std::string ui_dir, err;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK_FALSE(err.empty());
    CHECK_FALSE(fs::exists(base / "ui"));
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：目录条目（尾斜杠）建出目录；只有 './' 的条目被拒绝") {
    auto base = FreshDir("cnk_payload_direntry");
    ZipBuilder b;
    b.AddDir("sub/dir/");
    b.Add("sub/dir/inside.txt", "in");
    auto zip = b.Build();
    std::string ui_dir, err;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "v1", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(fs::is_directory(base / "ui" / "sub" / "dir"));
    CHECK(ReadAll(base / "ui" / "sub" / "dir" / "inside.txt") == "in");
    CleanDir(base);

    auto base2 = FreshDir("cnk_payload_dot");
    ZipBuilder d;
    d.Add("./", "");
    auto zip2 = d.Build();
    std::string ui_dir2, err2;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip2.data(), zip2.size(), "v1",
                                             base2, ui_dir2, err2));
    INFO("err=" << err2);
    CHECK(Contains(err2, "分隔符"));
    CHECK_FALSE(fs::exists(base2 / "ui"));
    CleanDir(base2);
}

TEST_CASE("载荷：版本戳为空时拒绝（不拿空戳去复用目录）") {
    auto base = FreshDir("cnk_payload_noid");
    ZipBuilder b;
    b.Add("a.txt", "a");
    auto zip = b.Build();
    std::string ui_dir, err;
    CHECK_FALSE(cnk::EnsureUiPayloadFromBytes(zip.data(), zip.size(), "", base, ui_dir, err));
    INFO("err=" << err);
    CHECK(Contains(err, "版本戳"));
    CHECK_FALSE(fs::exists(base / "ui"));
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}

TEST_CASE("载荷：ListStoredZip 报告条目名、大小与存储标记") {
    ZipBuilder b;
    b.Add("a.txt", "12345");
    b.AddRaw("b.bin", 8, "xxxx");
    auto zip = b.Build();

    std::vector<cnk::ZipEntryInfo> list;
    std::string err;
    REQUIRE(cnk::ListStoredZip(zip.data(), zip.size(), list, err));
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "a.txt");
    CHECK(list[0].size == 5);
    CHECK(list[0].stored);
    CHECK(list[1].name == "b.bin");
    CHECK_FALSE(list[1].stored);       // 诊断要如实报告，但不阻断列表
}

#ifdef CNK_UI_PAYLOAD_ZIP
TEST_CASE("载荷：真实 embed_ui.ps1 产物（存在时）全部是存储型且可解压") {
    const fs::path zip_path = CNK_UI_PAYLOAD_ZIP;
    if (!fs::exists(zip_path)) {
#ifdef CNK_REQUIRE_REAL_PAYLOAD
        // 完整版构建（build.ps1 -SingleExe 会打开这个开关）必须带着真实载荷：
        // 缺了就是构建形态错了，绝不能"静默跳过、显示全绿"（审阅 P2-3）
        FAIL("本构建声明要有真实 UI 载荷，但 " << zip_path.u8string() << " 不存在");
        return;
#else
        MESSAGE("跳过：尚未生成 UI 载荷（先跑 build.ps1 -SingleExe）；CI 无载荷属预期");
        return;
#endif
    }
    std::ifstream ifs(zip_path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 22);

    std::vector<cnk::ZipEntryInfo> list;
    std::string err;
    REQUIRE(cnk::ListStoredZip(bytes.data(), bytes.size(), list, err));
    INFO("err=" << err);
    CHECK(list.size() > 3);
    for (const auto& e : list) {
        INFO("entry=" << e.name << " size=" << e.size);
        CHECK(e.stored);              // .NET 的 NoCompression 必须真的产出存储型条目
    }
    // 干跑校验（结构与 CRC）也要过：这正是 --ui-info 与 build.ps1 的形态断言
    CHECK(cnk::VerifyStoredZip(bytes.data(), bytes.size(), err));
    INFO("verify err=" << err);

    auto base = FreshDir("cnk_payload_real");
    std::string ui_dir, err2;
    REQUIRE(cnk::EnsureUiPayloadFromBytes(bytes.data(), bytes.size(), "real", base, ui_dir, err2));
    INFO("err=" << err2);
    CHECK(fs::exists(base / "ui" / "CampusNet.UI.dll"));
    CHECK(fs::exists(base / "ui" / "CampusNet.UI.runtimeconfig.json"));
    CHECK(Leftovers(base).empty());
    CleanDir(base);
}
#endif

TEST_CASE("载荷：本地应用数据目录路径可用且不依赖创建成功") {
    // 沙箱里可能建不了目录，但路径本身必须能算出来（不含临时占位）
    const std::string p = cnk::LocalAppDataPath();
    INFO("path=" << p);
    CHECK_FALSE(p.empty());
    CHECK(Contains(p, "CampusNet-Keeper"));
}
