# -*- coding: utf-8 -*-
"""线上群管理接口诊断：直接拿 token 打平台接口，看机器人在群里的真实身份。"""
import json, sys, urllib.request, urllib.error, os

BASE = "https://api.bot.qq.com"
os.chdir("/www/wwwroot/KunClient-qqbot")
cfg = json.load(open("config.json", encoding="utf-8"))
appid, secret = str(cfg["app_id"]), cfg["app_secret"]


def post_json(url, body, headers=None):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode())


def call(method, path, token, body=None):
    url = BASE + path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "QQBot " + token)
    req.add_header("X-Union-Appid", appid)
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return -1, repr(e)


tok = post_json("https://bots.qq.com/app/getAppAccessToken",
                {"appId": appid, "clientSecret": secret})
token = tok.get("access_token")
print("token:", "OK" if token else tok)

groups = []
try:
    d = json.load(open("data.json", encoding="utf-8"))
    groups = list((d.get("groups") or {}).keys())
except Exception as e:
    print("read data.json failed:", e)
if len(sys.argv) > 1:
    groups = sys.argv[1:]
print("groups:", groups)

for g in groups:
    print("\n=== group", g)
    for method, path in (
        ("GET", f"/v2/groups/{g}/bot_state"),
        ("GET", f"/v2/groups/{g}/info"),
        ("GET", f"/v2/groups/{g}/restrict_chat_setting"),
        ("GET", f"/v2/groups/{g}/members?limit=20"),
        ("GET", f"/v2/groups/{g}/join_request_list?limit=5"),
    ):
        st, body = call(method, path, token)
        print(f"  {method} {path.split('/')[-1][:40]:42s} -> {st} {body[:300]}")
