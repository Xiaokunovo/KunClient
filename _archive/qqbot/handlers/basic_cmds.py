"""基础命令：帮助（分层美化版）/ 关于 / 我的ID / 角色。"""

VERSION = "v3.1.0"

# 帮助分两层：SHORT 只回答「发什么」，完整命令表在「帮助 更多」。
# 🔴 纯文本消息**不要**用 ─ 画分隔线、也不要用全角空格对齐：
#    QQ 手机端不是等宽字体，分隔线会断成锯齿、空格会撑出一大段空白，很难看。
#    只用「标签：内容」+「· 」当项目符号，任何客户端都整齐。
HELP_SHORT = """我的：签到 · 积分 · 积分排行 · kun绑定邮箱 · kun我
群管：群成员 · 禁言 · 撤回 · 入群申请 · 拉黑
超管：bot状态 · 日志 · 封号 · 菜单同步"""

HELP_ABOUT = """全部命令：帮助 更多
群里使用记得先 @我"""


def _help_all() -> str:
    return """📋 全部命令

我的
· 签到 · 积分 · 积分排行 · 积分兑换
· kun绑定邮箱 · kun我 · kun我的卡密 · kun兑换 · kun反馈

群管（管理员）
· 群成员 · 撤回 · 禁言 · 解除禁言 · 禁言列表 · 移除成员
· 入群申请 · 入群同意 · 入群拒绝 · 自动审批
· 关键词 · 拉黑 · 设置管理 · 公告 · 欢迎语 · 群信息

超管
· bot状态 · bot统计 · 群列表 · 日志 · 重连网关 · 重载配置
· 封号 · 解封 · 设QQ号 · 绑定表 · 审计日志 · 菜单同步

详解：帮助 管理 / 帮助 kun"""


def _help_admin() -> str:
    return """🛠 群管 · 详解

【身份】
主人 > 超管 > 群管 > 普通用户（发「角色」看自己的）
· 授权显式记录、可审计；不能对主人或同级以上的人操作

【拉黑 / 封号】
· 拉黑 昵称 —— 只封本群
· 封号 昵称 —— 跨所有群 + 私聊（需超管）
· 取消拉黑 ／ 解封 ／ 拉黑列表 ／ 封号列表
· 设QQ号 <QQ号> 昵称 ／ 绑定QQ ／ 解绑QQ ／ 绑定表
  对方没绑过 QQ 就先做 openid 兜底拦截，登记后升级为按号永久封

【群管理】（需机器人在群内是管理员）
· 群成员 —— 名册，编号用于下面几条（只有 @ 过我的人才在册）
· 撤回 ／ 撤回 3 ／ 撤回 #3 —— 2 分钟窗口，超时撤不掉
· 禁言 3 10分 ／ 解除禁言 3 ／ 禁言列表
  时长 30秒 / 10分 / 1小时 / 2天 / 永久（=30 天上限），只能对普通成员
· 移除成员 甲 乙 [拉黑] ／ 群信息
  ⚠️ 群消息事件没有 mentions，@ 不出目标 → 用「群成员」看编号

【入群审批】
· 入群申请 ／ 入群同意 1 2 3 ／ 入群拒绝 1 理由 [拉黑]
· 自动审批 ／ 自动审批 执行 1 —— 全量扫描约 10 分钟，需回「确认」

【其他】
· 关键词 添加 词 = 回复 ／ 关键词开关 ／ 公告 设置 文本 ／ 欢迎语
· 设置管理 ／ 移除管理 ／ 管理列表 ／ 超管添加 ／ 超管移除 ／ 审计日志
· 重载配置 ／ 重连网关 ／ bot状态 ／ bot统计 ／ 群列表 ／ 日志

⚠️ 群管理接口都要求机器人在群内是管理员；「移除成员 / 群成员列表」是
   平台内邀能力，没开白名单会提示「应用无接口访问权限」（11253）。"""


def _help_kun() -> str:
    return """🖥 KunClient · 详解

【所有人 · 绑定 / 注册 / 找回】
· kun绑定邮箱 邮箱 → kun验证 验证码 —— 免密码绑定（推荐）
· kun注册 邮箱 用户名 密码 → kun注册验证 验证码 —— 注册新账号
· kun找回 邮箱 → kun找回验证 验证码 新密码 —— 找回密码
· kun绑定 用户名 密码 —— 备选方式（群里发会泄露密码）
· kun解绑 —— 解除绑定
· kun我 —— 自己的账号与会员状态
· kun我的卡密 —— 名下卡密列表
· kun兑换 卡密 —— 卡密换会员时长
· kun反馈 标题 内容 —— 提交问题反馈
· kun公告 ／ kun用量 —— 服务器公告 / 使用记录
· kun版本 —— 客户端最新版本（无需绑定）

【超级管理员（超级管理员及以上）】
· kun状态 ／ kun在线 —— 服务器统计 / 在线列表
· kun用户 名字 —— 查用户详情
· kun踢 名字 —— 云端强制下线
· kunvip 名字 30 —— 加 30 天（负数扣减）
· kunvip 名字 永久 ／ kunvip 名字 取消
· kun卡密 5 30 svip —— 发卡密（天数 0=登录卡密）
· kun重置密码 名字 新密码
· kun公告设置 文本 —— 改客户端公告（空=清空）※需二次确认
· kun更新开关 开 ／ 关 —— 强制更新开关 ※需二次确认
· kun发版播报 —— 发版自动播报：状态 / 开|关 / 发送 / 本群|全部|清空
· kun反馈列表 5 —— 看最近用户反馈

※ 改公告、切强制更新、手动发版播报属「发布类」操作：命令发出后需回「确认」才生效
※ 机器人检测到客户端版本号变化时会自动把更新内容播报到登记的目标群

※ 数据来自 KunClient 登录服务器（实时）
※ 绑定凭据只存机器人本地，解绑即清除"""


def register(bot, router):
    async def _show_help(ctx, topic: str):
        name = bot.gateway.bot_user.get("username") or "KunClient Bot"
        if topic == "管理":
            await router.reply(ctx, _help_admin())
        elif topic == "kun":
            await router.reply(ctx, _help_kun())
        elif topic in ("更多", "全部", "all", "more", "list"):
            await router.reply(ctx, _help_all())
        else:
            notice = ""
            if ctx.is_group:
                n = bot.storage.data.get("notice", {}).get(ctx.group_openid or "", {})
                if n.get("text"):
                    notice = f"📢 公告：{n['text']}\n\n"
            await router.reply(
                ctx, f"{notice}📖 {name} · 常用命令\n\n{HELP_SHORT}\n\n{HELP_ABOUT}")

    @router.command("帮助", aliases=("菜单", "help", "menu", "功能", "命令"),
                    help="查看命令（帮助 更多 / 帮助 管理 / 帮助 kun）")
    async def help_cmd(ctx, args):
        await _show_help(ctx, args.strip().lower())

    # 直觉别名：把「kun帮助」这类连写词也指向对应详解页
    @router.command("kun帮助", aliases=("kunclient帮助", "服务器帮助", "kun"), help="KunClient 管理详解")
    async def kun_help_cmd(ctx, args):
        await _show_help(ctx, "kun")

    @router.command("bot帮助", aliases=("机器人帮助",), help="机器人管理详解（超管）")
    async def bot_help_cmd(ctx, args):
        await _show_help(ctx, "管理")

    @router.command("管理帮助", aliases=("群管帮助",), help="群管理详解")
    async def admin_help_cmd(ctx, args):
        await _show_help(ctx, "管理")

    @router.command("关于", aliases=("about", "version"), help="机器人版本信息")
    async def about_cmd(ctx, args):
        name = bot.gateway.bot_user.get("username") or "KunClient Bot"
        await router.reply(
            ctx,
            f"🤖 {name}\n"
            f"版本：{VERSION}\n"
            "━━━━━━━━━━━━\n"
            "功能：KunClient 账号服务 + 群管理\n"
            "身份：以真实 QQ 号为主键（绑定QQ 后跨群通用）\n"
            "权限：主人/超级管理员/群管理员/普通用户 四级角色，授权显式可审计\n"
            "黑名单：全局按 QQ 号 + openid 兜底，跨群跨私聊生效\n"
            "架构：aiohttp 直连官方 WebSocket 网关",
        )

    # ---------- 敏感操作二次确认 ----------
    # 「发布类」命令（改公告 / 切强制更新 / 手动发版播报）不会立刻生效：先把动作
    # 登记进 bot.confirms（core/confirm.py），用户回「确认」才真正执行，「取消」丢弃。
    # 这样一次手滑不会把全体客户端的行为改掉。

    @router.command("确认", aliases=("确定", "执行"), help="确认上一条待确认的敏感操作")
    async def confirm_cmd(ctx, args):
        slot = bot.confirms.take(ctx.session_key)
        if not slot:
            # 没有待确认操作：静默。日常聊天里的「确认」不该变成刷屏提示。
            return
        if slot.get("uid") and slot["uid"] != ctx.uid:
            return await router.reply(ctx, "⚠️ 该操作由其他人发起，请发起者本人确认。")
        perm = slot.get("perm") or ""
        if perm and not bot.perms.allows(ctx, perm):
            return await router.reply(ctx, bot.perms.deny_text(ctx, perm))
        await router.reply(ctx, f"▶️ 执行：{slot['summary']}")
        await slot["run"](ctx)

    @router.command("取消", aliases=("算了", "放弃"), help="放弃上一条待确认的敏感操作")
    async def cancel_cmd(ctx, args):
        slot = bot.confirms.take(ctx.session_key)
        if not slot:
            return
        await router.reply(ctx, f"🚫 已取消：{slot['summary']}")

    @router.command("我的ID", aliases=("id", "我的id"), help="查看自己的 QQ 号与内部 ID")
    async def my_id_cmd(ctx, args):
        qq = bot.identity.qq_of(ctx.sender)
        if qq:
            head = f"你的 QQ 号：{qq}"
        else:
            head = ("还没绑定 QQ 号（不影响使用）。想绑：绑定QQ <你的QQ号>")
        lines = [head, "", f"内部 ID：{ctx.sender}", f"角色：{ctx.role_label}（发「角色」看权限）"]
        lines.append("管理员权限：让主人发「超管添加 @你」，或编辑 config.json 的 admins。")
        await router.reply(ctx, "\n".join(lines))
