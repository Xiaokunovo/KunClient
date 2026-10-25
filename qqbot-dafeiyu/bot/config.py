"""配置加载：config.json + 默认值深合并 + 环境变量覆盖。

设计要点：
- 缺字段一律回落到 DEFAULTS，老配置升级不会崩；
- 环境变量优先级最高（DEEPSEEK_API_KEY / QQ_BOT_TOKEN 等），方便临时改；
- 每次 save() 都是原子写（先写 .tmp 再 replace），断电不会留下半截 JSON。
"""

from __future__ import annotations

import copy
import json
import os
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
CONFIG_PATH = ROOT / "config.json"
EXAMPLE_PATH = ROOT / "config.example.json"
DATA_DIR = ROOT / "data"

DEFAULT_SYSTEM_PROMPT = (
    "你是一个 QQ 群里的聊天助手，名字叫「小小坤ovo」。\n"
    "说话自然、简短，像群里的普通成员，不要用客服腔，不要复述对方的问题。\n"
    "一般 1~3 句；技术问题可以稍长，但别超过 150 字。不知道就说不知道。"
)

# AI 内容哨兵的判定提示词。放这里是为了 WebUI 能直接读出来编辑，
# 空字符串时 guard.py 会回落到这份默认值。
DEFAULT_GUARD_PROMPT = """你是 QQ 群聊的内容审核员。判断下面这条消息是否违规。

等级定义：
0 = 正常（技术讨论、玩梗、吐槽、情绪表达、开玩笑都算正常）
1 = 轻微不当（轻度脏话、擦边玩笑、阴阳怪气）
2 = 明显不当（辱骂他人、发广告引流、刷屏骚扰、引战）
3 = 严重（色情内容、诈骗信息、传播违禁品、持续骚扰）
4 = 极端（组织违法活动、暴恐、涉及未成年人的色情、大规模诈骗、明确的人身威胁或暴力恐吓）

判定原则（很重要，违反这些会导致误封）：
- 宁可判轻，不可判重。不确定时给 0 或 1。
- 技术讨论里出现的词不算违规：「炸弹」在讲内核漏洞、「注入」在讲 SQL、
  「破解」在讲逆向，都是正常技术交流。
- 群友之间互开玩笑、口嗨、玩梗，只要没有明确指向真实伤害，按 0~1 处理。
- 骂人但只是发泄情绪（没具体威胁）给 1~2。
- 「我要弄死你」「等着我收拾你」这种**明确的人身威胁**给 4，不要给 3。
- 只有明确、直接、有实际危害的内容才给 3~4。
- 不要因为消息「不礼貌」就给高分。

只输出一个 JSON 对象，不要任何解释文字：
{"level": 0, "category": "正常", "reason": "简短理由", "confidence": 0.9}

category 只能是：正常、辱骂、广告引流、色情、暴力威胁、涉政、诈骗、违禁品、骚扰、其他
confidence 是你的把握程度，0~1 的小数。"""


DEFAULTS: dict[str, Any] = {
    "bot": {
        "name": "小小坤ovo",
        "self_id": 0,
        "prefixes": ["/", "#"],
        "at_reply": True,          # 群里被 @ 时直接触发 AI 对话
        "reply_with_at": True,     # 群里回复时带上 @
        "command_in_private": True,
        "group_trigger": "smart",  # at=仅@/昵称｜smart=看场合(默认)｜prob=按概率｜all=全部消息
        "nickname_trigger": True,  # at 模式下，消息里提到昵称也触发对话
        "nicknames": ["小小坤ovo", "小小坤", "小坤"],
        "ignore_self": True,
    },
    "webui": {
        "enabled": True,
        "host": "127.0.0.1",       # 只绑本地，公网访问请走 SSH 隧道或 nginx 反代
        "port": 8899,
        "token": "",               # 留空则首次启动自动生成
    },
    "onebot": {
        "ws_url": "ws://127.0.0.1:3001",
        "token": "",
        "reconnect_min": 2,
        "reconnect_max": 30,
        "call_timeout": 15,
    },
    "deepseek": {
        "api_key": "",
        "base_url": "https://api.deepseek.com",
        "model": "deepseek-chat",
        "reasoner_model": "deepseek-reasoner",
        "temperature": 1.1,
        "max_tokens": 800,
        "timeout": 90,
        "retries": 2,
    },
    "chat": {
        "system_prompt": DEFAULT_SYSTEM_PROMPT,
        "context_rounds": 8,        # 每个会话保留的「用户+助手」轮数
        "context_ttl": 1800,        # 上下文闲置多久（秒）后丢弃
        "max_concurrent": 4,        # 同时进行的 DeepSeek 请求数上限
        "group_cooldown": 3,        # 同一个人在同一群的对话冷却（秒）
        "group_reply_probability": 0.35,  # prob 模式的接话概率
        "group_speak_cooldown": 20,  # 机器人自己说完后，这么多秒内不再主动接话（@ 不受限）
        "smart_reply_probability": 0.08,  # smart 模式下「不像提问」的消息被接的概率
        "max_input_chars": 1500,    # 单条输入超过则截断
        "show_reasoning": False,    # deepseek-reasoner 是否展示思考过程
        "reasoning_trigger": "深度",  # 消息里出现该词则改用 reasoner
    },
    "permission": {
        "superusers": [],           # 主人 QQ 号，拥有全部权限
        "admins": [],               # 机器人管理员（跨群生效）
        "allow_group_admin": True,  # 群主/群管理可以执行群管命令
    },
    "guard": {
        "enabled": True,
        "min_length": 4,            # 短于这个字数的消息不送审（「嗯」「1」这种）
        "sample_rate": 0.0,         # 随机抽查概率；0 = 只审命中嫌疑特征的
        "exempt_admin": True,       # 主人/机器人管理/群主/群管免审
        "ban_level": 4,             # 判到几级直接封禁（4 = 只有极端内容）
        "mute_level": 3,            # 判到几级禁言
        "mute_seconds": 600,        # AI 判定的禁言时长
        "recall_level": 2,          # 判到几级撤回 + 警告
        "min_confidence": 0.7,      # 模型把握低于这个值只记录不动手
        "max_per_minute": 10,       # 每分钟最多送审几条（防刷屏打爆 API）
        "prompt": DEFAULT_GUARD_PROMPT,
    },
    "group": {
        "welcome": True,
        "welcome_text": "欢迎 {at} 加入「{group}」！我是大肥鱼，有事 @我 就行，咕噜咕噜~",
        "farewell": False,
        "farewell_text": "{name} 溜了，群里又安静了一点点。",
        "auto_recall_words": [],    # 命中即撤回（可留空，用命令维护）
        "recall_tip": True,         # 撤回后是否提示
        "anti_spam": {
            "enabled": True,
            "window": 8,            # 统计窗口（秒）
            "limit": 6,             # 窗口内超过这么多条判定刷屏
            "mute": 60,             # 刷屏禁言时长（秒）
            "exempt_admin": True,
        },
        "warn_mute_threshold": 3,   # 警告累计到几次自动禁言
        "warn_mute_seconds": 600,
    },
    "log": {
        "level": "INFO",
        "keep_days": 7,
        "chat_log": True,           # 是否把对话落盘（data/chat/*.jsonl）
    },
}


def _deep_merge(base: dict, override: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in (override or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


class Config:
    """点号取值 + 自动落盘的轻量配置对象。"""

    def __init__(self, data: dict[str, Any], path: Path = CONFIG_PATH):
        # 统一在构造处合并默认值：直接 new 一个 Config（测试/脚本）也不会缺字段
        self._data = _deep_merge(DEFAULTS, data or {})
        self.path = path

    # ---- 读写 -------------------------------------------------------
    @classmethod
    def load(cls, path: Path | None = None) -> "Config":
        path = Path(path or CONFIG_PATH)
        raw: dict[str, Any] = {}
        if path.exists():
            try:
                raw = json.loads(path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"[config] {path.name} 不是合法 JSON：{exc}") from exc
        data = _deep_merge(DEFAULTS, raw)
        cfg = cls(data, path)
        cfg._apply_env()
        return cfg

    def _apply_env(self) -> None:
        env_map = {
            "DEEPSEEK_API_KEY": ("deepseek", "api_key"),
            "DEEPSEEK_MODEL": ("deepseek", "model"),
            "DEEPSEEK_BASE_URL": ("deepseek", "base_url"),
            "QQBOT_WS_URL": ("onebot", "ws_url"),
            "QQBOT_TOKEN": ("onebot", "token"),
        }
        for env, (sec, key) in env_map.items():
            val = os.environ.get(env)
            if val:
                self._data.setdefault(sec, {})[key] = val

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=str(self.path.parent), suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump(self._data, fh, ensure_ascii=False, indent=2)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, self.path)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)

    # ---- 取值 -------------------------------------------------------
    def get(self, dotted: str, default: Any = None) -> Any:
        cur: Any = self._data
        for part in dotted.split("."):
            if not isinstance(cur, dict) or part not in cur:
                return default
            cur = cur[part]
        return cur

    def set(self, dotted: str, value: Any, save: bool = False) -> None:
        parts = dotted.split(".")
        cur = self._data
        for part in parts[:-1]:
            cur = cur.setdefault(part, {})
        cur[parts[-1]] = value
        if save:
            self.save()

    def as_dict(self) -> dict[str, Any]:
        return copy.deepcopy(self._data)

    # ---- 校验 -------------------------------------------------------
    def ensure_webui_token(self) -> str:
        """WebUI 没有口令就生成一个并落盘（只生成一次，之后固定）。"""
        token = str(self.get("webui.token") or "")
        if token:
            return token
        import secrets

        token = secrets.token_urlsafe(18)
        self.set("webui.token", token, save=True)
        return token

    def validate(self) -> list[str]:
        problems: list[str] = []
        if not self.get("deepseek.api_key"):
            problems.append("deepseek.api_key 为空 —— AI 对话会不可用（群管命令不受影响）")
        if not str(self.get("onebot.ws_url", "")).startswith(("ws://", "wss://")):
            problems.append("onebot.ws_url 必须是 ws:// 或 wss:// 开头")
        if not self.get("permission.superusers"):
            problems.append("permission.superusers 为空 —— 没人能用主人命令（发「绑定主人」可在私聊里自助绑定）")
        return problems


def ensure_config() -> Config:
    """首次运行时用 config.example.json 生成 config.json。"""
    if not CONFIG_PATH.exists() and EXAMPLE_PATH.exists():
        CONFIG_PATH.write_text(EXAMPLE_PATH.read_text(encoding="utf-8"), encoding="utf-8")
    return Config.load()
