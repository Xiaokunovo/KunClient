# -*- coding: utf-8 -*-
import json, os, urllib.request
os.chdir("/www/wwwroot/KunClient-qqbot")
cfg = json.load(open("config.json", encoding="utf-8"))
appid = str(cfg["app_id"])
req = urllib.request.Request(
    "https://bots.qq.com/app/getAppAccessToken",
    data=json.dumps({"appId": appid, "clientSecret": cfg["app_secret"]}).encode(),
    method="POST")
req.add_header("Content-Type", "application/json")
tok = json.loads(urllib.request.urlopen(req, timeout=15).read())["access_token"]

for scope in ("group", "c2c"):
    r = urllib.request.Request(f"https://api.bot.qq.com/v2/panels?scope={scope}&limit=20")
    r.add_header("Authorization", "QQBot " + tok)
    r.add_header("X-Union-Appid", appid)
    raw = urllib.request.urlopen(r, timeout=15).read().decode()
    print(scope, "RAW:", raw[:600])
    d = json.loads(raw)
    for p in (d.get("panels") or d.get("list") or d.get("data") or []):
        panel = p.get("panel") or {}
        print("  ", p.get("panel_id"), panel.get("remark"), "->",
              [i.get("name") for i in panel.get("items") or []])
