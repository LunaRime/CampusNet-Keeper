#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""配置键的三方一致性检查：UCI 模板 / init.d 渲染 / main.c 解析。

为什么要有它（2026-11-24 R2）：init.d 的校验里写死了「19 个键」，
这个数字必须与渲染的 echo 行数、UCI 模板的 option 数、config_parse 认识的键数
同时相等。任一处加键忘了改别处，就会静默丢配置 —— 比如在模板里加了 option
但 init.d 不渲染，用户改了不生效却毫无提示。

用法：python3 tests/test_config_keys.py
退出码：0=一致；1=不一致（打印明细）。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

INITD = os.path.join(ROOT, "packaging/openwrt/files/etc/init.d/cnkrouter")
UCI = os.path.join(ROOT, "packaging/openwrt/files/etc/config/cnkrouter")
MAIN = os.path.join(ROOT, "src/main.c")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def main():
    initd = read(INITD)
    uci = read(UCI)
    src = read(MAIN)

    rendered = set(re.findall(r'echo "([a-z_]+)=', initd))
    opts = set(re.findall(r"^\s*option ([a-z_]+)", uci, re.M)) - {"enabled"}
    parsed = set(re.findall(r'strcmp\(key, "([a-z_]+)"\)', src))
    hard = re.findall(r"-lt (\d+)", initd)

    fail = 0

    def cmp(name, a, b):
        nonlocal fail
        if a == b:
            print("ok   %s（%d 项）" % (name, len(a)))
            return
        fail = 1
        print("FAIL %s" % name)
        for k in sorted(a - b):
            print("       只在前者：%s" % k)
        for k in sorted(b - a):
            print("       只在后者：%s" % k)

    cmp("init.d 渲染 == main.c 解析", rendered, parsed)
    cmp("UCI 模板（去掉 enabled）== init.d 渲染", opts, rendered)

    # init.d 里写死的下限必须等于真实键数，否则校验形同虚设
    if len(hard) != 1:
        print("FAIL 找不到唯一的 '-lt N' 校验：%r" % hard)
        fail = 1
    elif int(hard[0]) != len(rendered):
        print("FAIL 硬编码 %s 与实际渲染 %d 个键不一致" % (hard[0], len(rendered)))
        fail = 1
    else:
        print("ok   硬编码下限 %s == 实际键数 %d" % (hard[0], len(rendered)))

    print()
    if fail:
        print("配置键三方不一致：改键时请同步 UCI 模板 / init.d / config_parse")
        return 1
    print("配置键一致（%d 项）" % len(rendered))
    return 0


if __name__ == "__main__":
    sys.exit(main())
