# -*- coding: utf-8 -*-
"""禁言接口实测：POST restrict_chat_setting 打真实成员，验证请求体形状。"""
import json, sys, os, urllib.request, urllib.error
from datetime import datetime, timedelta, timezone

BASE = "https://api.bot.qq.com"
os.chdir("/www/wwwroot/KunClient-qqbot")
cfg = json.load(open("config.json", encoding="utf-8"))
appid, secret = str(cfg["app_id"]), cfg["app_secret"]
TZ = timezone(timedelta(hours=8))
G = "FE8C66EB4386DB0644FBDD0B8FC2041F"
TARGET = "4DA86C28FE8C60C70777E66DC7571110"   # 是宝不是贝^ / member


def call(method, path, token, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
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


req = urllib.request.Request("https://bots.qq.com/app/getAppAccessToken",
                             data=json.dumps({"appId": appid, "clientSecret": secret}).encode(),
                             method="POST")
req.add_header("Content-Type", "application/json")
token = json.loads(urllib.request.urlopen(req, timeout=15).read().decode())["access_token"]

exp = (datetime.now(TZ) + timedelta(minutes=2)).isoformat(timespec="seconds")
print("expire_at =", exp)

cases = [
    ("A: op=add", {"members": [{"op": "add", "member_openid": TARGET, "mute_expire_at": exp}]}),
    ("B: 无 op",   {"members": [{"member_openid": TARGET, "mute_expire_at": exp}]}),
    ("C: op=update", {"members": [{"op": "update", "member_openid": TARGET, "mute_expire_at": exp}]}),
]
for name, body in cases:
    st, out = call("POST", f"/v2/groups/{G}/restrict_chat_setting", token, body)
    print(f"{name:14s} -> {st} {out[:240]}")
    st2, out2 = call("GET", f"/v2/groups/{G}/restrict_chat_setting", token)
    print(f"{'  GET':14s} -> {st2} {out2[:240]}")

# 收尾：解除
st, out = call("POST", f"/v2/groups/{G}/restrict_chat_setting", token,
               {"members": [{"op": "del", "member_openid": TARGET, "mute_expire_at": ""}]})
print("del ->", st, out[:200])
st, out = call("GET", f"/v2/groups/{G}/restrict_chat_setting", token)
print("final GET ->", st, out[:300])
