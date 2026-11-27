#!/usr/bin/env python3
"""test_sign_update.py — tools/sign_update.py 签名前核对公钥（docs/CrossDesk借鉴设计.md CD9）。

私钥和编进 ashdesk_update 的公钥对不上时，签出来的清单所有装好的客户端都会拒收，所以要在签之前拒绝。
这里用临时密钥对把能签、不能签的几种情况各走一遍：对得上能签、签名验得过；对不上、公钥文件不在、
头文件里没有那个数组、没设私钥，都不能签，也不留下清单或签名。

要 cryptography 包。用法（在仓库根目录）：python3 tools/test_sign_update.py
输出一行 RESULT checks=N failures=M，退出码 0 = 都对。
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization

SCRIPT = str(Path(__file__).with_name("sign_update.py"))
checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f"FAIL {what}")


def run(*args, key=None):
    env = dict(os.environ)
    env.pop("ASHDESK_UPDATE_KEY", None)
    if key:
        env["ASHDESK_UPDATE_KEY"] = key
    return subprocess.run([sys.executable, SCRIPT, *args], env=env, capture_output=True, text=True)


with tempfile.TemporaryDirectory() as d:
    t = Path(d)
    for name in ("a", "b"):
        r = run("genkey", str(t / f"{name}.pem"), str(t / f"{name}.pub.pem"))
        check(r.returncode == 0, f"genkey {name}: {r.stderr}")
    run("header", str(t / "a.pub.pem"), str(t / "a.h"))
    (t / "pkg.zip").write_bytes(b"package bytes")
    pub_a = serialization.load_pem_public_key((t / "a.pub.pem").read_bytes())

    # 对得上：用 A 签、按 A 的头文件核，签名验得过；sign 子命令也一样
    r = run("manifest", "--version", "1.2.3", "--url", "https://example.invalid/p.zip", "--expect-pubkey", str(t / "a.h"),
            str(t / "pkg.zip"), str(t / "ok.txt"), key=str(t / "a.pem"))
    check(r.returncode == 0, f"matching header: {r.stderr}")
    if r.returncode == 0:
        pub_a.verify((t / "ok.txt.sig").read_bytes(), (t / "ok.txt").read_bytes())   # 不对就抛异常
    r = run("sign", "--expect-pubkey", str(t / "a.pub.pem"), str(t / "ok.txt"), key=str(t / "a.pem"))
    check(r.returncode == 0, f"matching PEM: {r.stderr}")

    # 对不上：用 B 签、按 A 核——拒绝，什么都不写
    r = run("manifest", "--version", "1.2.3", "--url", "https://example.invalid/p.zip", "--expect-pubkey", str(t / "a.h"),
            str(t / "pkg.zip"), str(t / "bad.txt"), key=str(t / "b.pem"))
    check(r.returncode != 0 and "clients would reject" in r.stderr, f"mismatch refused: rc={r.returncode} {r.stderr}")
    check(not (t / "bad.txt").exists() and not (t / "bad.txt.sig").exists(), "mismatch leaves no manifest or signature")
    before = (t / "ok.txt.sig").read_bytes()
    r = run("sign", "--expect-pubkey", str(t / "a.pub.pem"), str(t / "ok.txt"), key=str(t / "b.pem"))
    check(r.returncode != 0 and (t / "ok.txt.sig").read_bytes() == before, "re-sign with the wrong key keeps the old signature")

    # 默认按仓库里发布用的头文件核：临时密钥当然对不上
    r = subprocess.run([sys.executable, SCRIPT, "sign", str(t / "ok.txt")], capture_output=True, text=True,
                       cwd=str(Path(SCRIPT).parent.parent), env={**os.environ, "ASHDESK_UPDATE_KEY": str(t / "a.pem")})
    check(r.returncode != 0 and "update_pubkey.h" in r.stderr, f"default header checked: rc={r.returncode} {r.stderr}")

    # 公钥文件不在、头文件里没有数组、私钥没设：都不能签
    r = run("sign", "--expect-pubkey", str(t / "missing.h"), str(t / "ok.txt"), key=str(t / "a.pem"))
    check(r.returncode != 0 and "missing.h" in r.stderr, f"missing expected key: {r.stderr}")
    (t / "empty.h").write_text("#pragma once\n")
    r = run("sign", "--expect-pubkey", str(t / "empty.h"), str(t / "ok.txt"), key=str(t / "a.pem"))
    check(r.returncode != 0 and "UPD_PUBKEY" in r.stderr, f"header without the array: {r.stderr}")
    r = run("sign", "--expect-pubkey", str(t / "a.h"), str(t / "ok.txt"))
    check(r.returncode != 0 and "ASHDESK_UPDATE_KEY" in r.stderr, f"no private key: {r.stderr}")

print(f"RESULT checks={checks} failures={failures}")
sys.exit(1 if failures else 0)
