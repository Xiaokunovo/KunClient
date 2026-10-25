"""开发自测：离线模拟群消息驱动命令层，验证交互流程（不发送任何真实 QQ 消息）。

全流程不联网：命令层、权限、身份、群管理接口段都是本地假数据 / 假 API。

运行：
    py main.py --devtest     全流程
    py main.py --grouptest   只跑群管理接口段（撤回/禁言/入群审批/移除成员/群信息）
"""
import asyncio
import os

from core.storage import fresh_data
from main import Bot, load_config


async def _flow(bot, sent):
    async def ev(content, user="U1", group="G1", expect_block=False):
        print(f"\n{'=' * 50}\n>>> 用户发送: {content}")
        before = len(sent)
        await bot.router.handle_event("GROUP_AT_MESSAGE_CREATE", {
            "id": f"m{before}-{content}", "content": content,
            "group_openid": group, "author": {"id": user, "username": user}})
        replies = sent[before:]
        if expect_block:
            if replies:
                print(f"❌ 期望被拦截，却收到 {len(replies)} 条回复")
            else:
                print("<<< 已被拦截（无回复）✅")
            return
        if not replies:
            print("(无回复)")
        for c in replies:
            print("<<< 机器人回复:")
            print(c)
        return replies

    # —— 身份体系：QQ 号绑定 + 全局拉黑（纯离线，可单独跑）——
    await ev("我的QQ号")                      # 未绑定时的提示
    await ev("签到")                          # 未绑定时先攒一份数据（占位键）
    await ev("积分")
    await ev("绑定QQ 2089449185")             # 绑定后应认领上面那份积分
    await ev("积分")                          # 积分应跟到 QQ 号上
    await ev("我的QQ号")
    await ev("绑定QQ 12345678")               # 应拒绝改绑
    await ev("绑定QQ abc")                   # 格式错

    # U1 是 owner_qq，绑定后自动成为超级管理员
    print("\n>>> 断言：U1 已绑到 owner_qq 并自动获得超管权限")
    assert bot.identity.qq_of("U1") == "2089449185", bot.identity.qq_of("U1")
    assert bot.is_super(_mkctx(bot, "U1")), "owner 绑定后应为超管"
    sign = bot.storage.data.get("sign", {}).get("G1", {})
    assert "2089449185" in sign and "openid:U1" not in sign, f"积分未认领到 QQ 号: {list(sign)}"
    print("✅ 绑定 / 认领历史数据 / 超管自动生效")

    # 冒领保护
    await ev("绑定QQ 2089449185", user="U2")   # 已被占用 → 拒绝
    # 跨群隔离 + 并号
    await ev("绑定QQ 2089449185", user="U2", group="G2")
    await ev("关联身份 2089449185 U2", group="G2", user="U1")
    assert bot.identity.qq_of("U2") == "2089449185", bot.identity.qq_of("U2")
    print("✅ 冒领被拒 + 超管并号成功")

    # —— 关键体验：管理员完全不知道 QQ 号，只知道昵称/——
    await ev("绑定QQ 123456789", user="U5")    # U5 绑了自己的号
    await ev("拉黑 U5")                        # 按昵称拉黑（U5 已在 G1 名册里）
    await ev("拉黑列表")
    await ev("取消拉黑 U5")
    await ev("封号 U5")                        # 短命令 + 机器人自己查 QQ 号
    assert "123456789" in bot.identity.global_blacklist(), bot.identity.global_blacklist()
    print("✅ 管理员不用知道 QQ 号：拉黑/封号 直接报昵称即可")
    await ev("封号列表")
    await ev("解封 123456789")

    # 认得出是谁、但没绑 QQ → 跨群封不了，应降级为 openid 兜底拦截 + 本群并说清下一步
    await ev("签到", user="U9")                     # U9 先发言 → 进名册，但没绑 QQ
    await ev("封号 U9")                             # 封号仅超管，用 U1 发
    g_bl = bot.storage.group_section("blacklist", "G1")
    assert "openid:U9" in g_bl, g_bl
    assert "U9" in bot.identity.identity_ban_list(), bot.identity.identity_ban_list()
    assert not bot.identity.global_blacklist(), bot.identity.global_blacklist()
    print("✅ 未绑定的号无法按号封 → 已降级为 openid 兜底全局拦截 + 本群拉黑")

    # 超管自己知道号 → 直接登记，不必等对方绑定，然后就能全局封
    await ev("设QQ号 555555555 U9")
    assert bot.identity.qq_of("U9") == "555555555", bot.identity.qq_of("U9")
    assert "555555555" in bot.identity.global_blacklist(), bot.identity.global_blacklist()
    assert "U9" not in bot.identity.identity_ban_list()
    print("✅ 超管登记 QQ 号 → openid 兜底拦截自动升级为按号封，立刻全局生效")
    await ev("封号 U9")                             # 现在能跨群封了
    assert "555555555" in bot.identity.global_blacklist(), bot.identity.global_blacklist()
    await ev("签到", user="U9", group="G9", expect_block=True)   # 别的群也拦
    print("✅ 登记后封号全局生效")
    await ev("解封 U9")
    await ev("设QQ号 555555555 U9", user="U7")       # 群管理员无权登记
    print("✅ 群管理员无法登记 QQ 号")

    # —— RBAC 权限体系（v3 重做：四级角色 / 中心化 perm / 目标保护 / 审计）——
    await ev("签到", user="U7")                      # U7 先发言进名册（普通用户）
    await ev("拉黑 U1", user="U7")                   # 普通人 → 权限不足被中心化拒绝
    assert "2089449185" not in bot.storage.group_section("blacklist", "G1")
    await ev("设置管理 U7")                          # owner 显式授权 U7 为群管理员
    roles_g1 = (bot.storage.data.get("roles") or {}).get("G1", {})
    assert roles_g1.get("openid:U7") == "group_admin", roles_g1
    await ev("角色", user="U7")                      # U7 看到自己的角色
    await ev("拉黑 U1", user="U7")                   # 群管也不能封主人 → 目标保护
    assert "2089449185" not in bot.storage.group_section("blacklist", "G1")
    pts_before = bot.storage.data.get("sign", {}).get("G1", {}).get("2089449185", {}).get("total", 0)
    await ev("积分调整 2089449185 +10", user="U7")   # 群管不能对权限更高的人调积分
    pts_after = bot.storage.data.get("sign", {}).get("G1", {}).get("2089449185", {}).get("total", 0)
    assert pts_after == pts_before, (pts_before, pts_after)
    await ev("封号 U5", user="U7")                   # 群管无全局封号权限 → 拒
    assert "123456789" not in bot.identity.global_blacklist()
    await ev("超管添加 U7", user="U1")               # 仅主人可授权超管
    assert (bot.storage.data.get("roles") or {}).get("*", {}).get("openid:U7") == "super"
    await ev("超管添加 U7", user="U5")               # 超管也不能授权超管 → 拒
    await ev("超管移除 U7", user="U1")               # 仅主人可撤销
    assert "openid:U7" not in (bot.storage.data.get("roles") or {}).get("*", {})
    await ev("审计日志")                             # 授权/封禁/解绑全程留痕
    assert len(bot.perms.audit_records(50)) >= 8, bot.perms.audit_records(50)
    print("✅ RBAC：授权/降权/目标保护/权限拒绝/防下克上/审计 全流程")

    # 完全不认识的人（从没跟机器人说过话）→ 明确提示，而不是静默
    await ev("封号 从没说过话的人")

    # 全局拉黑真的跨群 + 跨私聊生效
    await ev("封号 123456789 恶意刷屏")
    await ev("签到", user="U5", group="G9", expect_block=True)
    await ev("解封 123456789")
    await ev("签到", user="U5", group="G9")     # 解封后恢复

    # 拉黑 全 @人 等价于封号
    await ev("拉黑 全 U5")
    assert "123456789" in bot.identity.global_blacklist(), bot.identity.global_blacklist()
    print("✅ 「拉黑 全」短写法等价于封号")
    await ev("取消拉黑 全 U5")
    assert "123456789" not in bot.identity.global_blacklist(), bot.identity.global_blacklist()
    print("✅ 「取消拉黑 全」可解除")

    # 群管理员不能用「拉黑 全」（必须超管）
    await ev("拉黑 全 U5", user="U7")
    assert "123456789" not in bot.identity.global_blacklist(), bot.identity.global_blacklist()
    print("✅ 群管理员无法执行全局封禁")

    await ev("解绑QQ 123456789", user="U5")     # 非超管 → 拒绝
    assert bot.identity.qq_of("U5") == "123456789"
    print("✅ 非超管无法解绑")

    await ev("帮助")                            # 文案应精简
    await ev("帮助 管理")
    await ev("我的ID")

    # —— 机器人自管理与群功能开关 ——
    await ev("公告 设置 本群每周五晚 8 点开黑")
    await ev("帮助")                 # 帮助顶部应显示公告
    await ev("bot状态")
    await ev("bot统计")
    await ev("日志")
    await ev("群管")
    await ev("绑定表")
    await ev("公告 删除")

    # —— 展示层：角色名必须中文；「管理列表」同一个人不能重复出现 ——
    out = await ev("角色", user="U7")
    assert out and "群管理员" in out[0], out
    assert "group_admin" not in out[0], out
    print("✅ 「角色」输出中文角色名（不再漏内部英文名）")

    # 模拟线上真实配置：同一个人既是 owner（按 QQ 号）又出现在 config.admins（按 openid）。
    # 修复前他会在「主人」和「超级管理员」两段各出现一次。
    saved_admins = bot.config.get("admins")
    bot.config["admins"] = ["U1", "1781653834"]
    out = await ev("管理列表")
    text = out[0] if out else ""
    assert "【主人】1 人" in text, text
    assert "【超级管理员】1 人" in text, text
    assert "1781653834" in text, text
    print("✅ 「管理列表」同一个人只出现一次（主人身份优先）")
    bot.config["admins"] = saved_admins

    # —— 服务端「更新鉴权」配套：发布类命令的二次确认 + 审计 ——
    # 发布类命令（改公告 / 切强制更新 / 手动发版播报）不允许「发出即生效」，
    # 必须先回「确认」。这里用假的服务端接口记录调用，验证「未确认前一个请求都不发」。
    calls: list = []
    holder = {"version": "2.0.0.8"}

    async def fake_kun_request(method, path, json_body=None, _retry=True):
        calls.append((method, path, json_body))
        if path == "/api/app/version":
            return {"version": holder["version"], "buildDate": "2026-10-12",
                    "forceUpdate": False,
                    "changelog": "1. 修复登录双发\n2. 主页快捷操作合并为模式自适配",
                    "downloadUrl": "/assets/KunClient-%s.exe" % holder["version"]}
        return {"message": "ok"}

    bot.kun.request = fake_kun_request

    out = await ev("kun公告设置 今晚 23:00 停机维护")
    assert out and "待确认" in out[0], out
    assert not any(p == "/api/admin/settings" for _, p, _ in calls), calls
    print("✅ 发布类命令未确认前不改服务端（只登记待确认）")

    out = await ev("确认")
    assert out and any("✅" in c for c in out), out
    assert any(m == "PUT" and p == "/api/admin/settings" for m, p, _ in calls), calls
    assert any(r["op"] == "server.notice.set" for r in bot.perms.audit_records(50)), \
        bot.perms.audit_records(50)
    print("✅ 「确认」后才真正执行，且已留审计（server.notice.set）")

    # 取消：槽被丢弃，之后再发「确认」无事发生
    calls.clear()
    out = await ev("kun更新开关 开")
    assert out and "待确认" in out[0], out
    out = await ev("取消")
    assert out and any("已取消" in c for c in out), out
    assert not any(p == "/api/admin/settings" for _, p, _ in calls), calls
    out = await ev("确认")
    assert not out, out
    print("✅ 「取消」丢弃待确认操作，且不留残余槽（再发「确认」无响应）")

    # 别人的「确认」不能替你执行（会话按 群+用户 隔离）
    calls.clear()
    await ev("kun更新开关 关")
    out = await ev("确认", user="U5")
    assert not any(p == "/api/admin/settings" for _, p, _ in calls), calls
    # U5 的「确认」只查自己的会话槽（G1:U5），U1 的槽必须原封不动
    assert bot.confirms.peek("G1:2089449185") is not None, "他人确认不该消费本人的槽"
    await ev("确认")   # 本人确认才生效
    assert any(p == "/api/admin/settings" for _, p, _ in calls), calls
    print("✅ 二次确认按会话隔离：他人确认无效，本人确认才生效")

    # —— 客户端发版自动播报 ——
    sent.clear()
    r = await bot.release.check_once()
    assert r and r["event"] == "baseline", r
    assert not sent, sent
    print("✅ 发版播报首轮只记录基线版本，不群发")

    # 安全默认：没登记目标群时即使检测到新版本也不发
    holder["version"] = "2.0.0.9"
    r = await bot.release.check_once()
    assert r and r["event"] == "skipped" and r["reason"] == "no_targets", r
    assert not sent, sent
    print("✅ 未登记目标群 → 检测到新版本也不群发（安全默认）")

    await ev("kun发版播报 本群")
    assert bot.release.targets() == ["G1"], bot.release.targets()

    sent.clear()
    r = await bot.release.check_once()
    assert r and r["event"] == "broadcast", r
    assert sent and "2.0.0.9" in sent[0] and "修复登录双发" in sent[0], sent
    # 服务端 changelog 是「1. xxx」编号格式：播报要剥掉编号，不能出现「· 1.」双重列表符
    assert "· 1." not in sent[0] and "· ·" not in sent[0], sent[0]
    print("✅ 版本号变化 → 自动把更新内容播报到目标群（changelog 编号已归一）")

    # 关掉后即使版本变化也不播
    await ev("kun发版播报 关")
    assert bot.release.enabled is False
    holder["version"] = "2.1.0.0"
    sent.clear()
    r = await bot.release.check_once()
    assert r and r["event"] == "skipped" and r["reason"] == "disabled", r
    assert not sent, sent
    await ev("kun发版播报 开")
    print("✅ 播报开关关闭时不发消息（仍记录版本）")

    # 主动消息权限缺失（403）→ 全部失败：不能推进基线，否则权限开通后这条更新永久丢失
    ok_send = bot.api.send_group_message

    async def boom(target, *, content=None, **kw):
        raise RuntimeError("403 无权限（未开通主动消息能力）")

    bot.api.send_group_message = boom
    holder["version"] = "2.1.0.1"
    baseline = bot.release.state()["last_version"]
    r = await bot.release.check_once()
    assert r and r["event"] == "broadcast_failed", r
    assert bot.release.state()["last_version"] == baseline, bot.release.state()
    bot.api.send_group_message = ok_send
    sent.clear()
    r = await bot.release.check_once()          # 恢复后应自动补发
    assert r and r["event"] == "broadcast" and r["version"] == "2.1.0.1", r
    assert sent and "2.1.0.1" in sent[0], sent
    print("✅ 发送全败不推进基线，下轮自动补发（主动消息权限开通后不会漏发）")

    # 手动播报走二次确认
    calls.clear()
    out = await ev("kun发版播报 发送")
    assert out and "待确认" in out[0], out
    assert not any(p == "/api/admin/settings" for _, p, _ in calls), calls
    sent.clear()
    out = await ev("确认")
    assert sent and holder["version"] in sent[0], sent
    assert any(r["op"] == "server.release.broadcast" for r in bot.perms.audit_records(50))
    print("✅ 手动播报需二次确认，且已留审计（server.release.broadcast）")

    # —— 群管理平台接口（撤回 / 禁言 / 入群审批 / 移除成员 / 群信息）——
    # 抽成独立函数：这段可脱离主流程单独跑（--grouptest）
    await group_flow(bot, sent)

    # 还原测试期间写入的持久化数据
    bot.storage.data = fresh_data()
    await bot.storage.save()


async def group_flow(bot, sent):
    """群管理平台接口（撤回/禁言/入群审批/移除成员/群信息）离线验证。

    全程用假 API 层，一个真实请求都不发；单独成函数是为了能脱离主流程单独跑
    （`py main.py --grouptest`）。
    """
    async def ev(content, user="U1", group="G1"):
        print(f"\n>>> {content}")
        before = len(sent)
        await bot.router.handle_event("GROUP_AT_MESSAGE_CREATE", {
            "id": f"g{before}-{content}", "content": content,
            "group_openid": group, "author": {"id": user, "username": user}})
        out = sent[before:]
        for c in out:
            print("<<<", c)
        return out

    # —— 群管理平台接口（撤回 / 禁言 / 入群审批 / 移除成员 / 群信息）——
    # 这些接口都要求「机器人在群内是管理员」，devtest 用假 API 层验证命令逻辑与参数拼装，
    # 一个真实请求都不发。
    gapi = {"deleted": [], "mutes": [], "approvals": [], "removes": [], "exec": []}

    state = {"role": "admin"}

    async def fake_bot_state(group):
        return {"member_openid": "BOT", "member_role": state["role"],
                "recv_msg_setting": "only_mention", "joined_at": "2026-01-01T00:00:00+08:00"}

    async def fake_group_info(group):
        return {"group_openid": group, "group_name": "测试群", "group_member_num": 42,
                "group_class_text": "科技", "group_tags": ["数码"], "group_finger_memo": "devtest"}

    async def fake_delete(group, mid):
        gapi["deleted"].append(mid)
        return {}

    async def fake_mute(group, members):
        gapi["mutes"].append(members)
        return {}

    async def fake_setting(group):
        return {"global_rule": {"mode": "none"}, "members": [
            {"member_openid": "U11", "username": "U11",
             "mute_expire_at": "2026-10-17T18:00:00+08:00"}]}

    async def fake_join_list(group, cursor="", limit=20):
        return {"list": [
            {"join_request_id": "jr-1", "member_openid": "J1", "username": "申请人甲",
             "apply_source": "self_apply", "apply_at": "2026-10-17T12:00:00+08:00",
             "risk_tips": "", "verify_info": {"method": "verify_message", "verify_message": "求进群"}},
            {"join_request_id": "jr-2", "member_openid": "J2", "username": "申请人乙",
             "apply_source": "invited", "apply_at": "2026-10-17T12:30:00+08:00",
             "risk_tips": "该账号近期被多次拒绝", "verify_info": {}}],
            "next_cursor": ""}

    async def fake_approval(group, member, op, **kw):
        gapi["approvals"].append({"member": member, "op": op,
                                  "req": kw.get("join_request_id"),
                                  "reason": kw.get("reject_reason"),
                                  "black": kw.get("add_to_member_blacklist")})
        return {}

    async def fake_strategies(cursor="", limit=20):
        return {"strategies": [{"strategy_id": "st_test01", "group_ids": ["10****499"],
                                "whitelist_user_count": 3, "is_enable": "on",
                                "expire_at": "2027-08-05T15:30:16+08:00",
                                "remark": "devtest 策略"}], "next_cursor": ""}

    async def fake_exec(sid):
        gapi["exec"].append(sid)
        return {}

    async def fake_remove(group, oids, add_to_member_blacklist=False):
        gapi["removes"].append({"oids": list(oids), "black": add_to_member_blacklist})
        return {"remove_members_result": "success",
                "add_to_member_blacklist_fail_openids": []}

    bot.api.get_bot_state = fake_bot_state
    bot.api.get_group_info = fake_group_info
    bot.api.delete_group_message = fake_delete
    bot.api.set_member_mute = fake_mute
    bot.api.get_restrict_chat_setting = fake_setting
    bot.api.list_join_requests = fake_join_list
    bot.api.approval_join_request = fake_approval
    bot.api.list_join_strategies = fake_strategies
    bot.api.execute_join_strategy = fake_exec
    bot.api.batch_remove_members = fake_remove

    # 真实平台发送成功会返回 message_id —— 撤回机器人自己的消息全靠它
    mid_seq = {"n": 0}

    async def fake_send_with_id(target, *, content=None, **kw):
        mid_seq["n"] += 1
        sent.append(content)
        return {"id": f"MSG{mid_seq['n']}"}

    bot.api.send_group_message = fake_send_with_id

    out = await ev("群信息")
    assert out and "测试群" in out[0] and "管理员" in out[0], out
    print("✅ 「群信息」展示群资料 + 机器人身份（禁言失败时可自检）")

    await ev("签到", user="U11")          # 让 U11 进名册，作为禁言/撤回目标
    assert bot.msglog.count("G1") > 0, "消息台账没记下 message_id"

    out = await ev("撤回")                # 撤机器人上一条回复
    assert gapi["deleted"], "「撤回」没有调用撤回接口"
    assert out and "已撤回" in out[0], out
    print("✅ 「撤回」按台账里的 message_id 撤掉机器人自己的消息")

    before = len(gapi["deleted"])
    out = await ev("撤回 U11")            # 撤成员最近一条 @ 机器人的消息
    assert len(gapi["deleted"]) == before + 1, gapi["deleted"]
    print("✅ 「撤回 @某人」撤成员消息（成员 openid 从事件台账取）")

    # 名册编号：群事件没有 mentions，@ 不出目标 → 「群成员」看编号 + 「#编号」操作
    out = await ev("群成员")
    assert out and "名册" in out[0], out
    idx = next((ln.split(".")[0] for ln in out[0].splitlines()
                if ln.split(". ", 1)[-1].startswith("U11")), None)
    assert idx, out[0]
    out = await ev("群成员 召集")
    assert out and "签到" in out[0] and "名册里" in out[0], out
    print("✅ 「群成员 召集」在群里公开喊话，引导全员 @ 机器人进名册")

    await ev(f"禁言 #{idx} 5分")
    assert gapi["mutes"][-1][0]["member_openid"] == "U11", gapi["mutes"][-1]
    print(f"✅ 「群成员」列名册，「禁言 #{idx} 5分」按编号命中同一个人")

    out = await ev("禁言 #99 5分")
    assert out and "名册" in out[0] and "签到" in out[0], out
    print("✅ 编号越界 → 说明平台限制 + 给出可用写法（不再让用户瞎试 @）")

    # QQ 号：走全局绑定表（qq_bind 跨群），不要求对方在本群 @ 过我
    bot.storage.data.setdefault("qq_bind", {})["13800000001"] = {
        "qq": "13800000001", "at": "2026-10-17 00:00", "openids": ["U11"]}
    bot.identity.rebuild()
    await ev("禁言 13800000001 5分")
    assert gapi["mutes"][-1][0]["member_openid"] == "U11", gapi["mutes"][-1]
    print("✅ 「禁言 <QQ号>」走全局绑定表命中（不必在本群 @ 过我）")

    await ev("禁言 U11 10分")
    assert gapi["mutes"], "「禁言」没有调用禁言接口"
    mute = gapi["mutes"][-1][0]
    assert mute["op"] == "add" and mute["member_openid"] == "U11", mute
    from datetime import datetime as _dt
    delta = (_dt.fromisoformat(mute["mute_expire_at"]) - _dt.now().astimezone()).total_seconds()
    assert 540 <= delta <= 660, (delta, mute["mute_expire_at"])
    print(f"✅ 「禁言 @某人 10分」→ 到期时间 {mute['mute_expire_at']}（{int(delta)}s 后）")

    await ev("解除禁言 U11")
    assert gapi["mutes"][-1][0]["op"] == "del", gapi["mutes"][-1]
    print("✅ 「解除禁言」→ op=del")

    await ev("禁言列表")
    await ev("入群申请")
    assert "申请人甲" in (await ev("入群申请"))[0], "入群申请列表没解析出来"
    await ev("入群同意 1")
    assert gapi["approvals"][-1]["op"] == "approve", gapi["approvals"]
    assert gapi["approvals"][-1]["req"] == "jr-1", gapi["approvals"]
    await ev("入群拒绝 2 广告号 拉黑")
    last = gapi["approvals"][-1]
    assert last["op"] == "decline" and last["reason"] == "广告号" and last["black"] is True, last
    print("✅ 入群审批：同意回传 join_request_id；拒绝带理由 + 拉黑")

    out = await ev("自动审批")
    assert out and "st_test01" in out[0], out
    out = await ev("自动审批 执行 1")
    assert out and "确认" in out[0] and not gapi["exec"], (out, gapi["exec"])
    await ev("确认")
    assert gapi["exec"] == ["st_test01"], gapi["exec"]
    print("✅ 「自动审批 执行」走二次确认，确认后才调用执行接口")

    await ev("移除成员 U11 拉黑")
    assert gapi["removes"] and gapi["removes"][-1]["black"] is True, gapi["removes"]
    print("✅ 「移除成员」批量移除 + 可选拉黑")

    # 机器人不是群管理员时：当场拒绝并给指引，绝不发无效请求
    # （线上真实场景：平台回 11703「机器人不是群管理员」，用户只看到「操作失败」）
    from handlers import group_cmds
    state["role"] = "member"
    group_cmds.reset_caches(bot)
    before_mute = len(gapi["mutes"])
    out = await ev("禁言 U11 10分")
    assert out and "不是本群管理员" in out[0], out
    assert len(gapi["mutes"]) == before_mute, "非管理员时不该调用禁言接口"
    out = await ev("群信息")
    assert out and "普通成员" in out[0], out
    out = await ev("撤回")
    assert out and "已撤回" in out[0], "撤自己发的消息不受管理员身份限制"
    print("✅ 机器人非管理员：禁言当场拒绝并给设置指引（不发无效请求）；撤回自己的仍可用")
    state["role"] = "admin"
    group_cmds.reset_caches(bot)

    ops = {r["op"] for r in bot.perms.audit_records(50)}
    assert {"group.recall", "group.mute.set", "group.mute.del", "group.join.approve",
            "group.join.decline", "group.join.strategy.exec", "group.member.remove"} <= ops, ops
    print("✅ 撤回/禁言/审批/移除 全部留审计")


def _mkctx(bot, openid):
    class _C:
        uid = bot.identity.uid_of_openid(openid)
        sender = openid
    return _C()


def run():
    cfg = load_config()
    # devtest 用独立数据文件，绝不碰真实 data.json
    cfg["data_file"] = "data.devtest.json"
    bot = Bot(cfg)
    sent: list = []

    async def fake_send(target, *, content=None, **kw):
        sent.append(content)
        return {}

    bot.api.send_group_message = fake_send
    bot.api.send_c2c_message = fake_send
    try:
        asyncio.run(_flow(bot, sent))
        print(f"\n{'=' * 50}\n开发自测完成 ✅（拦截 {len(sent)} 条回复，未发送真实 QQ 消息）")
    except Exception as e:
        print(f"\n开发自测失败 ❌：{e}")
        raise
    finally:
        # 清理临时数据文件（正式 data.json / backups/data-*.json 不受影响）
        for p in (cfg["data_file"], cfg["data_file"] + ".bak"):
            if os.path.exists(p):
                os.remove(p)
        bdir = "backups"
        prefix = os.path.splitext(os.path.basename(cfg["data_file"]))[0] + "-"
        if os.path.isdir(bdir):
            for f in os.listdir(bdir):
                if f.startswith(prefix):
                    os.remove(os.path.join(bdir, f))


def _cleanup(cfg):
    for p in (cfg["data_file"], cfg["data_file"] + ".bak"):
        if os.path.exists(p):
            os.remove(p)
    bdir = "backups"
    prefix = os.path.splitext(os.path.basename(cfg["data_file"]))[0] + "-"
    if os.path.isdir(bdir):
        for f in os.listdir(bdir):
            if f.startswith(prefix):
                os.remove(os.path.join(bdir, f))


def run_group_only():
    """只跑群管理平台接口段：py main.py --grouptest

    这段全是假 API，不联网、不依赖任何外部服务，随时能跑。
    """
    cfg = load_config()
    cfg["data_file"] = "data.grouptest.json"
    bot = Bot(cfg)
    sent: list = []

    async def fake_send(target, *, content=None, **kw):
        sent.append(content)
        return {"id": f"MSG{len(sent)}"}

    bot.api.send_group_message = fake_send
    bot.api.send_c2c_message = fake_send
    owners = bot.perms.owners()
    if owners:
        bot.identity.bind("U1", owners[0])   # 让 U1 直接成为主人，省掉绑定流程
        bot.identity.rebuild()
    try:
        asyncio.run(group_flow(bot, sent))
        print(f"\n{'=' * 50}\n群管理接口自测完成 ✅（{len(sent)} 条回复，未发送真实 QQ 消息）")
    except Exception as e:
        print(f"\n群管理接口自测失败 ❌：{e}")
        raise
    finally:
        _cleanup(cfg)
