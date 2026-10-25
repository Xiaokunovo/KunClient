"""敏感操作的二次确认（内存态，单进程内有效）。

为什么要单独一层：客户端公告、强制更新开关这类命令**一次回车就改变全体用户
看到的客户端行为**，误触/手滑没有回退余地（改公告还好，强制更新会让所有旧版
客户端被拦去升级）。旧实现是「发出去即生效」，没有任何确认与留痕。

设计要点：
- 槽位按 `ctx.session_key` 隔离（群内 = 群+用户，私聊 = 用户），别人替你按「确认」无效；
- 单槽覆盖：同一会话再次下发同类命令会**替换**旧槽（而不是排队），避免误确认陈旧意图；
- 90 秒过期；过期/被取消后按「确认」得到的是「没有待确认的操作」；
- 只存**执行所需的最小信息**（命令名 + 中文摘要 + 回调），摘要用于让用户看清将要做什么。
"""
import logging
import time

log = logging.getLogger("qq.confirm")

CONFIRM_TTL = 90          # 秒
CONFIRM_WORDS = ("确认", "确定", "执行", "yes", "y", "ok")
CANCEL_WORDS = ("取消", "算了", "放弃", "no", "n")


class PendingConfirms:
    """待确认操作槽位表。key = session_key。"""

    def __init__(self):
        self._slots: dict[str, dict] = {}

    def arm(self, key: str, *, command: str, summary: str, run, uid: str,
            perm: str = "") -> dict:
        """登记一个待确认操作（覆盖旧槽）。

        run：`async def run(ctx)` 回调，确认时执行真正的动作。
        perm：所需权限节点；**确认时再校验一次**（发起与确认可能不是同一条消息、
                期间权限可能被撤销）。
        """
        slot = {
            "command": command,
            "summary": summary,
            "run": run,
            "uid": str(uid or ""),
            "perm": perm or "",
            "exp": time.time() + CONFIRM_TTL,
        }
        self._slots[str(key)] = slot
        self._gc()
        return slot

    def peek(self, key: str) -> dict | None:
        slot = self._slots.get(str(key))
        if not slot:
            return None
        if slot["exp"] < time.time():
            self._slots.pop(str(key), None)
            return None
        return slot

    def take(self, key: str) -> dict | None:
        """取走并删除槽位（确认/取消都走这里，保证一次性）。"""
        slot = self.peek(key)
        self._slots.pop(str(key), None)
        return slot

    def _gc(self):
        if len(self._slots) > 2000:
            now = time.time()
            for k in [k for k, v in self._slots.items() if v["exp"] < now]:
                self._slots.pop(k, None)
