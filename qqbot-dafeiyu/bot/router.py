"""命令路由：注册表 + 前缀解析 + 权限拦截。

约定：
- 命令名可以是中文，最长匹配优先（「禁言列表」不会被「禁言」抢走）；
- 权限在路由层统一判定，handler 里不用再关心；
- 群里「@机器人 + 命令」也认，方便手机端操作。
"""

from __future__ import annotations

import logging
import re
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Coroutine

from .event import Event, build
from .permission import LEVELS, Actor

log = logging.getLogger("router")

Handler = Callable[["Context"], Coroutine[Any, Any, None]]

# 全角 → 半角，顺带去掉零宽字符
_TRANS = str.maketrans(
    "０１２３４５６７８９ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ　",
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz ",
)


def normalize(text: str) -> str:
    return text.translate(_TRANS).replace("\u200b", "").strip()


@dataclass
class Command:
    name: str
    func: Handler
    perm: str = "member"
    scope: str = "all"          # all / group / private
    need_admin_bot: bool = False
    usage: str = ""
    desc: str = ""
    category: str = "其它"
    aliases: list[str] = field(default_factory=list)
    hidden: bool = False

    @property
    def level(self) -> int:
        return LEVELS.get(self.perm, 100)


class Context:
    """一次命令调用的全部上下文，handler 只依赖它。"""

    def __init__(self, bot, ev: Event, cmd: Command, args: str, actor: Actor):
        self.bot = bot
        self.ev = ev
        self.cmd = cmd
        self.args = args
        self.actor = actor

    # 便捷访问
    @property
    def api(self):
        return self.bot.api

    @property
    def cfg(self):
        return self.bot.cfg

    @property
    def group_id(self) -> int:
        return self.ev.group_id

    @property
    def user_id(self) -> int:
        return self.ev.user_id

    @property
    def is_group(self) -> bool:
        return self.ev.is_group

    # 回复
    async def reply(self, text: str, *, at: bool = True, quote: bool = False) -> int:
        return await self.bot.reply(self.ev, text, at=at, quote=quote)

    async def say(self, text: str) -> int:
        return await self.bot.reply(self.ev, text, at=False, quote=False)

    async def send_image(self, path: str, caption: str = "") -> int:
        return await self.bot.reply(self.ev, caption, image=path)

    def arg_list(self) -> list[str]:
        return [p for p in re.split(r"\s+", self.args.strip()) if p]


class Router:
    def __init__(self):
        self.commands: dict[str, Command] = {}
        self._ordered: list[Command] = []
        self._registered: list[Command] = []  # 注册顺序，菜单按这个排才符合直觉

    # ---- 注册 -------------------------------------------------------
    def command(
        self,
        name: str,
        *,
        aliases: list[str] | None = None,
        perm: str = "member",
        scope: str = "all",
        need_admin_bot: bool = False,
        usage: str = "",
        desc: str = "",
        category: str = "其它",
        hidden: bool = False,
    ):
        def deco(func: Handler) -> Handler:
            cmd = Command(
                name=name,
                func=func,
                perm=perm,
                scope=scope,
                need_admin_bot=need_admin_bot,
                usage=usage,
                desc=desc,
                category=category,
                aliases=list(aliases or []),
                hidden=hidden,
            )
            self.register(cmd)
            return func

        return deco

    def register(self, cmd: Command) -> None:
        for key in [cmd.name, *cmd.aliases]:
            if key in self.commands:
                raise ValueError(f"命令重名：{key}")
            self.commands[key] = cmd
        self._registered.append(cmd)
        # 最长优先，保证「禁言列表」先于「禁言」命中
        self._ordered = sorted(self.commands.values(), key=lambda c: len(c.name), reverse=True)
        seen: set[int] = set()
        uniq: list[Command] = []
        for c in self._ordered:
            if id(c) not in seen:
                seen.add(id(c))
                uniq.append(c)
        self._ordered = uniq

    # ---- 解析 -------------------------------------------------------
    def parse(self, ev: Event, prefixes: list[str]) -> tuple[Command, str] | None:
        raw = normalize(ev.text)
        if not raw:
            return None

        body = raw
        explicit = False  # 是否带了命令前缀（区分「闲聊」和「命令」）

        for p in prefixes:
            if body.startswith(p):
                body = body[len(p):].lstrip()
                explicit = True
                break

        # 群聊里 @机器人 之后的文字也当命令源
        if not explicit and ev.is_group and ev.at_me:
            explicit = True

        # 🔴 没有前缀、也没 @ 的普通聊天，绝不当命令解析。
        # 否则「禁言他」「总结一下」这种日常发言会被当成命令，群里会莫名其妙冒提示。
        # 唯一例外：私聊里「只发一个命令名」（帮助 / 状态 / 绑定主人），方便新手。
        if not explicit:
            if ev.is_private:
                for cmd in self._ordered:
                    if body == cmd.name or body in cmd.aliases:
                        return cmd, ""
            return None

        for cmd in self._ordered:
            for key in (cmd.name, *cmd.aliases):
                if body.startswith(key):
                    rest = body[len(key):]
                    # 命令名后面必须是结尾/空格/中文标点，避免「禁言我」误配
                    if rest and not (rest[0].isspace() or rest[0] in "：:，,"):
                        continue
                    return cmd, rest.strip(" ：:，,")
        return None

    def is_command_like(self, ev: Event, prefixes: list[str]) -> bool:
        return self.parse(ev, prefixes) is not None

    # ---- 帮助 -------------------------------------------------------
    def unique(self) -> list[Command]:
        """去重后的命令列表，**按注册顺序**（菜单按这个排才符合直觉）。"""
        return list(self._registered)

    def help_text(self, level: int, category: str | None = None) -> str:
        groups: dict[str, list[Command]] = {}
        for cmd in self._ordered:
            if cmd.hidden or cmd.level > level:
                continue
            if category and cmd.category != category:
                continue
            groups.setdefault(cmd.category, []).append(cmd)
        if not groups:
            return "没有你能用的命令。"
        order = ["AI 对话", "群管理", "群设置", "娱乐", "系统", "其它"]
        lines: list[str] = []
        for cat in sorted(groups, key=lambda c: (order.index(c) if c in order else 99, c)):
            lines.append(f"【{cat}】")
            for cmd in groups[cat]:
                desc = cmd.desc or cmd.usage
                lines.append(f"· {cmd.usage or cmd.name} —— {desc}")
            lines.append("")
        return "\n".join(lines).strip()

    def categories(self, level: int) -> list[str]:
        seen: list[str] = []
        for cmd in self._ordered:
            if cmd.hidden or cmd.level > level:
                continue
            if cmd.category not in seen:
                seen.append(cmd.category)
        return seen


router = Router()
