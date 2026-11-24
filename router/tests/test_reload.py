#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SIGHUP 重载的进程级行为：配置切换、log_dir 真的换文件、坏配置不打断、写不进去退回 stderr。

为什么要有它（2026-11-24 R2）：`--selftest` 只能证明 config_load 事务化；
"改 log_dir 之后日志到底写到哪"必须在真进程上看 —— 这正是 R2 之前会静默失效的地方
（Config 换了，g_log_dir 没换，日志继续写旧目录）。

用法：python3 tests/test_reload.py <cnkrouter 路径>     （需要 Linux：用 SIGHUP）
"""
import glob
import http.server
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time


def start_probe_server():
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = b'{"result":1}' if "chkstatus" in self.path else b""
            self.send_response(204 if not body else 200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def write_conf(path, log_dir, port):
    with open(path, "w", encoding="utf-8") as f:
        f.write("auth_url=http://127.0.0.1:%d/\n" % port)
        f.write("username=alice\npassword=pw\nisp=cmcc\n")
        f.write("probe_url=http://127.0.0.1:%d/generate_204\n" % port)
        f.write("probe_timeout=2\nprobe_interval=10\nlog_level=3\n")
        f.write("log_dir=%s\n" % log_dir)


def logs_in(d):
    return sorted(glob.glob(os.path.join(d, "*.log")))


def read_all(files):
    out = []
    for f in files:
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                out.append(fh.read())
        except OSError:
            pass
    return "".join(out)


def wait_for(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.1)
    return False


def main():
    if not hasattr(signal, "SIGHUP"):
        print("跳过：本平台没有 SIGHUP（需要 Linux）")
        return 0
    exe = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="cnk-reload-")
    da, db = os.path.join(tmp, "a"), os.path.join(tmp, "b")
    os.makedirs(da)
    os.makedirs(db)
    conf = os.path.join(tmp, "cnkrouter.conf")
    srv, port = start_probe_server()

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name + ("" if cond else " " + detail))
        if not cond:
            fails.append(name)

    write_conf(conf, da, port)
    proc = subprocess.Popen([exe, "--config=" + conf, "--daemon"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    # 实时收集 stdout/stderr：HUP 可能落在一次 probe 中间，只在 terminate 时读会漏
    lines = []

    def _reader():
        if proc.stdout is None:
            return
        for line in proc.stdout:
            lines.append(line)

    threading.Thread(target=_reader, daemon=True).start()
    out = ""
    try:
        check("启动后日志落在 log_dir A", wait_for(lambda: logs_in(da), 10), str(logs_in(da)))

        # 1) 合法重载：切到 B，日志必须跟着走
        write_conf(conf, db, port)
        proc.send_signal(signal.SIGHUP)
        ok = wait_for(lambda: "配置已重新加载" in read_all(logs_in(db)), 15)
        check("SIGHUP 后写入 log_dir B，且含『配置已重新加载』", ok, str(logs_in(db)))

        # 2) 坏配置：进程存活、日志里给出原因、继续用 B
        with open(conf, "a", encoding="utf-8") as f:
            f.write("broken-line-without-equals\n")
        proc.send_signal(signal.SIGHUP)
        ok = wait_for(lambda: "配置重载失败" in read_all(logs_in(db)), 15)
        check("坏配置写入后记录『配置重载失败』", ok, read_all(logs_in(db))[-300:])
        check("坏配置后进程仍存活", proc.poll() is None)
        check("坏配置未把日志目录换走（仍只有 A/B）",
              set(os.path.basename(f) for f in logs_in(da)) == set(os.path.basename(f) for f in logs_in(db)))

        # 3) log_dir 指向不可写目录：进程不能死，日志退回 stderr（不能静默丢）
        write_conf(conf, "/proc/cnk-no-such-dir", port)
        proc.send_signal(signal.SIGHUP)
        ok = wait_for(lambda: any("无法打开日志" in l or "配置已重新加载" in l for l in lines), 15)
        check("切换 log_dir 到不可写目录后，日志退回 stderr", ok, repr("".join(lines)[-300:]))
        check("log_dir 不可写时进程仍存活", proc.poll() is None)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        time.sleep(0.3)          # 让 reader 线程把剩余输出取完
        srv.shutdown()

    print()
    if fails:
        print("失败 %d 项：%s" % (len(fails), ", ".join(fails)))
        return 1
    print("重载行为全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
