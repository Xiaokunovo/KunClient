#!/bin/bash
# 大肥鱼 · 服务器运维脚本
#   ./manage.sh status     看容器 / 机器人 / 端口
#   ./manage.sh qr         取当前登录二维码（不重启）
#   ./manage.sh qr -r      重启 NapCat 换一张新二维码（2 分钟内有效）
#   ./manage.sh fix        重新写入 OneBot WS 配置并重启 NapCat
#   ./manage.sh logs       机器人日志
#   ./manage.sh restart    只重启机器人

set -uo pipefail

APP=/www/wwwroot/dafeiyu-bot
CFG_VOL=/var/lib/docker/volumes/ea8765a338f9432dee0d3c3fe6522e8b942cfbd4fafce85420f5b3a6a2104884/_data
QQ_VOL=03c82d4b2a715d00555898cf7230774222b1bb5d49de3127769f3c78cea98067
IMAGE=mlikiowa/napcat-docker
PORT=3001

onebot_json() {
cat <<'JSON'
{
  "network": {
    "httpServers": [],
    "httpSseServers": [],
    "httpClients": [],
    "websocketServers": [
      {
        "name": "dafeiyu-ws",
        "enable": true,
        "host": "0.0.0.0",
        "port": 3001,
        "messagePostFormat": "array",
        "reportSelfMessage": false,
        "token": "",
        "enableForcePushEvent": true,
        "debug": false,
        "heartInterval": 30000
      }
    ],
    "websocketClients": [],
    "plugins": []
  },
  "musicSignUrl": "",
  "enableLocalFile2Url": false,
  "parseMultMsg": false
}
JSON
}

case "${1:-status}" in
  status)
    echo "── NapCat 容器 ──"
    docker ps --filter name=napcat --format "  {{.Status}}  端口 {{.Ports}}" || echo "  未运行"
    echo "── 机器人 ──"
    pm2 jlist 2>/dev/null | python3 -c '
import json,sys
try:
    for p in json.load(sys.stdin):
        if p["name"]=="dafeiyu-bot":
            e=p["pm2_env"]
            print("  %s  重启 %d 次  uptime %ds" % (e["status"], e["restart_time"], (__import__("time").time()*1000-e["pm_uptime"])/1000))
except Exception:
    print("  未在 pm2 里")
'
    echo "── 端口 $PORT ──"
    ss -lntp 2>/dev/null | grep ":$PORT" || echo "  未监听（NapCat 还没登录）"
    echo "── 登录态 ──"
    docker logs --tail 60 napcat 2>&1 | grep -E "登录成功|账号.*已上线|二维码" | tail -2 | sed 's/^/  /'
    ;;

  qr)
    if [ "${2:-}" = "-r" ]; then
      echo "重启 NapCat，换新二维码…"
      docker restart napcat >/dev/null
      sleep 12
    fi

    # NapCat 在二维码过期后会自己续一张（日志里能看到 Login Error 之后紧跟新码），
    # 所以默认直接把日志里最后一块 ASCII 二维码打出来，终端里就能扫。
    echo
    docker logs --tail 400 napcat 2>&1 | python3 -c '
import sys
lines = [l.rstrip("\n") for l in sys.stdin]
blocks, cur = [], []
for line in lines:
    if line and set(line) <= set("█▀▄▄ "):
        cur.append(line)
    else:
        if len(cur) >= 8:
            blocks.append(cur)
        cur = []
if len(cur) >= 8:
    blocks.append(cur)
if blocks:
    print("\n".join(blocks[-1]))
else:
    print("没找到二维码 —— 执行 ./manage.sh qr -r 换一张")
'
    echo
    docker logs --tail 60 napcat 2>&1 | grep "二维码解码URL" | tail -1
    echo "（二维码约 2 分钟有效，过期后 NapCat 会自动续一张；再执行一次本命令即可）"

    mkdir -p "$APP/data"
    if docker cp napcat:/app/napcat/cache/qrcode.png "$APP/data/qrcode.png" 2>/dev/null; then
      echo "图片版：$APP/data/qrcode.png（scp 到本机看）"
    fi
    ;;

  fix)
    echo "写入 OneBot WS 配置…"
    onebot_json > "$CFG_VOL/onebot11.json"
    cp "$CFG_VOL/onebot11.json" "$CFG_VOL/onebot11_4010621705.json"
    chmod 666 "$CFG_VOL"/onebot11*.json
    docker restart napcat >/dev/null
    sleep 12
    ss -lntp | grep ":$PORT" && echo "WS 已就绪" || echo "WS 还没起来（先登录）"
    ;;

  logs)
    pm2 logs dafeiyu-bot --lines 40 --nostream
    ;;

  restart)
    pm2 restart dafeiyu-bot
    ;;

  *)
    sed -n '2,10p' "$0"
    ;;
esac
