"""QQ 官方机器人 OpenAPI 封装（统一域名 api.bot.qq.com，旧域名兜底）。

覆盖本机器人用到的能力：群聊/单聊消息收发与撤回、群成员/禁言/入群审批等群管理、
自定义菜单与指令面板、网关地址获取。
"""
import logging

import aiohttp

from .auth import TokenManager

log = logging.getLogger("qq.api")

API_BASE = "https://api.bot.qq.com"
API_BASE_FALLBACK = "https://api.sgroup.qq.com"


class QQApiError(RuntimeError):
    """带中文提示的 QQ OpenAPI 错误。

    QQ 的接口失败有两种表达：① 标准 HTTP 状态码；② HTTP 200/400 + 业务 code。
    业务 code 才是真正的原因（例如 40064004 撤回超时、11253 接口未开白名单），
    所以 code 优先于状态码翻译。
    """

    HTTP_HINTS = {
        401: "鉴权失败（token 失效或无权限）",
        402: "鉴权失败（token 失效）",
        403: "无权限（机器人可能未开通该能力）",
        404: "接口或资源不存在",
        429: "请求太频繁，请稍后再试",
        500: "QQ 服务器开小差了，稍后再试",
        502: "QQ 服务器开小差了，稍后再试",
        503: "QQ 服务器开小差了，稍后再试",
        504: "QQ 服务器开小差了，稍后再试",
    }

    # 群管理 / 指令面板等接口的业务错误码（来自官方文档错误码表 + 实测）
    CODE_HINTS = {
        40061001: "请求参数无效（请检查参数格式）",
        40062003: "无操作权限（机器人需为群管理员，且只能对普通成员操作）",
        40064004: "已超出消息撤回时限（消息发出超过 2 分钟不可撤回）",
        50065001: "消息撤回失败，请稍后重试",
        # 11703 是实测拿到的（文档没列）：群里机器人还是普通成员时，禁言/查禁言/审批都会报它
        11703: "机器人不是群管理员（请群主在「群设置 → 群成员」里把它设为管理员）",
        11253: "该接口仅白名单机器人可用（应用无接口访问权限）",
        40030006: "指令面板不存在",
        40030008: "链接格式错误（必须以 https:// 开头）",
        40030009: "指令面板操作进行中，请稍后重试",
        40030013: "超出数量限制（请减少一次操作的数量）",
        40030015: "面板元素类型不合法（仅支持 command / link）",
        40030016: "必填字段缺失",
        40030017: "操作类型不合法（仅支持 add / del）",
        40030018: "当前场景不支持此操作",
        40030021: "全局面板不支持添加指定关联对象",
    }

    def __init__(self, status: int, data):
        self.status = status
        self.data = data
        if isinstance(data, dict):
            detail = str(data.get("message") or data.get("msg") or data)
            self.code = data.get("code")
        else:
            detail = str(data)
            self.code = None
        if "msg_id" in detail or "msg seq" in detail.lower():
            text = "回复超时或已达条数上限（每条消息最多回 5 条），请重新 @机器人 发送"
        elif self.code in self.CODE_HINTS:
            text = self.CODE_HINTS[self.code]
        else:
            text = self.HTTP_HINTS.get(status, "请求失败")
        super().__init__(f"{text}（HTTP {status}：{detail[:120]}）")


class QQApi:
    def __init__(self, tokens: TokenManager):
        self.tokens = tokens
        self._session: aiohttp.ClientSession | None = None
        self._base = API_BASE

    async def start(self):
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()

    async def close(self):
        if self._session and not self._session.closed:
            await self._session.close()
        self._session = None

    def reset_base(self):
        """热重载时复位「粘滞」的旧域名。

        _base 一旦在网络异常时切到 API_BASE_FALLBACK（api.py:83-86）就会一直留在那儿，
        连得上就再也切不回来。重载配置等于一次「重新开始」，这里复位回主域名。
        """
        self._base = API_BASE

    @property
    def session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            raise RuntimeError("QQApi 尚未启动（未调用 start）")
        return self._session

    async def request(self, method: str, path: str, *, json_body=None, params=None, _retry_auth=True):
        await self.start()
        token = await self.tokens.get_token()
        headers = {"Authorization": f"QQBot {token}"}
        url = self._base + path
        try:
            async with self.session.request(
                method, url, json=json_body, params=params, headers=headers,
                timeout=aiohttp.ClientTimeout(total=30),
            ) as resp:
                status = resp.status
                try:
                    data = await resp.json(content_type=None)
                except Exception:
                    data = {"raw": (await resp.text())[:300]}
        except aiohttp.ClientError as e:
            if self._base == API_BASE:
                log.warning("请求 %s 网络异常(%s)，切换旧域名重试", path, e)
                self._base = API_BASE_FALLBACK
                return await self.request(method, path, json_body=json_body, params=params, _retry_auth=_retry_auth)
            raise
        if status in (401, 402) and _retry_auth:
            log.warning("凭证失效(HTTP %s)，刷新 token 后重试 %s", status, path)
            await self.tokens.get_token(force=True)
            return await self.request(method, path, json_body=json_body, params=params, _retry_auth=False)
        if status >= 400:
            raise QQApiError(status, data)
        return data

    # ---------- 网关 ----------

    async def gateway_url(self) -> str:
        data = await self.request("GET", "/gateway")
        return data["url"]

    # ---------- 消息 ----------

    async def send_group_message(self, group_openid: str, *, content=None, msg_type=0,
                                 msg_id=None, msg_seq=1, media=None, event_id=None):
        """发群消息。msg_id+msg_seq 为被动回复；不传 msg_id 即主动消息（需平台权限）。"""
        body: dict = {"msg_type": msg_type, "msg_seq": msg_seq}
        if content is not None:
            body["content"] = content
        if media is not None:
            body["media"] = media
        if msg_id:
            body["msg_id"] = msg_id
        if event_id:
            body["event_id"] = event_id
        return await self.request("POST", f"/v2/groups/{group_openid}/messages", json_body=body)

    async def send_c2c_message(self, openid: str, *, content=None, msg_type=0,
                               msg_id=None, msg_seq=1, media=None, event_id=None):
        body: dict = {"msg_type": msg_type, "msg_seq": msg_seq}
        if content is not None:
            body["content"] = content
        if media is not None:
            body["media"] = media
        if msg_id:
            body["msg_id"] = msg_id
        if event_id:
            body["event_id"] = event_id
        return await self.request("POST", f"/v2/users/{openid}/messages", json_body=body)

    # ---------- 群管理 ----------
    #
    # 官方 2026-08-10 / 09-03 两批新增：撤回群消息、入群申请审批、群禁言、群成员管理。
    # 所有群管理接口都要求**机器人在群内是管理员**（否则 40062003），
    # 「群成员批量移除 / 群成员列表」目前还是内邀能力（未开白名单时 11253）。

    async def delete_group_message(self, group_openid: str, message_id: str):
        """撤回群消息。成功返回 HTTP 200 无响应体（这里返回 {}）。

        限制：消息发出超过 2 分钟不可撤回（40064004）；
        机器人是管理员时能撤自己和普通成员的消息，普通成员只能撤自己发的。
        """
        return await self.request(
            "DELETE", f"/v2/groups/{group_openid}/messages/{message_id}")

    async def get_group_info(self, group_openid: str):
        """群基本信息：群名 / 简介 / 分类 / 标签 / 人数。"""
        return await self.request("GET", f"/v2/groups/{group_openid}/info")

    async def get_bot_state(self, group_openid: str):
        """机器人在群内的状态：member_role(member/owner/admin) / 收消息设置 等。

        用来自检「能不能禁言、能不能撤别人的消息」——比等接口报 40062003 友好。
        """
        return await self.request("GET", f"/v2/groups/{group_openid}/bot_state")

    async def list_group_members(self, group_openid: str, cursor: str = "", limit: int = 30):
        """群成员列表（内邀能力，每次最多 30 条）。"""
        params: dict = {"limit": limit}
        if cursor:
            params["cursor"] = cursor
        return await self.request("GET", f"/v2/groups/{group_openid}/members", params=params)

    async def batch_remove_members(self, group_openid: str, member_openids: list,
                                   add_to_member_blacklist: bool = False):
        """群成员批量移除（单次最多 20 个，内邀能力）。

        响应：{remove_members_result: "success", add_to_member_blacklist_fail_openids: []}
        """
        body = {"member_openids": list(member_openids)[:20]}
        if add_to_member_blacklist:
            body["add_to_member_blacklist"] = True
        return await self.request(
            "POST", f"/v2/groups/{group_openid}/batch_remove_members", json_body=body)

    # ---------- 入群申请审批 ----------

    async def list_join_requests(self, group_openid: str, cursor: str = "", limit: int = 20):
        """拉取入群申请列表（30 QPM）。返回 {list: [JoinRequest], next_cursor}。"""
        params: dict = {"limit": max(1, min(int(limit or 20), 50))}
        if cursor:
            params["cursor"] = cursor
        return await self.request(
            "GET", f"/v2/groups/{group_openid}/join_request_list", params=params)

    async def approval_join_request(self, group_openid: str, member_openid: str, op: str, *,
                                    join_request_id: str = "", reject_reason: str = "",
                                    add_to_member_blacklist: bool = False):
        """审批入群申请：op=approve 通过 / decline 拒绝。

        join_request_id 建议回传（列表接口给的申请 ID），否则平台要靠 member_openid 猜。
        """
        body: dict = {"op": op}
        if join_request_id:
            body["join_request_id"] = join_request_id
        if op == "decline":
            if reject_reason:
                body["reject_reason"] = reject_reason
            if add_to_member_blacklist:
                body["add_to_member_blacklist"] = True
        return await self.request(
            "POST", f"/v2/groups/{group_openid}/approval_join_request/{member_openid}",
            json_body=body)

    # ---------- 入群自动审批策略（机器人级，非单群） ----------

    async def list_join_strategies(self, cursor: str = "", limit: int = 20):
        """查询生效中的入群自动审批策略。返回 {strategies: [...], next_cursor}。"""
        params: dict = {"limit": max(1, min(int(limit or 20), 50))}
        if cursor:
            params["cursor"] = cursor
        return await self.request("GET", "/v2/groups/join_approval_strategy", params=params)

    async def create_join_strategy(self, *, group_openids=None, group_ids=None,
                                   is_enable: str = "on", expire_at: str = "", remark: str = ""):
        """创建入群自动审批策略（group_openids 与 group_ids 二选一）。"""
        body: dict = {"is_enable": is_enable}
        if group_openids:
            body["group_openids"] = list(group_openids)[:100]
        if group_ids:
            body["group_ids"] = list(group_ids)[:100]
        if expire_at:
            body["expire_at"] = expire_at
        if remark:
            body["remark"] = remark[:255]
        return await self.request("POST", "/v2/groups/join_approval_strategy", json_body=body)

    async def execute_join_strategy(self, strategy_id: str):
        """执行入群自动审批策略：对关联群全量扫描，白名单号码的申请自动通过。

        异步执行，官方说明约 10 分钟完成；接口本身立即返回。
        """
        return await self.request(
            "POST", f"/v2/groups/join_approval_strategy/{strategy_id}/execute", json_body={})

    # ---------- 群禁言 ----------

    async def get_restrict_chat_setting(self, group_openid: str):
        """查询群禁言状态：全员禁言规则 + 当前被禁言的成员列表。"""
        return await self.request("GET", f"/v2/groups/{group_openid}/restrict_chat_setting")

    async def set_member_mute(self, group_openid: str, members: list):
        """设置群成员禁言（60 QPM，单次最多 20 个）。

        members 每项：{op: add|update|del, member_openid, mute_expire_at(RFC3339)}
        只能对**普通成员**生效（群主/管理员/机器人不行），最长 30 天。
        """
        return await self.request(
            "POST", f"/v2/groups/{group_openid}/restrict_chat_setting",
            json_body={"members": list(members)[:20]})

    # ---------- 自定义菜单（/v2/menu，C2C 全局菜单） ----------

    async def get_menu(self):
        return await self.request("GET", "/v2/menu")

    async def put_menu(self, menu):
        """覆盖式修改全局自定义菜单（5 QPM）。menu = {"items": [...]}。"""
        return await self.request("PUT", "/v2/menu", json_body={"menu": menu})

    # ---------- 指令面板（/v2/panels，单聊 + 群聊） ----------

    async def list_panels(self, scope: str, cursor: str = "", limit: int = 20):
        params: dict = {"scope": scope, "limit": max(1, min(int(limit or 20), 50))}
        if cursor:
            params["cursor"] = cursor
        return await self.request("GET", "/v2/panels", params=params)

    async def create_panel(self, payload: dict):
        return await self.request("POST", "/v2/panels", json_body=payload)

    async def update_panel(self, panel_id: str, panel: dict):
        return await self.request("PUT", f"/v2/panels/{panel_id}", json_body={"panel": panel})

    async def delete_panel(self, panel_id: str):
        return await self.request("DELETE", f"/v2/panels/{panel_id}")

    # ---------- 聊天菜单 ----------

    async def get_menus(self):
        return await self.request("GET", "/users/@me/menus")

    async def set_menus(self, payload):
        return await self.request("PUT", "/users/@me/menus", json_body=payload)
