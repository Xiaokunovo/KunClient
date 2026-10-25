"""命令包：import 即注册（装饰器在 import 时跑）。"""

from . import (  # noqa: F401
    admin,
    chat,
    extras,
    fun,
    groupinfo,
    guard_cmd,
    meme,
    moderation,
    purge,
    system,
)

__all__ = [
    "admin", "chat", "extras", "fun", "groupinfo",
    "guard_cmd", "meme", "moderation", "purge", "system",
]
