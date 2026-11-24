#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_ipk.sh 的行为回归：用合成 .ipk 证明它真的会红。

为什么需要（2026-11-24 R1）：校验器若被改成"永远通过"，CI 里那份真实 .ipk
照样全绿 —— 只有故意做坏的包能暴露。合成包不联网、不需要 OpenWrt SDK，
也刻意不复用 Makefile 的任何解析结果（期望清单只出现在测试里）。

用法：python3 tests/test_verify_ipk.py      （在 router/ 下，或任意目录）
"""
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "scripts", "verify_ipk.sh")

BASE = [
    ("usr/bin/cnkrouter", 0o755, b"ELF-fake-binary"),
    ("usr/bin/cnk-cleanup", 0o755, b"#!/bin/sh\ntrue\n"),
    ("usr/bin/cnk-traffic", 0o755, b"#!/bin/sh\ntrue\n"),
    ("usr/bin/cnk-blip", 0o755, b"#!/bin/sh\ntrue\n"),
    ("etc/init.d/cnkrouter", 0o755, b"#!/bin/sh /etc/rc.common\n"),
    ("etc/config/cnkrouter", 0o600, b"config cnkrouter 'main'\n"),
    ("etc/hotplug.d/iface/90-cnkrouter", 0o755, b"#!/bin/sh\ntrue\n"),
]


def _tar(entries):
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w:gz") as tf:
        for name, mode, data in entries:
            ti = tarfile.TarInfo("./" + name)
            ti.size, ti.mode, ti.mtime = len(data), mode, 1700000000
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            tf.addfile(ti, io.BytesIO(data))
    return bio.getvalue()


def make_ipk(path, entries):
    control = _tar([("control", 0o644,
                     b"Package: cnkrouter\nVersion: 0.1.0-1\n"
                     b"Architecture: aarch64_generic\n")])
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w:gz") as tf:
        for name, blob in (("./debian-binary", b"2.0\n"),
                           ("./control.tar.gz", control),
                           ("./data.tar.gz", _tar(entries))):
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime = len(blob), 0o644, 1700000000
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            tf.addfile(ti, io.BytesIO(blob))
    with open(path, "wb") as f:
        f.write(bio.getvalue())


@unittest.skipUnless(shutil.which("sh") and shutil.which("tar"), "需要 sh + tar")
class VerifyIpkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="cnk-ipk-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def run_verify(self, ipk=None):
        # 传相对文件名、cwd 指到包所在目录：Windows 的 C:\... 直接传给 MSYS 的 tar 会失败，
        # 而脚本内部的 $"$ipk" 不经过 MSYS 的参数转换。
        if ipk is None:
            return subprocess.run(["sh", SCRIPT], capture_output=True, text=True)
        return subprocess.run(["sh", SCRIPT, os.path.basename(ipk)],
                              capture_output=True, text=True,
                              cwd=os.path.dirname(ipk))

    def build(self, name, mutate=None):
        entries = [list(e) for e in BASE]
        if mutate:
            mutate(entries)
        path = os.path.join(self.tmp, name)
        make_ipk(path, [tuple(e) for e in entries])
        return path

    def test_good_passes(self):
        r = self.run_verify(self.build("good.ipk"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("未安装 crontab", r.stdout)

    def test_wrong_mode_fails(self):
        def m(entries):
            for e in entries:
                if e[0] == "usr/bin/cnk-blip":
                    e[1] = 0o644
        r = self.run_verify(self.build("bad_mode.ipk", m))
        self.assertEqual(r.returncode, 1)
        self.assertIn("usr/bin/cnk-blip", r.stdout)

    def test_missing_file_fails(self):
        def m(entries):
            entries[:] = [e for e in entries if e[0] != "usr/bin/cnk-traffic"]
        r = self.run_verify(self.build("missing.ipk", m))
        self.assertEqual(r.returncode, 1)
        # 必须命中"清单缺项"这条，而不是别的失败原因（否则 rc=1 是假阳性）
        self.assertIn("usr/bin/cnk-traffic", r.stdout)

    def test_crontab_fails(self):
        def m(entries):
            entries.append(["etc/crontabs/root", 0o600,
                            b"* * * * * /bin/sh /usr/bin/cnk-blip\n"])
        r = self.run_verify(self.build("cron.ipk", m))
        self.assertEqual(r.returncode, 1)
        self.assertIn("默认不启动", r.stdout)

    def test_static_check_on_repo(self):
        r = self.run_verify()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=2).result.wasSuccessful() else 1)
