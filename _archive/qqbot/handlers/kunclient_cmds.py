"""KunClient（刷机客户端）服务端命令：普通用户功能 + 超管管理功能。

【普通用户】绑定自己的 KunClient 账号后可用（数据以服务端为准）：
· kun绑定 用户名 密码 ／ kun解绑
· kun我 —— 自己的账号与会员状态
· kun我的卡密 ／ kun兑换 卡密
· kun反馈 标题 内容 ／ kun公告 ／ kun版本 ／ kun用量

【超级管理员及以上】
· kun状态 ／ kun在线 ／ kun用户 ／ kun踢 ／ kunvip ／ kun卡密 ／ kun重置密码

服务器地址与管理员账号在 config.json 的 kunclient 段配置。
"""
import asyncio
import logging
import time

import aiohttp

from core.release_watch import clean_changelog

log = logging.getLogger("qq.kun")

ROLE_LABEL = {"admin": "管理员", "developer": "开发者", "agent": "代理", "user": "用户"}

UNBOUND_TIP = ("你还没有绑定 KunClient 账号。推荐用邮箱验证码绑定（无需发密码）：\n"
               "① kun绑定邮箱 <你的邮箱> —— 收验证码\n"
               "② kun验证 <验证码> —— 完成绑定\n"
               "（需账号已在客户端/官网绑定过邮箱；绑定后可用 kun我 / kun兑换 / kun反馈 等）")


class KunApiError(RuntimeError):
    def __init__(self, msg, code=None):
        super().__init__(msg)
        self.code = code


class KunClientApi:
    """KunClient 登录服务器客户端：管理员与普通用户的登录态分别缓存。"""

    def __init__(self, base_url: str, username: str, password: str):
        self.base = base_url.rstrip("/")
        self.username = username
        self.password = password
        self._token: str | None = None
        self._expire_at = 0.0
        self._user_tokens: dict = {}        # uid -> (token, expire_at)
        self._pending_binds: dict = {}      # uid -> {email, ts}，邮箱验证码绑定中间态

    async def _post_login(self, session: aiohttp.ClientSession, username: str, password: str) -> dict:
        async with session.post(f"{self.base}/api/login",
                                json={"username": username, "password": password},
                                timeout=aiohttp.ClientTimeout(total=15)) as r:
            data = await r.json(content_type=None)
        if r.status >= 400 or not isinstance(data, dict) or not data.get("success"):
            msg = data.get("message") if isinstance(data, dict) else str(data)
            code = data.get("code") if isinstance(data, dict) else None
            raise KunApiError(f"{msg}（HTTP {r.status}）", code=code)
        d = data["data"]
        return {"token": d["token"], "expire": time.time() + int(d.get("expiresIn", 86400))}

    async def _login(self, session: aiohttp.ClientSession):
        d = await self._post_login(session, self.username, self.password)
        self._token = d["token"]
        self._expire_at = d["expire"]

    async def _parse(self, status: int, data, _retry: bool, relogin):
        if status == 401 and _retry:
            await relogin()
        if isinstance(data, dict) and data.get("success"):
            return data.get("data")
        msg = data.get("message") if isinstance(data, dict) else str(data)
        code = data.get("code") if isinstance(data, dict) else None
        raise KunApiError(f"{msg}（HTTP {status}）", code=code)

    async def request(self, method: str, path: str, json_body=None, _retry=True):
        """管理员身份请求。"""
        async with aiohttp.ClientSession() as session:
            if not self._token or time.time() >= self._expire_at - 300:
                await self._login(session)

            async def relogin():
                await self._login(session)

            async with session.request(
                method, self.base + path, json=json_body,
                headers={"Authorization": f"Bearer {self._token}"},
                timeout=aiohttp.ClientTimeout(total=20),
            ) as r:
                data = await r.json(content_type=None)
                status = r.status
        if status == 401 and _retry:
            self._token = None
            return await self.request(method, path, json_body, _retry=False)
        return await self._parse(status, data, _retry, relogin)

    async def user_request(self, uid: str, rec: dict, method: str, path: str,
                           json_body=None, _retry=True):
        """普通用户身份请求。

        rec 为绑定记录：密码绑定 {"username","password"}；
        邮箱绑定 {"email","token","token_expire"}（token 过期后引导重新验证）。
        """
        async with aiohttp.ClientSession() as session:
            cached = self._user_tokens.get(uid)
            token = None
            if cached and cached[1] > time.time():
                token = cached[0]
            elif rec.get("token") and rec.get("token_expire", 0) > time.time():
                token = rec["token"]
            elif rec.get("password"):
                token = (await self._login_user(session, uid, rec["username"], rec["password"]))["token"]
            else:
                raise KunApiError("绑定登录态已过期，请重新发送 kun绑定邮箱 <邮箱> 获取验证码")

            async def relogin():
                if not rec.get("password"):
                    raise KunApiError("绑定登录态已过期，请重新发送 kun绑定邮箱 <邮箱> 获取验证码")
                await self._login_user(session, uid, rec["username"], rec["password"])

            async with session.request(
                method, self.base + path, json=json_body,
                headers={"Authorization": f"Bearer {token}"},
                timeout=aiohttp.ClientTimeout(total=20),
            ) as r:
                data = await r.json(content_type=None)
                status = r.status
        if status == 401 and _retry:
            self._user_tokens.pop(uid, None)
            rec.pop("token", None)
            if rec.get("password"):
                return await self.user_request(uid, rec, method, path, json_body, _retry=False)
            raise KunApiError("绑定登录态已过期，请重新发送 kun绑定邮箱 <邮箱> 获取验证码")
        return await self._parse(status, data, _retry, relogin)

    async def _login_user(self, session: aiohttp.ClientSession, uid: str, username: str, password: str):
        d = await self._post_login(session, username, password)
        self._user_tokens[uid] = (d["token"], d["expire"])
        return d


def _fmt_user(u: dict) -> str:
    vip = u.get("vipLevelLabel") or ("普通用户" if not u.get("isVip") else "会员")
    if u.get("vipPermanent"):
        vip += "（永久）"
    elif u.get("isVip"):
        left = u.get("vipDaysLeft")
        if isinstance(left, (int, float)) and left >= 0:
            vip += f"（剩 {int(left)} 天）"
    role = ROLE_LABEL.get(u.get("role"), u.get("role"))
    online = "🟢 在线" if u.get("online") else "离线"
    mods = u.get("modulesLabel") or "无"
    return (f"👤 {u.get('username')}（ID {u.get('id')}）\n"
            f"角色：{role} ｜ 状态：{u.get('status') or '—'}\n"
            f"会员：{vip}\n"
            f"{online} ｜ 最近登录：{u.get('last_login') or '—'}\n"
            f"开通模块：{mods}")


async def _find_user(api: KunClientApi, name: str):
    """按用户名精确匹配，其次包含匹配（唯一时直接用）。零匹配返回 None，多个返回元组。"""
    users = await api.request("GET", "/api/users")
    n = name.strip().lower()
    exact = [u for u in users if str(u.get("username", "")).lower() == n]
    if exact:
        return exact[0]
    part = [u for u in users if n in str(u.get("username", "")).lower()]
    if not part:
        return None
    if len(part) == 1:
        return part[0]
    return part, len(part), len(users)


def register(bot, router):
    api = bot.kun

    # ---------- 普通用户功能（绑定自己的 KunClient 账号） ----------

    def _bound(ctx):
        """返回绑定记录 dict（含 username/password 或 email/token），未绑定返回 None。

        以真实 QQ 号（uid）为键，所以换群也能认出是同一个账号。
        """
        return bot.storage.data.get("kun_users", {}).get(ctx.uid)

    async def _arm_confirm(ctx, *, perm, summary, run):
        """登记一次敏感操作的二次确认，并回报「待确认」提示。

        为什么发布类命令必须走这里：改公告 / 切强制更新 / 手动发版播报会**立刻改变
        全体客户端的行为**（强制更新会把所有旧版客户端拦去升级），且没有回退按钮。
        旧实现是「命令发出即生效」，一次手滑就得赶紧再发一条命令补救。
        """
        from core.confirm import CONFIRM_TTL
        bot.confirms.arm(ctx.session_key, command=summary, summary=summary,
                         run=run, uid=ctx.uid, perm=perm)
        short = summary if len(summary) <= 60 else summary[:60] + "…"
        await router.reply(
            ctx,
            f"⚠️ 待确认的操作：\n{short}\n\n"
            f"回复「确认」执行，「取消」放弃（{CONFIRM_TTL} 秒内有效）。")

    @router.command("kun绑定邮箱", usage="kun绑定邮箱 <邮箱>",
                    help="向账号邮箱发送验证码，完成无密码绑定（推荐）")
    async def kun_bind_email(ctx, args):
        email = args.strip()
        if "@" not in email or "." not in email:
            return await router.reply(
                ctx, "用法：kun绑定邮箱 <邮箱>\n"
                     "（需该邮箱已在 KunClient 账号上绑定过；发送后 1 分钟内收到验证码，"
                     "再发 kun验证 <验证码> 完成绑定）")
        try:
            await api.request("POST", "/api/auth/send-code",
                              json_body={"email": email, "type": "login"})
        except KunApiError as e:
            hint = ""
            if "未注册" in str(e):
                hint = "\n（该邮箱没有绑定在任何 KunClient 账号上，请先在客户端/官网绑定邮箱）"
            return await router.reply(ctx, f"❌ 验证码发送失败：{e}{hint}")
        api._pending_binds[ctx.uid] = {"email": email, "ts": time.time()}
        await router.reply(
            ctx, f"📬 验证码已发送到 {email}（10 分钟内有效）。\n"
                 "发送：kun验证 <验证码> 完成绑定。")

    @router.command("kun验证", usage="kun验证 <验证码>", help="输入邮箱收到的验证码，完成绑定")
    async def kun_verify(ctx, args):
        code = args.strip()
        pend = api._pending_binds.get(ctx.uid)
        if not pend:
            return await router.reply(ctx, "请先发送 kun绑定邮箱 <邮箱> 获取验证码。")
        if time.time() - pend["ts"] > 600:
            api._pending_binds.pop(ctx.uid, None)
            return await router.reply(ctx, "绑定会话已过期（10 分钟），请重新发送 kun绑定邮箱 <邮箱>。")
        if not code:
            return await router.reply(ctx, "用法：kun验证 <验证码>")
        data = await api.request("POST", "/api/auth/code-login",
                                 json_body={"email": pend["email"], "code": code})
        api._pending_binds.pop(ctx.uid, None)
        users = bot.storage.data.setdefault("kun_users", {})
        users[ctx.uid] = {
            "username": data.get("username"),
            "email": pend["email"],
            "token": data.get("token"),
            "token_expire": time.time() + int(data.get("expiresIn", 86400)),
            "bind_type": "email",
            "bound_at": time.strftime("%Y-%m-%d %H:%M"),
        }
        api._user_tokens.pop(ctx.uid, None)
        await bot.storage.save()
        await router.reply(
            ctx, f"✅ 绑定成功！账号：{data.get('username')}\n"
                 "现在可用：kun我 ／ kun我的卡密 ／ kun兑换 卡密 ／ kun反馈 ／ kun公告 ／ kun用量\n"
                 "（全程未使用密码；解绑发送 kun解绑）")

    @router.command("kun注册", usage="kun注册 <邮箱> <用户名> <密码>",
                    help="注册 KunClient 账号（邮箱收验证码 → kun注册验证）")
    async def kun_register(ctx, args):
        tokens = args.split()
        if len(tokens) < 3:
            return await router.reply(
                ctx, "用法：kun注册 <邮箱> <用户名> <密码>\n"
                     "用户名 3-20 位（字母/数字/下划线），密码至少 6 位。\n"
                     "发送后会往邮箱发验证码，再发 kun注册验证 <验证码> 完成注册。")
        email, username, password = tokens[0], tokens[1], tokens[2]
        import re as _re
        if not _re.fullmatch(r"[a-zA-Z0-9_]{3,20}", username):
            return await router.reply(ctx, "❌ 用户名需 3-20 个字符（字母/数字/下划线）。")
        if len(password) < 6:
            return await router.reply(ctx, "❌ 密码至少 6 位。")
        try:
            await api.request("POST", "/api/auth/send-code",
                              json_body={"email": email, "type": "register"})
        except KunApiError as e:
            return await router.reply(ctx, f"❌ 验证码发送失败：{e}")
        api._pending_binds[ctx.uid] = {"type": "register", "email": email,
                                          "username": username, "password": password,
                                          "ts": time.time()}
        await router.reply(
            ctx, f"📬 注册验证码已发送到 {email}（10 分钟内有效）。\n"
                 "发送：kun注册验证 <验证码> 完成注册（成功后自动绑定当前身份）。")

    @router.command("kun注册验证", usage="kun注册验证 <验证码>", help="输入邮箱验证码完成注册")
    async def kun_register_verify(ctx, args):
        code = args.strip()
        pend = api._pending_binds.get(ctx.uid)
        if not pend or pend.get("type") != "register":
            return await router.reply(ctx, "请先发送 kun注册 <邮箱> <用户名> <密码> 获取验证码。")
        if time.time() - pend["ts"] > 600:
            api._pending_binds.pop(ctx.uid, None)
            return await router.reply(ctx, "注册会话已过期（10 分钟），请重新 kun注册。")
        if not code:
            return await router.reply(ctx, "用法：kun注册验证 <验证码>")
        data = await api.request("POST", "/api/auth/code-register", json_body={
            "email": pend["email"], "code": code, "username": pend["username"],
            "password": pend["password"], "agree": True})
        api._pending_binds.pop(ctx.uid, None)
        users = bot.storage.data.setdefault("kun_users", {})
        users[ctx.uid] = {
            "username": data.get("username"),
            "email": pend["email"],
            "token": data.get("token"),
            "token_expire": time.time() + int(data.get("expiresIn", 86400)),
            "bind_type": "email",
            "bound_at": time.strftime("%Y-%m-%d %H:%M"),
        }
        api._user_tokens.pop(ctx.uid, None)
        await bot.storage.save()
        await router.reply(
            ctx, f"🎉 注册成功！账号：{data.get('username')}（已自动绑定当前身份）\n"
                 "现在可用：kun我 ／ kun兑换 卡密 ／ kun反馈 等。")

    @router.command("kun找回", usage="kun找回 <邮箱>", help="忘记密码：向账号邮箱发送重置验证码")
    async def kun_reset_start(ctx, args):
        email = args.strip()
        if "@" not in email or "." not in email:
            return await router.reply(ctx, "用法：kun找回 <邮箱> → 收到验证码后 kun找回验证 <验证码> <新密码>")
        try:
            await api.request("POST", "/api/auth/send-code",
                              json_body={"email": email, "type": "reset"})
        except KunApiError as e:
            hint = ""
            if "未注册" in str(e):
                hint = "\n（该邮箱没有绑定在任何 KunClient 账号上）"
            return await router.reply(ctx, f"❌ 验证码发送失败：{e}{hint}")
        api._pending_binds[ctx.uid] = {"type": "reset", "email": email, "ts": time.time()}
        await router.reply(
            ctx, f"📬 重置验证码已发送到 {email}（10 分钟内有效）。\n"
                 "发送：kun找回验证 <验证码> <新密码>（新密码至少 6 位）")

    @router.command("kun找回验证", usage="kun找回验证 <验证码> <新密码>", help="验证码 + 新密码，完成找回")
    async def kun_reset_verify(ctx, args):
        tokens = args.split()
        pend = api._pending_binds.get(ctx.uid)
        if not pend or pend.get("type") != "reset":
            return await router.reply(ctx, "请先发送 kun找回 <邮箱> 获取验证码。")
        if time.time() - pend["ts"] > 600:
            api._pending_binds.pop(ctx.uid, None)
            return await router.reply(ctx, "会话已过期（10 分钟），请重新 kun找回 <邮箱>。")
        if len(tokens) < 2:
            return await router.reply(ctx, "用法：kun找回验证 <验证码> <新密码>")
        code, newpwd = tokens[0], tokens[1]
        if len(newpwd) < 6:
            return await router.reply(ctx, "❌ 新密码至少 6 位。")
        await api.request("POST", "/api/auth/reset-password",
                          json_body={"email": pend["email"], "code": code, "newPassword": newpwd})
        api._pending_binds.pop(ctx.uid, None)
        # 同步更新本机绑定的密码型凭据（如有）
        rec = bot.storage.data.get("kun_users", {}).get(ctx.uid)
        if rec and rec.get("password") and rec.get("email") == pend["email"]:
            rec["password"] = newpwd
            await bot.storage.save()
        await router.reply(ctx, "✅ 密码已重置，用新密码在客户端登录即可。")

    @router.command("kun绑定", usage="kun绑定 <用户名> <密码>（备选，建议 kun绑定邮箱）",
                    help="用密码绑定（kun绑定邮箱 更安全）")
    async def kun_bind(ctx, args):
        tokens = args.split()
        if len(tokens) < 2:
            return await router.reply(
                ctx, "用法：kun绑定 <用户名> <密码>\n"
                     "⚠️ 群里发密码会被别人看到，推荐改用：kun绑定邮箱 <邮箱>\n"
                     "（1. kun绑定邮箱 收验证码 → 2. kun验证 <验证码>，全程无需密码）")
        username, password = tokens[0], tokens[1]
        import aiohttp
        try:
            async with aiohttp.ClientSession() as session:
                await api._login_user(session, ctx.uid, username, password)
        except KunApiError as e:
            return await router.reply(ctx, f"❌ 绑定失败：{e}")
        users = bot.storage.data.setdefault("kun_users", {})
        users[ctx.uid] = {"username": username, "password": password,
                             "bound_at": time.strftime("%Y-%m-%d %H:%M")}
        await bot.storage.save()
        await router.reply(
            ctx, f"✅ 绑定成功！账号：{username}\n"
                 "现在可用：kun我 ／ kun我的卡密 ／ kun兑换 卡密 ／ kun反馈 ／ kun公告 ／ kun用量\n"
                 "（解绑发送 kun解绑；凭据只存在机器人本地）")

    @router.command("kun解绑", help="解除 QQ 与 KunClient 账号的绑定")
    async def kun_unbind(ctx, args):
        users = bot.storage.data.get("kun_users", {})
        if ctx.uid not in users:
            return await router.reply(ctx, "你还没有绑定 KunClient 账号。")
        users.pop(ctx.uid)
        api._user_tokens.pop(ctx.uid, None)
        await bot.storage.save()
        await router.reply(ctx, "✅ 已解绑，机器人本地不再保存你的凭据。")

    @router.command("kun我", help="查看自己的 KunClient 账号与会员状态")
    async def kun_me(ctx, args):
        rec = _bound(ctx)
        if rec is None:
            return await router.reply(ctx, UNBOUND_TIP)
        profile, ent = await asyncio.gather(
            api.user_request(ctx.uid, rec, "GET", "/api/user/full-profile"),
            api.user_request(ctx.uid, rec, "GET", "/api/client/entitlement"),
        )
        role = ROLE_LABEL.get(profile.get("role"), profile.get("role"))
        if ent.get("isVip"):
            left = ent.get("vipDaysLeft")
            if not isinstance(left, (int, float)) or left > 36500 or "2999" in str(ent.get("vipExpireAt")):
                vip = "永久会员"
            else:
                vip = f"会员（剩 {int(left)} 天）"
        else:
            vip = "普通用户"
        lic = ""
        if "licensed" in ent and not ent.get("licensed"):
            lic = f"\n⚠️ 授权状态：{ent.get('reason') or '不可用'}"
        await router.reply(
            ctx, f"👤 {profile.get('username')}\n"
                 f"角色：{role} ｜ 状态：{profile.get('status') or '—'}\n"
                 f"会员：{vip}\n"
                 f"邮箱：{profile.get('email') or '未绑定'}\n"
                 f"签名：{profile.get('signature') or '无'}\n"
                 f"最近登录：{profile.get('last_login') or '—'}{lic}")

    @router.command("kun我的卡密", help="查看自己名下的卡密")
    async def kun_mykeys(ctx, args):
        rec = _bound(ctx)
        if rec is None:
            return await router.reply(ctx, UNBOUND_TIP)
        keys = await api.user_request(ctx.uid, rec, "GET", "/api/user/keys")
        if not keys:
            return await router.reply(ctx, "你名下还没有卡密。")
        lines = [f"🔑 名下卡密（{len(keys)} 张）："]
        lines += [f"· {k.get('key_code')}（{k.get('status')}）" for k in keys[:10]]
        if len(keys) > 10:
            lines.append(f"…另有 {len(keys) - 10} 张")
        await router.reply(ctx, "\n".join(lines))

    @router.command("kun兑换", usage="kun兑换 <卡密>", help="兑换卡密为自己的会员时长")
    async def kun_redeem(ctx, args):
        rec = _bound(ctx)
        if rec is None:
            return await router.reply(ctx, UNBOUND_TIP)
        key = args.strip()
        if not key:
            return await router.reply(ctx, "用法：kun兑换 <卡密>（如 KUN-VIP-XXXX-XXXX）")
        res = await api.user_request(ctx.uid, rec, "POST", "/api/auth/redeem",
                                     json_body={"key": key.strip().upper()})
        await router.reply(ctx, f"🎉 {res.get('message', '兑换成功')}")

    @router.command("kun反馈", usage="kun反馈 <标题> <内容>", help="向开发者提交问题反馈")
    async def kun_feedback(ctx, args):
        rec = _bound(ctx)
        if rec is None:
            return await router.reply(ctx, UNBOUND_TIP)
        parts = args.split(None, 1)
        if not parts:
            return await router.reply(ctx, "用法：kun反馈 <标题> <内容>\n例：kun反馈 无法识别设备 插上后 LSUSB 列表为空")
        title = parts[0].strip()
        content = parts[1].strip() if len(parts) > 1 else title
        if len(title) > 60:
            title = title[:60]
        if len(content) < 5:
            who = ctx.uid if not bot.identity.is_openid_uid(ctx.uid) else "未绑定QQ"
            content = f"{title}（来自 QQ {who}）"
        res = await api.user_request(ctx.uid, rec, "POST", "/api/feedback",
                                     json_body={"category": "bug", "title": title,
                                                "content": content, "contact": ctx.uid})
        await router.reply(ctx, f"✅ 反馈已提交：{res.get('message', '感谢反馈')}")

    @router.command("kun公告", help="查看 KunClient 服务器公告")
    async def kun_notice(ctx, args):
        rec = _bound(ctx)
        if rec is None:
            return await router.reply(ctx, UNBOUND_TIP)
        res = await api.user_request(ctx.uid, rec, "GET", "/api/announcement")
        text = (res or {}).get("content", "").strip()
        await router.reply(ctx, f"📢 公告：\n{text}" if text else "当前没有公告。")

    @router.command("kun用量", help="查看自己最近的使用记录")
    async def kun_usage(ctx, args):
        rec = _bound(ctx)
        if rec is None:
            return await router.reply(ctx, UNBOUND_TIP)
        res = await api.user_request(ctx.uid, rec, "GET", "/api/usage/logs?limit=5")
        logs = (res or {}).get("logs") or []
        total = (res or {}).get("total", 0)
        if not logs:
            return await router.reply(ctx, f"暂无使用记录（累计 {total} 条）。")
        lines = [f"🧾 最近使用（共 {total} 条）："]
        for entry in logs[:5]:
            lines.append(f"· [{entry.get('created_at', '')[:16]}] {entry.get('action') or ''} {entry.get('detail') or ''}".rstrip())
        await router.reply(ctx, "\n".join(lines))

    @router.command("kun版本", help="查看 KunClient 客户端最新版本")
    async def kun_version(ctx, args):
        v = await api.request("GET", "/api/app/version")
        lines = [f"📦 KunClient 最新版本：{v.get('version')}（{v.get('buildDate')}）"]
        if v.get("forceUpdate"):
            lines.append("⚡ 该版本为强制更新")
        if v.get("changelog"):
            cl = clean_changelog(v["changelog"])
            lines.append("📝 更新内容：")
            lines += [f"· {ln}" for ln in cl[:5]]
        dl = v.get("downloadUrl") or ""
        if dl.startswith("/"):
            dl = api.base + dl
        lines.append(f"⬇️ 下载：{dl}")
        await router.reply(ctx, "\n".join(lines))

    # ---------- 超管管理功能 ----------

    @router.command("kun状态", aliases=("kun服务器",), perm="server.manage",
                    help="KunClient 服务器统计与版本（超级管理员及以上）")
    async def kun_status(ctx, args):
        stats, version = await asyncio.gather(
            api.request("GET", "/api/stats"),
            api.request("GET", "/api/app/version"),
        )
        changelog = clean_changelog(version.get("changelog"))
        dl = version.get("downloadUrl") or ""
        if dl.startswith("/"):
            dl = api.base + dl
        lines = [
            "🖥 KunClient 服务端状态",
            "━━━━━━━━━━━━",
            f"📦 客户端版本：{version.get('version')}（{version.get('buildDate')}）",
            f"⚡ 强制更新：{'开' if version.get('forceUpdate') else '关'}",
            f"👥 用户：{stats.get('totalUsers')}（正常 {stats.get('activeUsers')}）",
            f"🔑 卡密：{stats.get('totalKeys')}（有效 {stats.get('activeKeys')}）",
            f"📊 24h 登录：成功 {stats.get('loginsToday')} ｜ 失败 {stats.get('failedLogins')}",
            f"🧾 使用记录：{stats.get('totalUsage')} 条",
            f"⬇️ 下载：{dl}",
        ]
        if changelog:
            lines.append(f"📝 最新更新：{changelog[0][:60]}")
        await router.reply(ctx, "\n".join(lines))

    @router.command("kun在线", perm="server.manage", help="KunClient 当前在线用户（超级管理员及以上）")
    async def kun_online(ctx, args):
        users = await api.request("GET", "/api/users")
        online = [u for u in users if u.get("online")]
        lines = [f"🟢 在线用户（{len(online)}/{len(users)}）"]
        if online:
            lines += [f"· {u.get('username')}（{ROLE_LABEL.get(u.get('role'), u.get('role'))}）"
                      for u in online[:20]]
            if len(online) > 20:
                lines.append(f"…另有 {len(online) - 20} 人")
        else:
            lines.append("（当前没有用户在线）")
        await router.reply(ctx, "\n".join(lines))

    @router.command("kun用户", perm="server.manage", usage="kun用户 <名字>", help="查询用户详情（超级管理员及以上）")
    async def kun_user(ctx, args):
        name = args.strip()
        if not name:
            return await router.reply(ctx, "用法：kun用户 <名字>")
        found = await _find_user(api, name)
        if found is None:
            return await router.reply(ctx, f"没找到用户「{name}」。")
        if isinstance(found, tuple):
            return await router.reply(ctx, f"「{name}」匹配到多个用户，请用更完整的名字。")
        await router.reply(ctx, _fmt_user(found))

    @router.command("kun踢", perm="server.manage", usage="kun踢 <名字>", help="云端强制下线（超级管理员及以上）")
    async def kun_kick(ctx, args):
        name = args.strip()
        if not name:
            return await router.reply(ctx, "用法：kun踢 <名字>（该用户客户端将在约 30 秒内退出登录）")
        u = await _find_user(api, name)
        if u is None:
            return await router.reply(ctx, f"没找到用户「{name}」。")
        if isinstance(u, tuple):
            return await router.reply(ctx, "匹配到多个用户，请用更完整的名字。")
        res = await api.request("POST", f"/api/users/{u['id']}/kick")
        await router.reply(ctx, f"✅ {res.get('message', '已强制下线')}")

    @router.command("kunvip", perm="server.manage", usage="kunvip <名字> <天数|永久|取消>",
                    help="会员管理：加天数 / 设永久 / 取消（超级管理员及以上）")
    async def kun_vip(ctx, args):
        tokens = args.split()
        if len(tokens) < 2:
            return await router.reply(ctx, "用法：kunvip <名字> <天数|永久|取消>\n例：kunvip 小坤 30 ／ kunvip 小坤 永久 ／ kunvip 小坤 取消")
        name, op = tokens[0], tokens[1]
        u = await _find_user(api, name)
        if u is None:
            return await router.reply(ctx, f"没找到用户「{name}」。")
        if isinstance(u, tuple):
            return await router.reply(ctx, "匹配到多个用户，请用更完整的名字。")
        if op in ("永久", "forever"):
            body = {"permanent": True}
        elif op in ("取消", "清空", "remove"):
            body = {"permanent": False}
        elif op.lstrip("-").isdigit():
            body = {"days": int(op)}  # 正数叠加，负数扣减
        else:
            return await router.reply(ctx, "参数支持：天数（正加负减）、永久、取消。")
        res = await api.request("PUT", f"/api/users/{u['id']}/vip", json_body=body)
        await router.reply(ctx, f"✅ {res.get('message', '已更新会员')}")

    @router.command("kun卡密", perm="server.manage", usage="kun卡密 [数量] [天数] [svip]",
                    help="生成卡密：kun卡密 5 30 svip（天数 0 = 登录卡密）（超级管理员及以上）")
    async def kun_keys(ctx, args):
        tokens = args.split()
        count, days, level = 1, 0, "vip"
        nums = [t for t in tokens if t.lstrip("-").isdigit()]
        if len(nums) >= 1:
            count = max(1, min(int(nums[0]), 20))
        if len(nums) >= 2:
            days = max(0, min(int(nums[1]), 36500))
        if "svip" in args.lower():
            level = "svip"
        res = await api.request("POST", "/api/keys", json_body={"count": count, "days": days, "level": level})
        keys = res if isinstance(res, list) else (res or {}).get("keys") or []
        if not keys:
            return await router.reply(ctx, "⚠️ 服务器未返回卡密，请稍后再试。")
        kind = f"{days} 天 {level.upper()}" if days > 0 else "登录卡密"
        lines = [f"🔑 已生成 {len(keys)} 张卡密（{kind}）："]
        lines += [f"{i}. {k}" for i, k in enumerate(keys, 1)]
        lines.append("（请私发给学生/用户，不要留在群里）")
        await router.reply(ctx, "\n".join(lines))

    @router.command("kun公告设置", usage="kun公告设置 <文本>", perm="server.manage",
                    help="设置客户端公告（需回复「确认」）（超级管理员及以上）")
    async def kun_set_notice(ctx, args):
        text = args.strip()
        if not text:
            return await router.reply(ctx, "用法：kun公告设置 <文本>（清空发送：kun公告设置 空）")
        value = "" if text == "空" else text
        summary = "清空客户端公告" if not value else f"把客户端公告改为：{value}"

        async def _run(c):
            await api.request("PUT", "/api/admin/settings", json_body={"announcement": value})
            bot.perms.audit("server.notice.set", c, detail=summary)
            await bot.storage.save()
            await router.reply(c, "✅ 客户端公告已更新（客户端下次请求 /api/announcement 时生效）。")

        await _arm_confirm(ctx, perm="server.manage", summary=summary, run=_run)

    @router.command("kun更新开关", usage="kun更新开关 开|关", perm="server.manage",
                    help="开关客户端强制更新（需回复「确认」）（超级管理员及以上）")
    async def kun_force_update(ctx, args):
        sub = args.strip()
        if sub not in ("开", "开启", "关", "关闭"):
            v = await api.request("GET", "/api/app/version")
            return await router.reply(
                ctx, f"当前强制更新：{'开' if v.get('forceUpdate') else '关'}\n用法：kun更新开关 开 / 关")
        turning_on = sub in ("开", "开启")
        summary = ("开启客户端强制更新（所有旧版客户端将被拦去升级）" if turning_on
                   else "关闭客户端强制更新")

        async def _run(c):
            await api.request("PUT", "/api/admin/settings",
                              json_body={"force_update": "1" if turning_on else "0"})
            bot.perms.audit("server.force_update.set", c, detail=summary)
            await bot.storage.save()
            await router.reply(c, f"✅ 客户端强制更新已{'开启' if turning_on else '关闭'}。")

        await _arm_confirm(ctx, perm="server.manage", summary=summary, run=_run)

    # ---------- 客户端发版自动播报 ----------

    def _release_status() -> str:
        w = bot.release
        st = w.state()
        targets = w.targets()
        lines = [
            "📣 客户端发版自动播报",
            "━━━━━━━━━━━━",
            f"开关：{'开' if w.enabled else '关'}",
            f"轮询间隔：{w.interval} 秒",
            f"目标群：{len(targets)} 个",
        ]
        if targets:
            lines += [f"  · …{g[-8:]}" for g in targets[:5]]
            if len(targets) > 5:
                lines.append(f"  （其余 {len(targets) - 5} 个略）")
        else:
            lines.append("  ⚠️ 尚未登记目标群 —— 不会自动播报")
        if st.get("last_version"):
            lines.append(f"上次播报版本：{st['last_version']}"
                         + (f"（{st.get('last_time')}）" if st.get("last_time") else ""))
            if st.get("last_result"):
                lines.append(f"上次结果：{st['last_result']}")
            if st.get("last_failed"):
                lines.append("  （失败多为未开通主动消息权限：403，需在 QQ 开放平台申请）")
        else:
            lines.append("上次播报版本：尚无（首轮只记录基线，不群发）")
        lines.append("")
        lines.append("用法：kun发版播报 开|关 ／ kun发版播报 发送 ／ "
                     "kun发版播报 本群|移除本群 ／ kun发版播报 全部|清空")
        return "\n".join(lines)

    @router.command("kun发版播报", perm="server.manage",
                    help="客户端发版自动播报：状态 / 开|关 / 发送 / 目标群（超级管理员及以上）")
    async def kun_release_broadcast(ctx, args):
        w = bot.release
        sub = args.strip()
        st = w.state()
        if sub in ("开", "开启", "启用"):
            st["enabled"] = True
            await bot.storage.save()
            return await router.reply(ctx, "✅ 发版自动播报已开启。")
        if sub in ("关", "关闭", "停用"):
            st["enabled"] = False
            await bot.storage.save()
            return await router.reply(ctx, "✅ 发版自动播报已关闭（仍会记录版本，不群发）。")
        if sub in ("本群", "加本群", "添加本群"):
            if not ctx.group_openid:
                return await router.reply(ctx, "请在目标群里发送「kun发版播报 本群」。")
            targets = w.targets()
            if ctx.group_openid in targets:
                return await router.reply(ctx, "ℹ️ 本群已在播报目标里。")
            targets.append(ctx.group_openid)
            st["targets"] = targets
            await bot.storage.save()
            return await router.reply(ctx, f"✅ 已把本群加入播报目标（共 {len(targets)} 个群）。")
        if sub in ("移除本群", "删本群"):
            if not ctx.group_openid:
                return await router.reply(ctx, "请在要移除的群里发送本命令。")
            targets = [g for g in w.targets() if g != ctx.group_openid]
            st["targets"] = targets
            await bot.storage.save()
            return await router.reply(ctx, f"✅ 已把本群移出播报目标（剩 {len(targets)} 个群）。")
        if sub == "全部":
            known = list(bot.groups.keys())
            if not known:
                return await router.reply(ctx, "ℹ️ 机器人还没在任何群收到过消息，无法枚举群列表。")
            st["targets"] = known
            await bot.storage.save()
            return await router.reply(
                ctx, f"✅ 已把机器人收到过消息的 {len(known)} 个群设为播报目标。")
        if sub in ("清空", "重置"):
            st["targets"] = []
            await bot.storage.save()
            return await router.reply(ctx, "✅ 播报目标已清空（不会自动群发；发送前请先登记目标群）。")
        if sub == "发送":
            v = await api.request("GET", "/api/app/version")
            ver = v.get("version") or "?"

            async def _run(c):
                msg = await w.manual(c)
                bot.perms.audit("server.release.broadcast", c, detail=f"手动播报版本 {ver}")
                await bot.storage.save()
                await router.reply(c, msg)

            return await _arm_confirm(
                ctx, perm="server.manage",
                summary=f"立即向 {len(w.targets())} 个目标群播报当前版本 {ver}", run=_run)
        # 无参数 / 其他：状态
        return await router.reply(ctx, _release_status())

    @router.command("kun反馈列表", usage="kun反馈列表 [条数]", perm="server.manage",
                    help="查看最近的用户反馈（超级管理员及以上）")
    async def kun_feedback_list(ctx, args):
        n = min(max(1, int(args.strip()) if args.strip().isdigit() else 5), 20)
        res = await api.request("GET", f"/api/feedback?limit={n}")
        items = (res or {}).get("data") or []
        open_count = (res or {}).get("openCount", 0)
        if not items:
            return await router.reply(ctx, "暂无用户反馈。")
        cat = {"bug": "缺陷", "suggestion": "建议", "account": "账号", "other": "其他"}
        lines = [f"📨 最近反馈（未处理 {open_count} 条）："]
        lines += [f"#{f.get('id')} [{cat.get(f.get('category'), f.get('category'))}] "
                  f"{f.get('title')}（{f.get('username')}）" for f in items[:n]]
        await router.reply(ctx, "\n".join(lines))

    @router.command("kun重置密码", perm="server.manage", usage="kun重置密码 <名字> <新密码>",
                    help="重置用户密码（至少 6 位，会踢下线）（超级管理员及以上）")
    async def kun_resetpwd(ctx, args):
        tokens = args.split()
        if len(tokens) < 2:
            return await router.reply(ctx, "用法：kun重置密码 <名字> <新密码>（至少 6 位）")
        name, newpwd = tokens[0], tokens[1]
        if len(newpwd) < 6:
            return await router.reply(ctx, "新密码至少 6 个字符。")
        u = await _find_user(api, name)
        if u is None:
            return await router.reply(ctx, f"没找到用户「{name}」。")
        if isinstance(u, tuple):
            return await router.reply(ctx, "匹配到多个用户，请用更完整的名字。")
        res = await api.request("POST", f"/api/users/{u['id']}/reset-password",
                                json_body={"newPassword": newpwd})
        await router.reply(ctx, f"✅ {res.get('message', '密码已重置')}（{u.get('username')}）")
