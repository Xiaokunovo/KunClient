"""本地二维码看板：实时显示服务器上 NapCat 当前的登录二维码。

为什么要这个：QQ 的登录二维码只有约 2 分钟有效期，NapCat 过期后会自动续一张。
「截图 → 发出去 → 对方打开」这条链路根本追不上，所以改成：

    浏览器打开 http://127.0.0.1:7788  →  页面每 4 秒去服务器取一次最新二维码

二维码换了，页面自动跟着换，什么时候扫都行。

    python tools/qr-server.py            启动看板（默认 7788）
    python tools/qr-server.py --port 8080
    python tools/qr-server.py --once     只取一张存到 data/server-qrcode.png
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEY = Path.home() / ".ssh" / "kunkun_admin_ed25519"
HOST = "root@your-server.example.com"
CONTAINER = "napcat"
REMOTE_QR = "/app/napcat/cache/qrcode.png"

_ssh_base = [
    "ssh",
    "-i", str(KEY),
    "-o", "StrictHostKeyChecking=no",
    "-o", "ConnectTimeout=8",
    "-o", "BatchMode=yes",
    HOST,
]

_cache_lock = threading.Lock()
_cache = {"at": 0.0, "data": b"", "err": ""}
TTL = 2.0  # 秒，避免页面每 4 秒刷新时反复打 SSH

# 自愈：NapCat 在「扫码后授权失败」这种情况下**不会**再续二维码，
# 页面就会一直显示一张死码。所以同一张图超过 STALE 秒还没换、且确认没登录，
# 就重启容器逼它出一张新的（限频，别把容器重启成筛子）。
STALE = 150
RESTART_GAP = 200
_seen = {"hash": b"", "since": 0.0, "last_restart": 0.0, "note": "", "logged": False}


def restart_napcat() -> None:
    try:
        subprocess.run(_ssh_base + [f"docker restart {CONTAINER}"], capture_output=True, timeout=60)
        _cache.update(data=b"", at=0.0, err="")
        _seen.update(hash=b"", since=time.time())
        _seen["note"] = "二维码长时间未更新，已自动重启 NapCat 换新码"
        print(f"[watchdog] {time.strftime('%H:%M:%S')} 重启 NapCat")
    except Exception as exc:  # noqa: BLE001
        _seen["note"] = f"自动重启失败：{exc}"


def maybe_heal(data: bytes) -> None:
    import hashlib

    # 🔴 登录成功过就永久闭嘴。
    # 判断依据是 pm2 日志里那行「登录账号」，日志滚动几轮之后这行会滑出 tail 窗口，
    # 那时 login_state() 会误判成「没登录」，看板就会去重启容器 —— 把好端端的会话干掉。
    if _seen["logged"]:
        return

    now = time.time()
    digest = hashlib.md5(data).digest()
    if digest != _seen["hash"]:
        _seen.update(hash=digest, since=now)
        return
    if now - _seen["since"] < STALE:
        return
    if now - _seen["last_restart"] < RESTART_GAP:
        return
    ok, _who = login_state()
    if ok:  # 已登录就别动，否则会把好好的会话重启掉
        _seen["logged"] = True
        return
    _seen["last_restart"] = now
    threading.Thread(target=restart_napcat, daemon=True).start()


def fetch_qr(force: bool = False) -> tuple[bytes, str]:
    """取当前二维码 PNG 字节。失败时返回上一次成功的图 + 错误信息。"""
    with _cache_lock:
        if not force and _cache["data"] and time.time() - _cache["at"] < TTL:
            return _cache["data"], _cache["err"]
        try:
            proc = subprocess.run(
                _ssh_base + [f"docker exec {CONTAINER} cat {REMOTE_QR}"],
                capture_output=True,
                timeout=20,
            )
            data = proc.stdout
            if proc.returncode == 0 and data[:4] == b"\x89PNG":
                _cache.update(data=data, at=time.time(), err="")
                maybe_heal(data)
            else:
                msg = (proc.stderr or b"").decode("utf-8", "replace").strip()
                _cache["err"] = msg[:200] or "取二维码失败（容器没跑？）"
        except subprocess.TimeoutExpired:
            _cache["err"] = "SSH 超时"
        except Exception as exc:  # noqa: BLE001
            _cache["err"] = str(exc)[:200]
        return _cache["data"], _cache["err"]


def login_state() -> tuple[bool, str]:
    """判断是否真的登录了。

    🔴 不能用「3001 端口在监听」当判据 —— NapCat 的 WS 适配器在**扫码之前**就已经
    把端口开起来了，只是握手一直不成功。可靠信号是机器人日志里那条「登录账号」，
    它是 get_login_info 成功之后才打印的。
    """
    try:
        # 🔴 不能只看「登录账号」这一行 —— 它是历史日志，账号掉线之后这行还在。
        # 要看**最后一条**连接状态：已连上 / 连接断开，谁在后面谁说了算。
        proc = subprocess.run(
            _ssh_base + [
                "L=$(tail -n 800 /root/.pm2/logs/dafeiyu-bot-out.log 2>/dev/null"
                " | grep -E '已连上 OneBot 端|连接断开' | tail -1);"
                "echo \"LAST:$L\";"
                "tail -n 800 /root/.pm2/logs/dafeiyu-bot-out.log 2>/dev/null"
                " | grep '登录账号' | tail -1"
            ],
            capture_output=True,
            timeout=12,
        )
        out = proc.stdout.decode("utf-8", "replace")
        lines = out.splitlines()
        last_state = next((l for l in lines if l.startswith("LAST:")), "LAST:")
        if "已连上" not in last_state:
            return False, ""
        who = ""
        for line in lines:
            if "登录账号" in line:
                who = line.split("登录账号")[-1].strip(" :：|")
        return True, who
    except Exception:  # noqa: BLE001
        pass
    return False, ""


_bot_kick = {"done": False}


def ensure_bot_connected() -> None:
    """登录成功的那一刻把机器人重启一次，立刻建连。

    否则要等它自己的重连退避（最长 30 秒一轮），体感像是「扫了没反应」。
    """
    if _bot_kick["done"]:
        return
    _bot_kick["done"] = True
    try:
        subprocess.run(_ssh_base + ["pm2 restart dafeiyu-bot >/dev/null 2>&1"], capture_output=True, timeout=45)
        print(f"[watchdog] {time.strftime('%H:%M:%S')} 检测到登录成功，已重启机器人建连")
    except Exception:  # noqa: BLE001
        _bot_kick["done"] = False


PAGE = """<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><title>大肥鱼 · 扫码登录</title>
<style>
  :root { color-scheme: light; }
  * { box-sizing: border-box; }
  body {
    margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center;
    background: #f4f7fb; font-family: -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
    color: #1b2432;
  }
  .card {
    background: #fff; border-radius: 20px; padding: 28px 32px 24px;
    box-shadow: 0 10px 40px rgba(20, 40, 80, .10), 0 1px 0 rgba(255,255,255,.9) inset;
    text-align: center; max-width: 420px;
  }
  h1 { font-size: 19px; margin: 0 0 4px; letter-spacing: .3px; }
  .sub { font-size: 13px; color: #6b7a90; margin-bottom: 18px; }
  .qrbox {
    width: 300px; height: 300px; margin: 0 auto; border-radius: 16px;
    background: #f7f9fc; display: flex; align-items: center; justify-content: center;
    overflow: hidden; border: 1px solid #e6ecf5;
  }
  .qrbox img { width: 288px; height: 288px; image-rendering: pixelated; display: block; }
  .dot { display: inline-block; width: 7px; height: 7px; border-radius: 50%; background: #22c55e; margin-right: 6px; }
  .foot { margin-top: 16px; font-size: 12px; color: #8a97a8; line-height: 1.7; }
  .err { margin-top: 12px; font-size: 12px; color: #d33; }
</style></head>
<body>
  <div class="card">
    <h1 id="title">🐳 大肥鱼 · 扫码登录</h1>
    <div class="sub" id="sub">用要当机器人的那个 QQ 扫</div>
    <div class="qrbox" id="qrbox"><img id="qr" src="/qr.png" alt="二维码"></div>
    <div class="foot">
      <span class="dot" id="dot"></span><span id="tick">自动刷新中</span>
      <br>二维码约 2 分钟过期；失效后这里会自动换新的
    </div>
    <div class="err" id="err"></div>
  </div>
<script>
  let n = 0;
  const img = document.getElementById('qr');
  const tick = document.getElementById('tick');
  const errBox = document.getElementById('err');
  const title = document.getElementById('title');
  const sub = document.getElementById('sub');
  const dot = document.getElementById('dot');

  async function pull() {
    try {
      const r = await fetch('/qr.png?t=' + Date.now(), { cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const blob = await r.blob();
      img.src = URL.createObjectURL(blob);
      n++;
      tick.textContent = '已刷新 ' + n + ' 次 · ' + new Date().toLocaleTimeString('zh-CN');
      errBox.textContent = '';
    } catch (e) {
      errBox.textContent = '取二维码失败：' + e.message + '（服务器/容器还在吗？）';
    }
  }

  async function checkStatus() {
    try {
      const t = await (await fetch('/status?t=' + Date.now(), { cache: 'no-store' })).text();
      const [state, extra] = t.split('|');
      if (state === 'logged_in') {
        title.textContent = '✅ 已登录';
        sub.textContent = '账号 ' + (extra || '') + ' —— 可以关掉这个页面了';
        document.getElementById('qrbox').style.display = 'none';
        dot.style.background = '#22c55e';
        tick.textContent = '机器人已连上';
      } else if (extra) {
        errBox.textContent = extra;
      }
    } catch (e) { /* 静默 */ }
  }

  pull();
  checkStatus();
  setInterval(pull, 4000);
  setInterval(checkStatus, 6000);
</script>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # 静音，别刷屏
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            body = PAGE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/qr.png":
            data, err = fetch_qr()
            if not data:
                self.send_response(503)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.end_headers()
                self.wfile.write((err or "no qr").encode("utf-8"))
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        if path == "/status":
            ok, who = login_state()
            if ok:
                _seen["logged"] = True
                body = f"logged_in|{who}"
                threading.Thread(target=ensure_bot_connected, daemon=True).start()
            else:
                body = f"waiting|{_seen['note']}"
            raw = body.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return

        self.send_error(404)


def main() -> int:
    ap = argparse.ArgumentParser(description="大肥鱼二维码看板")
    ap.add_argument("--port", type=int, default=7788)
    ap.add_argument("--once", action="store_true", help="只取一张图存盘，不起服务")
    args = ap.parse_args()

    if args.once:
        data, err = fetch_qr(force=True)
        if not data:
            print("取二维码失败：", err)
            return 1
        out = ROOT / "data" / "server-qrcode.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print("已保存", out)
        return 0

    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{args.port}"
    print(f"二维码看板已启动：{url}")
    print("二维码会自动跟着服务器更新，随时可以扫。Ctrl+C 退出。")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已退出。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
