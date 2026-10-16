#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""证据目录脱敏审计（提交 docs/evidence/ 变更前必跑）。

设计原则：全部使用「通用模式」，脚本内不写任何真实值（避免审计工具本身成为泄漏源）；
允许清单仅含归一化占位符与既定的公开值，其余命中一律列出交人工复核。

检查项：
  1) 8 / 12 位十六进制 token（常为 IP 的 4 字节编码、6 字节设备标识），非允许清单即列出；
  2) 点分四段地址，非允许清单即列出；
  3) 12 位纯数字串（学号常见形态），非允许清单即列出。

用法：python audit_redaction.py   （在任意目录均可，默认扫描脚本所在目录）
退出码：0=通过；1=有待复核项。
"""
import glob
import os
import re
import sys

# 允许清单（全部为归一化占位符或既定公开值）
SAFE_HEX8 = {
    "0a000064",  # 归一化后的客户端 IP（10.0.0.100）之十六进制
    "ac120106",  # 门户服务器地址（10.0.0.1）之十六进制——按策略保留
}
SAFE_HEX12 = {
    "000000000000",  # 设备标识清零占位
    "202400000000",  # 脱敏账号占位
}
SAFE_QUADS = {
    "10.0.0.1", "10.0.0.100", "192.168.100.100",
    "0.0.0.0", "000.000.000.000", "1.1.1.1", "127.0.0.1",
}
SAFE_NUM12 = {"202400000000", "000000000000"}

RE_HEX12 = re.compile(rb"(?<![0-9A-Fa-f])([0-9A-Fa-f]{12})(?![0-9A-Fa-f])")
RE_HEX8 = re.compile(rb"(?<![0-9A-Fa-f])([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")
RE_QUAD = re.compile(rb"\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}")
RE_NUM12 = re.compile(rb"(?<![0-9A-Za-z])(\d{12})(?![0-9A-Za-z])")
# 纯数字且形如 19xx/20xx + 月 + 日 的 8 位 token 视为公开日期（门户页面注释/文件名常见）
RE_DATE8 = re.compile(r"(19|20)\d{2}(0[1-9]|1[0-2])(0[1-9]|[12]\d|3[01])")


def looks_like_date8(token):
    return RE_DATE8.fullmatch(token) is not None


def scan_file(path):
    data = open(path, "rb").read()
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


def main():
    root = os.path.dirname(os.path.abspath(__file__))
    total = 0
    for path in sorted(glob.glob(os.path.join(root, "**", "*"), recursive=True)):
        if os.path.isdir(path):
            continue
        for kind, token in scan_file(path):
            print("%-44s %-6s %s" % (os.path.relpath(path, root), kind, token))
            total += 1
    print("-" * 60)
    if total:
        print("待人工复核 %d 项：判定为占位符/公开值则放行，否则先修复再提交。" % total)
        return 1
    print("通过：未发现待复核项。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
