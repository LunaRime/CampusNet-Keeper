#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cnkrouter --diagnose 的行为回归（D1）。

验四件事：时间窗过滤、跨午夜、来源缺失要标明、超限要截断，以及**输出必须已脱敏**。
全部用合成日志，在临时目录里跑，不碰任何真实日志。

用法：python3 tests/test_diag.py <cnkrouter 路径>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

PASS = 0
FAIL = 0


def check(cond, name):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  [PASS]", name)
    else:
        FAIL += 1
        print("  [FAIL]", name)


def ts(t):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t))


def ymd(t):
    return time.strftime("%Y-%m-%d", time.localtime(t))


def write_log(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for line in lines:
            f.write(line + "\n")


def run(exe, conf, out, *extra):
    cmd = [exe, "--config=" + conf, "--diagnose", "--out=" + out] + list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True)
    body = ""
    if os.path.exists(out):
        with open(out, encoding="utf-8") as f:
            body = f.read()
    return p, body


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./build/cnkrouter"
    # 回退目录是硬编码的 /tmp/cnkrouter：先清掉，免得读到别的用例的残留
    shutil.rmtree("/tmp/cnkrouter", ignore_errors=True)

    tmp = tempfile.mkdtemp(prefix="cnk_diag_")
    logs = os.path.join(tmp, "logs")
    conf = os.path.join(tmp, "cnkrouter.conf")
    out = os.path.join(tmp, "out.log")
    with open(conf, "w", encoding="utf-8") as f:
        f.write("log_dir=%s\n" % logs)

    now = time.time()

    print("== 时间窗过滤 + 脱敏 ==")
    write_log(os.path.join(logs, ymd(now) + ".log"), [
        ts(now - 20 * 60) + " [INFO] 二十分钟前：不该出现",
        ts(now - 60) + " [INFO] 一分钟前：该出现",
        ts(now - 5) + " [WARN] 五秒前 password=Sec123 userid=20230000000 该出现",
        ts(now - 4) + " [INFO] 门户 http://192.0.2.10/eportal/login?params=AbC%3D HTTP 302",
    ])
    # 别人家的文件
    write_log(os.path.join(logs, "other.log"), [ts(now - 10) + " [INFO] 别人家的日志"])

    p, body = run(exe, conf, out)
    check(p.returncode == 0, "退出码 0")
    check("二十分钟前" not in body, "窗口外的行被排除")
    check("一分钟前" in body, "窗口内的行被采到")
    check("别人家的日志" not in body, "只采 CNK 自己的日志文件")
    check("password=Sec123" not in body, "凭据已脱敏")
    check("password=***" in body, "凭据替换为 ***")
    check("20230000000" not in body, "账号已打码")
    check("192.0.2.10" not in body, "真实地址已归一化")
    check("192.0.x.x" in body, "地址保留网段结构")
    check("params=AbC" not in body, "门户查询参数已脱敏")
    check("# 版本: " in body, "头部含构建版本")
    check("# 时间窗: " in body, "头部含时间窗")

    print("== 跨午夜（--around） ==")
    os.remove(os.path.join(logs, ymd(now) + ".log"))
    os.remove(os.path.join(logs, "other.log"))
    midnight = time.mktime(time.strptime(ymd(now) + " 00:00:00", "%Y-%m-%d %H:%M:%S"))
    write_log(os.path.join(logs, ymd(midnight - 3600) + ".log"),
              [ts(midnight - 60) + " [INFO] 前一夜的行"])
    write_log(os.path.join(logs, ymd(midnight) + ".log"),
              [ts(midnight + 60) + " [INFO] 当天的行"])
    p, body = run(exe, conf, out, "--around=" + ts(midnight))
    check(p.returncode == 0, "跨日退出码 0")
    check("前一夜的行" in body, "跨午夜取到前一天")
    check("当天的行" in body, "跨午夜取到当天")
    check("# 缺失: " not in body, "两天都在时不该报缺失")

    print("== 来源缺失要标出来 ==")
    os.remove(os.path.join(logs, ymd(midnight - 3600) + ".log"))
    p, body = run(exe, conf, out, "--around=" + ts(midnight))
    check(p.returncode == 0, "缺文件时仍正常退出")
    check("# 缺失: " in body, "头部标出缺失日期")
    check(ymd(midnight - 3600) in body, "缺失项写明是哪一天")

    print("== 超限截断 ==")
    os.remove(os.path.join(logs, ymd(midnight) + ".log"))
    write_log(os.path.join(logs, ymd(now) + ".log"),
              [ts(now - 30) + " [INFO] 第 %d 行" % i for i in range(6000)])
    p, body = run(exe, conf, out, "--minutes=15")
    check(p.returncode == 0, "超限时退出码 0")
    check("# 截断: 是" in body, "头部标明截断")
    check("输出已截断" in body, "尾部也有截断说明")
    body_lines = [l for l in body.splitlines() if l and not l.startswith("#")]
    check(len(body_lines) == 5000, "行数被截到 5000（实际 %d）" % len(body_lines))

    print("== 默认窗口 15 分钟 ==")
    os.remove(os.path.join(logs, ymd(now) + ".log"))
    write_log(os.path.join(logs, ymd(now) + ".log"), [
        ts(now - 16 * 60) + " [INFO] 十六分钟前",
        ts(now - 14 * 60) + " [INFO] 十四分钟前",
    ])
    p, body = run(exe, conf, out)
    check("十六分钟前" not in body, "默认 15 分钟：16 分钟前被排除")
    check("十四分钟前" in body, "默认 15 分钟：14 分钟前保留")

    shutil.rmtree(tmp, ignore_errors=True)
    print()
    print("通过 %d，失败 %d" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
