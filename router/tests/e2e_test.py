#!/usr/bin/env python3
"""cnkrouter 端到端测试：本地模拟 Dr.COM 门户（不碰真实校园网）。

验证链路：探测 → 劫持识别 → chkstatus → loadConfig(rcn) → AES 加密登录 → 复测在线。
服务端**真的解密** params 并逐字段断言，因此不是"客户端自己说自己成功"。

AES-128 解密为纯 Python 独立实现，启动时先用 FIPS-197 向量自校验；
若自校验失败，整套测试直接判失败（防止 oracle 本身错了还"全绿"）。

用法：python tests/e2e_test.py [cnkrouter 可执行文件路径]
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

# ---------------------------------------------------------------- AES-128 解密

SBOX = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
]
INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i


def xtime(a):
    a <<= 1
    return (a ^ 0x1B) & 0xFF if a & 0x100 else a


def gmul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        a = xtime(a)
        b >>= 1
    return p


def key_expand(key):
    rk = list(key)
    rcon = 1
    for i in range(16, 176, 4):
        t = rk[i - 4:i]
        if i % 16 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[x] for x in t]
            t[0] ^= rcon
            rcon = xtime(rcon)
        for j in range(4):
            rk.append(rk[i - 16 + j] ^ t[j])
    return rk


def inv_shift_rows(s):
    # 行 1 右移 1
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t
    # 行 2 右移 2
    t = s[2]; s[2] = s[10]; s[10] = t
    t = s[6]; s[6] = s[14]; s[14] = t
    # 行 3 右移 3（等价左移 1）
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t


def inv_mix_columns(s):
    for c in range(4):
        p = s[4 * c:4 * c + 4]
        a0, a1, a2, a3 = p
        s[4 * c + 0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9)
        s[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13)
        s[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11)
        s[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14)


def aes_decrypt_block(rk, block):
    s = [block[i] ^ rk[160 + i] for i in range(16)]
    for rnd in range(9, 0, -1):
        inv_shift_rows(s)
        s = [INV_SBOX[x] for x in s]
        s = [s[i] ^ rk[rnd * 16 + i] for i in range(16)]
        inv_mix_columns(s)
    inv_shift_rows(s)
    s = [INV_SBOX[x] for x in s]
    return bytes(s[i] ^ rk[i] for i in range(16))


def aes_ecb_decrypt(key, data):
    rk = key_expand(key)
    out = b""
    for i in range(0, len(data), 16):
        out += aes_decrypt_block(rk, data[i:i + 16])
    if not out:
        raise ValueError("空密文")
    pad = out[-1]
    if pad < 1 or pad > 16 or out[-pad:] != bytes([pad]) * pad:
        raise ValueError("PKCS7 填充非法")
    return out[:-pad]


def _selftest_aes():
    """FIPS-197 C.1：密钥 000102..0f，密文 69c4..c55a，明文 00112233..eeff"""
    key = bytes(range(16))
    ct = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
    rk = key_expand(key)
    pt = aes_decrypt_block(rk, list(ct))
    if pt.hex() != "00112233445566778899aabbccddeeff":
        raise SystemExit("FIPS-197 自检失败，测试 oracle 不可信")


_selftest_aes()

# ---------------------------------------------------------------- 模拟门户

AES_KEY = b"5c1d5ad4dea0e8dd"
STATE = {
    "online": False,
    "logins": 0,
    "seen": [],          # 每次登录解出的 payload
    "bad_params": 0,
}


class PortalHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    valid_user = "alice"
    valid_pass = "pw123"
    isp_suffix = "@cmcc"

    def log_message(self, fmt, *args):   # 静音默认日志
        pass

    def _send(self, body, status=200, extra=None):
        raw = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)

        if u.path == "/probe":
            # 登录前：302 到门户（模拟真实劫持）；登录后：200 且正文含预期标识
            if STATE["online"]:
                self._send("Success", status=200,
                           extra={"Content-Type": "text/html"})
            else:
                self.send_response(302)
                # 真实劫持是跳到内网门户（异主机）；同主机会被判"在线"
                self.send_header("Location", "http://10.0.0.9/a79.htm")
                self.send_header("Content-Length", "0")
                self.end_headers()
            return

        if u.path == "/portal":
            self._send("<html>Dr.COMWebLoginID_0</html>", extra={"Server": "DrcomServer1.0"})
            return

        if u.path == "/drcom/chkstatus":
            self._send('dr8001({"result":1,"v46ip":"10.0.0.100"})')
            return

        if u.path == "/eportal/portal/page/loadConfig":
            self._send('dr1003({"result":1,"data":{"rcn":"testrcn1"}})')
            return

        if u.path == "/eportal/portal/login":
            self._handle_login(q)
            return

        self._send("not found", status=404)

    def _handle_login(self, q):
        params = q.get("params", [""])[0]
        try:
            plain = aes_ecb_decrypt(AES_KEY, __import__("base64").b64decode(params))
            payload = json.loads(plain.decode("utf-8"))
        except Exception as exc:                       # noqa: BLE001
            STATE["bad_params"] += 1
            import traceback
            print("  [debug] params 长度=%d 解码失败：%s" % (len(params), exc))
            traceback.print_exc()
            self._send('dr8003({"result":0,"msg":"参数解析失败：%s"})' % exc)
            return

        STATE["logins"] += 1
        STATE["seen"].append(payload)

        user = payload.get("user_account", "")
        pw = payload.get("user_password", "")
        expect_user = ",0," + self.valid_user + self.isp_suffix
        if user != expect_user:
            self._send('dr8003({"result":0,"msg":"账号不存在"})')
            return
        if pw != self.valid_pass:
            self._send('dr8003({"result":0,"msg":"密码错误，请重新输入"})')
            return
        if payload.get("rcn") != "testrcn1":
            self._send('dr8003({"result":0,"msg":"当前页面已超时"})')
            return

        STATE["online"] = True
        self._send('dr8003({"result":1,"msg":"认证成功"})')


# ---------------------------------------------------------------- 驱动

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run_client(exe, conf, *args):
    r = subprocess.run([exe, "--config=" + conf] + list(args),
                       capture_output=True, text=True, timeout=90,
                       encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "build", "cnkrouter.exe")

    port = free_port()
    srv = HTTPServer(("127.0.0.1", port), PortalHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    tmpdir = tempfile.mkdtemp(prefix="cnkrouter_e2e_")
    conf = os.path.join(tmpdir, "cnkrouter.conf")

    def write_conf(password):
        with open(conf, "w", encoding="utf-8") as f:
            f.write(
                "auth_url=http://127.0.0.1:%d/\n" % port +
                "username=alice\n"
                "password=%s\n" % password +
                "isp=cmcc\n"
                "probe_url=http://127.0.0.1:%d/probe\n" % port +
                "probe_expect=Success\n"
                "probe_timeout=3\n"
                "probe_interval=30\n"
                "log_level=3\n")

    fails = []

    def check(name, cond, detail=""):
        if cond:
            print("  [PASS] %s" % name)
        else:
            print("  [FAIL] %s %s" % (name, detail))
            fails.append(name)

    print("== 1. 正常登录（--once）==")
    write_conf("pw123")
    rc, out = run_client(exe, conf, "--once")
    check("退出码 0（最终在线）", rc == 0, "rc=%d\n%s" % (rc, out))
    check("日志出现复测在线", "复测结果：已在线 OK" in out or "已在线" in out, out[-400:])
    check("服务端收到 1 次登录", STATE["logins"] == 1, "logins=%d" % STATE["logins"])
    if STATE["seen"]:
        p = STATE["seen"][0]
        check("user_account 含 ,0, 前缀与 @cmcc 后缀",
              p.get("user_account") == ",0,alice@cmcc", repr(p.get("user_account")))
        check("wlan_user_ip 取自 chkstatus", p.get("wlan_user_ip") == "10.0.0.100",
              repr(p.get("wlan_user_ip")))
        check("rcn 取自 loadConfig", p.get("rcn") == "testrcn1", repr(p.get("rcn")))
        check("login_method / is_page_new 与实测一致",
              p.get("login_method") == 1 and p.get("is_page_new") == 558, repr(p))
    check("无解密失败", STATE["bad_params"] == 0, "bad=%d" % STATE["bad_params"])

    print("== 2. 已在线时不再重复登录（--once）==")
    before = STATE["logins"]
    rc, out = run_client(exe, conf, "--once")
    check("退出码 0", rc == 0, "rc=%d" % rc)
    check("未新增登录请求", STATE["logins"] == before, "logins=%d" % STATE["logins"])

    print("== 3. 密码错误（--login）==")
    STATE["online"] = False
    write_conf("badpass")
    rc, out = run_client(exe, conf, "--login")
    check("退出码 2（失败）", rc == 2, "rc=%d" % rc)
    check("识别为凭据类错误", "密码错误" in out, out[-300:])

    print("== 4. 探测离线（--probe，端口关闭）==")
    write_conf("pw123")
    with open(conf, "r", encoding="utf-8") as f:
        text = f.read()
    with open(conf, "w", encoding="utf-8") as f:
        f.write(re.sub(r"probe_url=[^\n]+", "probe_url=http://127.0.0.1:%d/nothing" % (port + 1), text))
    rc, out = run_client(exe, conf, "--probe")
    check("退出码 3（离线）", rc == 3, "rc=%d\n%s" % (rc, out))

    print("== 5. 守护模式（--daemon）==")
    STATE["online"] = False
    before = STATE["logins"]
    write_conf("pw123")
    proc = subprocess.Popen([exe, "--config=" + conf, "--daemon"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    time.sleep(6)
    proc.terminate()
    try:
        dout = proc.communicate(timeout=10)[0] or ""
    except subprocess.TimeoutExpired:
        proc.kill()
        dout = ""
    check("守护模式完成登录", STATE["logins"] > before, "logins=%d" % STATE["logins"])
    check("守护日志出现已恢复在线", "已恢复在线" in dout, dout[-400:])
    check("守护日志包含启动信息", "启动" in dout, dout[:300])

    srv.shutdown()
    print()
    if fails:
        print("E2E 失败：%d 项 -> %s" % (len(fails), ", ".join(fails)))
        return 1
    print("E2E 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
