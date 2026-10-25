"""最近消息记录（纯内存）：撤回功能需要 message_id。

为什么要单独记一份：
- 撤回接口 `DELETE /v2/groups/{group}/messages/{message_id}` 只认 message_id；
- 机器人自己发的消息，id 在发送接口的响应里（`{"id": ...}`）；
- 群里成员的消息，id 在事件体的 `d.id` 里（也就是 ctx.msg_id）。
这两处原本都被丢掉了，命令层拿不到任何可撤的 ID。

为什么不落盘：
平台撤回窗口只有 **2 分钟**（40064004 已超出消息撤回时限），跨进程/跨重启
的记录毫无用处，落盘只会白白写坏 data.json。所以按群做定长环形缓冲，
进程退出即消失。

并发说明：机器人是单事件循环，record/recent 都不 await，天然原子；
不需要锁。
"""
import logging
import time
from collections import OrderedDict

log = logging.getLogger("qq.msglog")

RECALL_WINDOW = 120      # 平台撤回时限（秒），超过必失败
MAX_PER_GROUP = 40       # 每群保留的最近消息条数
MAX_GROUPS = 300         # 最多同时跟踪多少个群（LRU 淘汰）


class MessageLog:
    """group_openid -> [ {id, openid, name, ts, mine, text} ]，新的在后。"""

    def __init__(self, per_group: int = MAX_PER_GROUP, max_groups: int = MAX_GROUPS):
        self.per_group = per_group
        self.max_groups = max_groups
        self._groups: OrderedDict[str, list] = OrderedDict()

    # ---------- 写 ----------

    def record(self, group_openid: str, msg_id: str, *, openid: str = "", name: str = "",
               mine: bool = False, text: str = "") -> None:
        """记一条消息。msg_id 为空直接忽略（部分事件不带 id）。"""
        group_openid = str(group_openid or "")
        msg_id = str(msg_id or "")
        if not group_openid or not msg_id:
            return
        bucket = self._groups.get(group_openid)
        if bucket is None:
            bucket = self._groups[group_openid] = []
        else:
            self._groups.move_to_end(group_openid)
        bucket.append({
            "id": msg_id,
            "openid": str(openid or ""),
            "name": str(name or ""),
            "ts": time.time(),
            "mine": bool(mine),
            "text": str(text or "")[:60],
        })
        if len(bucket) > self.per_group:
            del bucket[:-self.per_group]
        while len(self._groups) > self.max_groups:
            self._groups.popitem(last=False)

    def drop(self, group_openid: str, msg_id: str) -> None:
        """撤成功后移除，避免同一条被重复撤（重复撤会报错，体验差）。"""
        bucket = self._groups.get(str(group_openid or ""))
        if not bucket:
            return
        bucket[:] = [m for m in bucket if m["id"] != str(msg_id or "")]

    # ---------- 读 ----------

    def recent(self, group_openid: str, *, mine=None, openid=None, n: int = 1) -> list:
        """取最近 n 条（按时间正序返回，便于逐条撤）。mine/openid 为过滤条件。"""
        bucket = list(self._groups.get(str(group_openid or ""), ()))
        if mine is not None:
            bucket = [m for m in bucket if m["mine"] is bool(mine)]
        if openid:
            bucket = [m for m in bucket if m["openid"] == str(openid)]
        n = max(1, int(n or 1))
        return bucket[-n:]

    def last(self, group_openid: str, *, mine=None, openid=None):
        hits = self.recent(group_openid, mine=mine, openid=openid, n=1)
        return hits[0] if hits else None

    def count(self, group_openid: str) -> int:
        return len(self._groups.get(str(group_openid or ""), ()))

    # ---------- 时间判定 ----------

    @staticmethod
    def age(entry: dict) -> float:
        return time.time() - float(entry.get("ts") or 0)

    @classmethod
    def recallable(cls, entry: dict) -> bool:
        """是否还在 2 分钟撤回窗口内（留 3 秒余量抵消网络延迟）。"""
        return cls.age(entry) <= RECALL_WINDOW - 3

    @classmethod
    def age_text(cls, entry: dict) -> str:
        sec = max(0, int(cls.age(entry)))
        if sec < 60:
            return f"{sec} 秒前"
        return f"{sec // 60} 分 {sec % 60} 秒前"
