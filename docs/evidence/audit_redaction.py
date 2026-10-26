#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""仓库脱敏审计（提交前 / CI 必跑）。

背景（2026-10-17 独立审阅 P1，真实教训）：
  原脚本只扫 `docs/evidence/`，于是"文本门禁全绿"的同一次运行里，`docs/design/` 下的
  一张真机截图正含着真实学号与门户地址——**栅格图里的敏感信息对文本模式原理上不可见**。
  所以现在的审计分两段，缺一不可：

  A) 内容扫描
     · `docs/evidence/`：沿用原有的四类通用模式（十六进制 token / 点分地址 / 12 位数字串）；
     · 整个仓库的**文本文件**：再扫一遍高信号模式（学号样数字串、MAC、私有网段地址），
       排除 `.git` / `build*` / `dist` / `build-ui` / `third_party` 等目录。
  B) 资产清单
     仓库内所有图片/压缩包类文件必须登记在 `SAFE_ASSETS` 白名单里（新增截图必须先**人工看图**
     确认是合成数据再登记）——这样"往仓库里塞一张含真实数据的截图"会直接让门禁失败，
     而不是像上次那样静默通过。

设计原则：脚本内不写任何真实值（避免审计工具本身成为泄漏源）；允许清单仅含归一化占位符与
既定的公开值，其余命中一律列出交人工复核。

用法：python docs/evidence/audit_redaction.py   （在任意目录均可）
退出码：0=通过；1=有待复核项。
"""
import glob
import os
import re
import sys

# ---------------------------------------------------------------- 允许清单
# 全部为归一化占位符或既定公开值
SAFE_HEX8 = {
    "0a000064",  # 归一化后的客户端 IP（10.0.0.100）之十六进制
    "ac120106",  # 门户服务器地址（10.0.0.1）之十六进制——按策略保留
}
SAFE_HEX12 = {
    "000000000000",  # 设备标识清零占位
    "202400000000",  # 脱敏账号占位
}
SAFE_QUADS = {
    "10.0.0.1", "10.0.0.100", "192.168.100.100", "10.10.10.10",
    "0.0.0.0", "000.000.000.000", "1.1.1.1", "127.0.0.1",
    # 文档/单测里的示例地址（RFC1918 样例，非真实环境）
    "10.0.0.5", "10.0.0.9", "10.1.2.3",
    # OpenWrt 出厂默认 LAN 网关：全世界每一台刷了 OpenWrt 的设备都是它，
    # 不携带任何院校信息（router/scripts/install_openwrt.sh 的示例）
    "192.168.1.1",
}
SAFE_NUM12 = {"202400000000", "000000000000"}
# 仓库内允许长期保留的二进制资产（新增前必须人工看图确认是合成/公开内容）
SAFE_ASSETS = {
    "assets/app.ico",
    "assets/app-preview.png",
    "docs/design/ui-shell-20261016.png",
    "docs/design/ui-settings-20261016.png",
    "docs/design/ui-task25-20261016.png",
    "docs/design/ui-real-daemon-20261017-redacted.png",  # 真机截图，已打码
}
SAFE_MACS = {"00-00-00-00-00-00", "00:00:00:00:00:00"}
SAFE_STUDENTISH = {
    "202400000000", "202400000001",          # 单测/自检里的合成账号
    "20230000000",                           # spec 里的示例学号
}
# 审计脚本自身不参与内容扫描：白名单里的占位值本来就会被自己的模式命中
SELF_PATH = "docs/evidence/audit_redaction.py"

# ---------------------------------------------------------------- 模式（通用，不含真实值）
RE_HEX12 = re.compile(rb"(?<![0-9A-Fa-f])([0-9A-Fa-f]{12})(?![0-9A-Fa-f])")
RE_HEX8 = re.compile(rb"(?<![0-9A-Fa-f])([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")
RE_QUAD = re.compile(rb"\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}")
RE_NUM12 = re.compile(rb"(?<![0-9A-Za-z])(\d{12})(?![0-9A-Za-z])")
# 纯数字且形如 19xx/20xx + 月 + 日 的 8 位 token 视为公开日期（门户页面注释/文件名常见）
RE_DATE8 = re.compile(r"(19|20)\d{2}(0[1-9]|1[0-2])(0[1-9]|[12]\d|3[01])")
# 高信号模式（仓库全文扫描用）
RE_STUDENTISH = re.compile(rb"(?<![0-9A-Za-z])((?:19|20)\d{2}\d{6,8})(?![0-9A-Za-z])")
RE_MAC = re.compile(rb"(?i)(?<![0-9a-f])([0-9a-f]{2}(?:[:-][0-9a-f]{2}){5})(?![0-9a-f])")
RE_PRIVATE_QUAD = re.compile(rb"\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}")

ASSET_SUFFIXES = (".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp", ".zip", ".7z")
SKIP_DIRS = {".git", "build", "build-m1", "build-ui", "dist", "third_party",
             ".workbuddy", ".superpowers", "node_modules", ".vs"}

EVIDENCE_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(EVIDENCE_DIR))


def looks_like_date8(token):
    return RE_DATE8.fullmatch(token) is not None


def is_private_quad(token):
    parts = token.split(".")
    if len(parts) != 4 or not all(p.isdigit() for p in parts):
        return False
    a, b = int(parts[0]), int(parts[1])
    if a == 10:
        return True
    if a == 172 and 16 <= b <= 31:
        return True
    if a == 192 and b == 168:
        return True
    return False


def is_text(data):
    return b"\x00" not in data[:8192]


def scan_evidence(data):
    """docs/evidence/ 的原有四类模式。"""
    findings = []
    for m in RE_HEX12.finditer(data):
        t = m.group(1).decode()
        if t.lower() not in SAFE_HEX12:
            findings.append(("hex12", t))
    for m in RE_HEX8.finditer(data):
        t = m.group(1).decode()
        if t.lower() in SAFE_HEX8:
            continue
        if t.isdigit() and looks_like_date8(t):
            continue  # 公开日期串
        findings.append(("hex8", t))
    for m in RE_QUAD.finditer(data):
        t = m.group().decode()
        if t not in SAFE_QUADS:
            findings.append(("quad", t))
    for m in RE_NUM12.finditer(data):
        t = m.group(1).decode()
        if t not in SAFE_NUM12:
            findings.append(("num12", t))
    return findings


def scan_repo_text(data):
    """仓库全文的高信号模式：学号样数字串 / MAC / 私有网段地址。"""
    findings = []
    for m in RE_STUDENTISH.finditer(data):
        t = m.group(1).decode()
        if t in SAFE_STUDENTISH:
            continue
        if looks_like_date8(t):
            continue
        findings.append(("student-ish", t))
    for m in RE_MAC.finditer(data):
        t = m.group(1).decode()
        if t.lower() in SAFE_MACS:
            continue
        findings.append(("mac", t))
    for m in RE_PRIVATE_QUAD.finditer(data):
        t = m.group().decode()
        if t in SAFE_QUADS or not is_private_quad(t):
            continue
        findings.append(("private-ip", t))
    return findings


def rel_posix(path):
    return os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")


def skip(path):
    rel = os.path.relpath(path, REPO_ROOT)
    return any(part in SKIP_DIRS for part in rel.split(os.sep))


def main():
    total = 0
    for path in sorted(glob.glob(os.path.join(REPO_ROOT, "**", "*"), recursive=True)):
        if os.path.isdir(path) or skip(path):
            continue
        rel = rel_posix(path)
        data = open(path, "rb").read()

        # B) 资产清单：图片/压缩包必须登记（栅格图里的敏感信息文本模式扫不到，只能靠登记+看图）
        if rel.lower().endswith(ASSET_SUFFIXES):
            if rel not in SAFE_ASSETS:
                print("%-44s %-6s %s" % (rel, "asset", "未登记资产：请人工看图确认无真实数据后加入 SAFE_ASSETS"))
                total += 1
            continue

        # A) 内容扫描
        if rel == SELF_PATH:
            continue
        if rel.startswith("docs/evidence/"):
            for kind, token in scan_evidence(data):
                print("%-44s %-6s %s" % (rel, kind, token))
                total += 1
        if is_text(data):
            for kind, token in scan_repo_text(data):
                print("%-44s %-6s %s" % (rel, kind, token))
                total += 1

    print("-" * 60)
    if total:
        print("待人工复核 %d 项：判定为占位符/公开值则放行，否则先修复再提交。" % total)
        print("⚠ 图片类命中是「未登记资产」：文本模式看不见图里的内容，必须逐张打开确认。")
        return 1
    print("通过：未发现待复核项。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
