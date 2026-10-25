"""QQ 官方机器人入口。

用法：
    py main.py             运行机器人
    py main.py --selftest  自检（QQ 凭证 + 网关鉴权）
    py main.py --smoke     冒烟测试（连接网关保持 15 秒后退出）
    py main.py --devtest   离线交互自测（模拟群消息跑命令流程，不发真实消息）
    py main.py --grouptest 群管理接口自测（撤回/禁言/入群审批/移除成员，纯离线）
    py main.py --menutest  菜单/指令面板自测（字符限制、数量上限、命令名是否有效）
    py main.py --idtest    身份层自测（v1->v2 迁移 / 全局拉黑 / 防绕过，离线）
"""
import asyncio
import json
import logging
import os
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
os.chdir(BASE)
if BASE not in sys.path:
    sys.path.insert(0, BASE)

# Windows 控制台默认 GBK，强制 UTF-8 防止中文/emoji 乱码
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

from core.api import QQApi
from core.auth import TokenManager
from core.confirm import PendingConfirms
from core.gateway import INTENT_GROUP_AND_C2C, Gateway
from core.identity import Identity
from core.menu import menu_changed, validate_menu, validate_panels
from core.msglog import MessageLog
from core.permissions import ROLE_GROUP, ROLE_SUPER, Permissions
from core.release_watch import ReleaseWatcher
from core.storage import Storage
from handlers import (admin_cmds, basic_cmds, bot_cmds, fun_cmds, group_cmds,
                      kunclient_cmds)
from handlers.kunclient_cmds import KunClientApi
from handlers.router import Router

log = logging.getLogger("qq.main")

DEFAULT_WELCOME = (
    "大家好，我是 KunClient 机器人 🤖\n"
    "我的账号：@我 发送「kun我」\n"
    "全部功能：@我 发送「帮助」"
)


def load_config() -> dict:
    with open("config.json", "r", encoding="utf-8") as f:
        return json.load(f)


class ConfigError(RuntimeError):
    """配置不可用（语法错误 / 缺必填项）。热重载靠它拒绝「半重载」。"""


def try_load_config() -> tuple[dict | None, str | None]:
    """容错读配置，返回 (配置, 错误原因)；成功时错误为 None。

    存在的意义：编辑 config.json 时被读到半截会抛 JSONDecodeError，而 reload()
    是「先换 self.config 再换其余组件」的写法 —— 中途抛出就再也回不去了，
    机器人会停在一个 config 是新的、storage 还是旧的半重载状态。
    所以校验必须**前置且可读**，让失败变成一条正常的用户提示。
    """
    try:
        cfg = load_config()
    except FileNotFoundError:
        return None, "config.json 不存在"
    except json.JSONDecodeError as e:
        return None, ("config.json 语法错误（第 %d 行第 %d 列：%s），"
                      "多半是编辑器正写到一半" % (e.lineno, e.colno, e.msg))
    except Exception as e:
        return None, "config.json 读取失败：%s" % e
    if not isinstance(cfg, dict):
        return None, "config.json 顶层不是 JSON 对象"
    # 必填项：缺了就别换凭证，否则 API 全部 401，比不重载更糟
    missing = [k for k in ("app_id", "app_secret") if not str(cfg.get(k) or "").strip()]
    if missing:
        return None, "config.json 缺少必填项：" + "、".join(missing)
    return cfg, None


def setup_logging(log_file: str):
    from logging.handlers import RotatingFileHandler
    fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s")
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    sh = logging.StreamHandler(sys.stdout)
    sh.setFormatter(fmt)
    # 单文件 5MB，保留 3 份，避免日志无限增长
    fh = RotatingFileHandler(log_file, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
    fh.setFormatter(fmt)
    root.addHandler(sh)
    root.addHandler(fh)


class Bot:
    def __init__(self, config: dict):
        self.config = config
        self.tokens = TokenManager(config["app_id"], config["app_secret"])
        self.api = QQApi(self.tokens)
        self.storage = Storage(config.get("data_file", "data.json"))
        self.identity = Identity(self.storage)
        # v1 -> v3：openid 主键换算成 QQ 号主键（幂等）+ 授权体系迁到 roles 显式角色表。
        # 有待落盘的迁移则在启动后保存。
        self._pending_migration = bool(self.identity.migrate_legacy())
        # 统一权限体系（RBAC）：角色解析 / 权限节点 / 目标保护 / 审计，见 core/permissions.py
        self.perms = Permissions(self)
        # 敏感操作的二次确认槽位（发布公告 / 强制更新开关等），见 core/confirm.py
        self.confirms = PendingConfirms()
        self.stats = {"start": time.time(), "msg_recv": 0, "cmd": 0,
                      "replies": 0, "errors": 0, "blocked": 0, "denied": 0}
        self.groups: dict = {}  # group_openid -> {count, first, last}，收到群消息时更新
        # 消息 ID 台账（纯内存）：撤回接口只认 message_id，而 id 只在事件/发送响应里出现一次。
        # 平台撤回窗口仅 2 分钟，落盘没有意义 —— 见 core/msglog.py。
        self.msglog = MessageLog()
        # 群内「首次未识别消息」的引导标记（纯内存）：每个群只引导一次，避免闲聊刷屏。
        # 放内存不放 data.json —— 重启后重来一次无所谓，也不动线上数据文件。
        self._guided: set = set()
        _kc = config.get("kunclient", {})
        self.kun = KunClientApi(_kc.get("base_url", "https://kunawa.cn"),
                                _kc.get("admin_username", ""), _kc.get("admin_password", ""))
        # 客户端发版自动播报：轮询服务端版本号，变化即主动把更新内容推到登记的目标群
        self.release = ReleaseWatcher(self)
        self.router = Router(self)
        basic_cmds.register(self, self.router)
        admin_cmds.register(self, self.router)
        bot_cmds.register(self, self.router)
        kunclient_cmds.register(self, self.router)
        fun_cmds.register(self, self.router)
        # 群管理平台接口命令（撤回/入群审批/禁言/移除成员/群信息）
        group_cmds.register(self, self.router)
        self.gateway = Gateway(self.api, self.tokens, INTENT_GROUP_AND_C2C, self.router.handle_event)

    # ---------- 权限 ----------
    #
    # 权威实现在 core/permissions.py（Permissions）：角色分层 owner/super/group_admin/user，
    # 每条命令在 Router 注册时声明 perm 节点并中心化判定。这里只留两个兼容封装，
    # 供测试与少量旧调用使用；新代码一律用 bot.perms.allows(ctx, node)。

    def is_super(self, ctx) -> bool:
        return self.perms.resolve(ctx)[1] >= ROLE_SUPER[1]

    def is_admin(self, ctx) -> bool:
        return self.perms.resolve(ctx)[1] >= ROLE_GROUP[1]

    def guard_target(self, ctx, target_uid: str, action: str = "该操作"):
        """针对人的管理操作保护：不能对主人 / 权限>=自己的成员执行。返回 None 放行。"""
        return self.perms.target_guard(ctx, target_uid, action)

    def is_blacklisted(self, ctx) -> bool:
        # ① 全局拦截：QQ 号封禁 ∪ openid 兜底封禁（identity_ban），所有群 + 私聊一律生效
        if self.identity.is_globally_blocked(ctx.sender):
            return True
        # ② 本群黑名单（uid 键，绑定后由 claim_user 自动迁移）
        return ctx.uid in self.storage.data.get("blacklist", {}).get(ctx.group_openid or "", [])

    def feature_enabled(self, ctx, name: str) -> bool:
        """群功能开关（keywords），默认开启。"""
        if not ctx.is_group or not name:
            return True
        feats = self.storage.data.get("features", {}).get(ctx.group_openid or "", {})
        return feats.get(name, True)

    # ---------- 非命令消息 ----------

    async def on_keyword(self, ctx):
        """未命中命令的消息：群内先查关键词回复，都没有则给一次性引导。"""
        if ctx.is_group:
            if not self.feature_enabled(ctx, "keywords"):
                return
            kws = self.storage.data.get("keywords", {}).get(ctx.group_openid or "", [])
            text = ctx.content
            for kw in kws:
                if text == kw.get("t"):
                    return await self.router.reply(ctx, kw.get("r", ""))
            for kw in kws:
                t = kw.get("t") or ""
                if len(t) >= 2 and t in text:
                    return await self.router.reply(ctx, kw.get("r", ""))
        # 走到这里说明「既不是命令，也不是关键词」。
        # 群里原来直接静默 —— 用户 @ 机器人说句话没反应，会以为坏了。
        # 现在每个群只引导一次，之后仍静默（闲聊不该刷屏）。
        hint = ("你好，我是 KunClient 机器人\n"
                "发「帮助」看能做什么 · 账号相关发「kun我」")
        if ctx.is_group:
            gid = ctx.group_openid or ""
            if gid and gid not in self._guided:
                self._guided.add(gid)
                await self.router.reply(ctx, hint)
            return
        await self.router.reply(ctx, hint)

    async def on_other_event(self, event_type: str, d: dict):
        if event_type == "GROUP_ADD_ROBOT":
            group = d.get("group_openid") or ""
            cfg = self.storage.data.get("welcome", {}).get(group, {})
            if cfg.get("on", True):
                text = cfg.get("text") or DEFAULT_WELCOME
                try:
                    await self.api.send_group_message(group, content=text)
                except Exception as e:
                    log.info("发送进群欢迎失败（大概率未开通主动消息权限，不影响其他功能）: %s", e)
        elif event_type in ("GROUP_DEL_ROBOT", "FRIEND_ADD", "FRIEND_DEL",
                            "GROUP_MSG_REJECT", "C2C_MSG_REJECT", "GROUP_MSG_RECEIVE", "C2C_MSG_RECEIVE"):
            log.info("收到事件 %s: %s", event_type, {k: v for k, v in d.items() if k in ("group_openid", "user_openid", "timestamp")})

    # ---------- 配置热重载 ----------

    async def sync_menu(self) -> dict:
        """把 menu.json 推给平台（启动与重载共用）。失败不影响其他功能。

        三条规矩（都是踩出来的）：
        1. **先校验后推送**：文档的字符/数量限制（顶层 name ≤10 字符≈5 汉字、
           子菜单 ≤5 项 ≤14 字符、items ≤10 项）超了平台只回一个 `40030013`，
           不说哪一项超了 —— 所以本地先用 core/menu.py 查一遍，错在哪直接写日志。
        2. **内容没变就不推**：先 `GET /v2/menu` 深比较（剥掉平台回填的 icon），
           相同就跳过。既省 5 QPM，也不会让 version 每次重启都白涨。
        3. **菜单只服务单聊**：文档写明 `/v2/menu` 仅 C2C 场景全局生效，
           所以群管理类入口一律不放在这里（那是 panels.json 的 group 面板）。

        兼容两种格式：
        - 新版 `{"menu": {"items": [...]}}` → `PUT /v2/menu`（C2C 全局自定义菜单，
          支持 send_message / link / switch / menu 折叠子菜单，最多 10 项）；
        - 旧版 `{"content": {"key": "main", "menus": [...]}}` → `PUT /users/@me/menus`。
        保留旧格式是为了「运维手上还有旧文件」时不至于一条菜单都推不上去。
        """
        if not os.path.exists("menu.json"):
            return {"ok": False, "status": "missing", "errors": ["没有 menu.json"]}
        try:
            with open("menu.json", "r", encoding="utf-8") as f:
                payload = json.load(f)
            payload.pop("_说明", None)
            if isinstance(payload.get("menu"), dict):
                errors = validate_menu(payload["menu"])
                if errors:
                    for e in errors:
                        log.error("menu.json 校验未通过：%s", e)
                    log.warning("菜单未推送（改好 menu.json 后发「菜单同步」重试）")
                    return {"ok": False, "status": "invalid", "errors": errors}
                remote = None
                try:
                    remote = await self.api.get_menu()
                except Exception as e:
                    log.info("读取线上菜单失败（本次按需推送处理）: %s", e)
                if remote and not menu_changed(remote.get("menu"), payload["menu"]):
                    log.info("自定义菜单与线上一致（version=%s），跳过推送", remote.get("version"))
                    return {"ok": True, "status": "unchanged",
                            "version": remote.get("version")}
                resp = await self.api.put_menu(payload["menu"])
                ver = (resp or {}).get("version") if isinstance(resp, dict) else None
                log.info("已同步自定义菜单（menu.json → /v2/menu%s）",
                         f"，version={ver}" if ver is not None else "")
                return {"ok": True, "status": "updated", "version": ver}
            elif isinstance(payload.get("content"), dict):
                await self.api.set_menus(payload)
                log.info("已同步机器人单聊菜单（menu.json → /users/@me/menus）")
                return {"ok": True, "status": "updated", "version": None}
            log.warning("menu.json 格式无法识别：需要 menu 或 content 字段")
            return {"ok": False, "status": "format",
                    "errors": ["menu.json 需要 menu 或 content 字段"]}
        except Exception as e:
            log.warning("菜单同步失败（不影响其他功能）: %s"
                        "（若平台未开放 /v2/menu，可把 menu.json 换回旧格式 "
                        '{"content": {"key": "main", "menus": [...]}}）', e)
            return {"ok": False, "status": "error", "errors": [str(e)]}

    async def sync_panels(self) -> dict:
        """把 panels.json 里的指令面板同步给平台（幂等）。

        ⚠️ `POST /v2/panels` 是**创建**语义：每次启动都 POST 会在平台上堆出一串
        重复面板（最多 20 个），所以这里先按 scope 拉列表，用 panel.remark 当幂等键
        —— 找到就 `PUT /v2/panels/{id}` 覆盖，找不到才创建。
        指令面板支持 group 场景，群里点开就是一组指令（只有管理员能点的项标 only_admin）。
        """
        if not os.path.exists("panels.json"):
            return {"ok": False, "status": "missing", "errors": ["没有 panels.json"]}
        try:
            with open("panels.json", "r", encoding="utf-8") as f:
                cfg = json.load(f)
            specs = cfg.get("panels") or []
            if not isinstance(specs, list) or not specs:
                return {"ok": False, "status": "format", "errors": ["panels 必须是非空数组"]}
            errors = validate_panels(specs, commands=set(self.router.commands))
            if errors:
                for e in errors:
                    log.error("panels.json 校验未通过：%s", e)
                log.warning("指令面板未推送（改好 panels.json 后发「菜单同步」重试）")
                return {"ok": False, "status": "invalid", "errors": errors}
            # 先按场景把已有面板收集成 {(scope, remark): panel_id}
            existing: dict[tuple, str] = {}
            for scope in {s.get("scope") for s in specs if s.get("scope")}:
                try:
                    data = await self.api.list_panels(scope, limit=50)
                except Exception as e:
                    log.info("拉取 %s 场景指令面板失败（本次按创建处理）: %s", scope, e)
                    continue
                for rec in (data or {}).get("records") or []:
                    remark = ((rec.get("panel") or {}).get("remark") or "")
                    if remark:
                        existing[(rec.get("scope"), remark)] = rec.get("panel_id")
            done = 0
            for spec in specs:
                scope = spec.get("scope")
                panel = spec.get("panel") or {}
                if not scope or not panel.get("items"):
                    continue
                pid = existing.get((scope, panel.get("remark") or ""))
                if pid:
                    await self.api.update_panel(pid, panel)
                else:
                    body = {k: v for k, v in spec.items() if not k.startswith("_")}
                    body["panel"] = panel
                    body.setdefault("target_type", "all")
                    await self.api.create_panel(body)
                done += 1
            log.info("指令面板已同步 %d 个（panels.json）", done)
            return {"ok": bool(done), "status": "updated" if done else "empty",
                    "count": done}
        except Exception as e:
            log.warning("指令面板同步失败（不影响其他功能）: %s", e)
            return {"ok": False, "status": "error", "errors": [str(e)]}

    async def reload(self, save_first: bool = True) -> dict:
        """在线重读 config.json + data.json，返回给用户看的变更摘要。

        三条铁律：
        1. **先校验后切换**：配置读不通 / 缺必填项就直接抛 ConfigError，
           此时一个字段都没动过，绝不留下半重载状态。
        2. **storage/identity 原地刷新**，不新建对象 —— 新 Storage 会带来一把
           互不互斥的新锁，旧对象在途的 save() 会用旧内存快照覆盖磁盘（用户数据回滚）。
        3. **tokens/api/gateway 一并更新**：只换 config 的话改 AppID/AppSecret 永远不生效
           （token 有缓存），改域名兜底也复位不了。

        ⚠️ save_first=False：**磁盘优先，不要先刷盘**。
        运维在服务器上手改 data.json 后发「重载配置」时，save_first=True 会把
        内存里的旧快照先写回去，直接把管理员刚改的内容覆盖掉（实测踩过：
        手工登记的 QQ 绑定在 3 秒内被冲掉）。此时以磁盘为准，内存里的
        未落盘改动会被丢弃 —— 这是「重载」本身该有的语义。
        """
        cfg, err = try_load_config()
        if err:
            raise ConfigError(err + "（已保持原配置继续运行）")

        # ── 数据层：落盘在途改动 -> 就地重读（同一把锁，杜绝回写竞态）──
        if save_first:
            await self.storage.save()
        data_changed = await self.storage.reload(cfg.get("data_file", "data.json"))
        self.identity.rebuild()
        self._pending_migration = bool(self.identity.migrate_legacy())
        if self._pending_migration or self.storage.dropped_keys:
            await self.storage.save()
            log.info("重载后已写回数据（迁移或废弃字段清理），文件：%s", self.storage.path)
        self.storage.dropped_keys = []

        # ── 配置层：以上都没抛错，现在整体切换 ──
        self.config = cfg
        cred_changed = self.tokens.update_credentials(cfg["app_id"], cfg["app_secret"])
        self.api.reset_base()
        _kc = cfg.get("kunclient", {})
        self.kun = KunClientApi(_kc.get("base_url", "https://kunawa.cn"),
                                _kc.get("admin_username", ""), _kc.get("admin_password", ""))
        # 换了凭证就得重新鉴权，否则网关还挂着旧 session 等于没换
        if cred_changed:
            try:
                await self.gateway.force_reconnect()
            except Exception as e:
                log.warning("凭证变更后重连网关失败（不影响重载本身）: %s", e)
        # menu.json / panels.json 也是配置，改了顺手推一次（失败不影响运行）
        await self.sync_menu()
        await self.sync_panels()
        log.info("配置已重载（data.json 变化=%s，凭证变化=%s）", data_changed, cred_changed)
        return {"data_changed": data_changed, "cred_changed": cred_changed,
                "data_file": self.storage.path}

    async def run(self):
        await self.api.start()
        if self._pending_migration:
            await self.storage.save()
            log.info("数据已升级为 v3（用户主键 = QQ 号；授权迁入 roles 角色表），已写回 %s",
                     self.storage.path)
            self._pending_migration = False
        if self.storage.dropped_keys:
            await self.storage.save()
            log.info("废弃字段已写回清除：%s", ", ".join(self.storage.dropped_keys))
            self.storage.dropped_keys = []
        log.info("机器人启动完成，等待网关事件…")
        # 启动时同步单聊菜单 + 指令面板（文件存在即推送；失败不影响运行）
        await self.sync_menu()
        await self.sync_panels()
        # 客户端发版自动播报：常驻后台轮询（首次只记录基线版本，不群发）
        self.release.start()
        try:
            await self.gateway.run()
        finally:
            self.release.stop()
            await self.api.close()


def main():
    config = load_config()
    setup_logging(config.get("log_file", "bot.log"))
    log.info("QQ 官方机器人启动（appid=%s）", config["app_id"])
    bot = Bot(config)
    try:
        asyncio.run(bot.run())
    except KeyboardInterrupt:
        log.info("收到退出信号，机器人已停止。")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        from tools import selftest
        selftest.run()
    elif "--smoke" in sys.argv:
        from tools import smoke
        smoke.run()
    elif "--devtest" in sys.argv:
        from tools import devtest
        devtest.run()
    elif "--grouptest" in sys.argv:
        from tools import devtest
        devtest.run_group_only()
    elif "--menutest" in sys.argv:
        from tools import menutest
        menutest.run()
    elif "--idtest" in sys.argv:
        from tools import idtest
        idtest.run()
    else:
        main()
