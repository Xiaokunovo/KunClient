"""DeepSeek API 客户端（OpenAI 兼容协议）。

只做一件事：给一段 messages，拿回一段文本。附带重试、超时、余额/限流错误的可读化。
"""

from __future__ import annotations

import asyncio
import json
import logging
from dataclasses import dataclass, field
from typing import Any

import aiohttp

log = logging.getLogger("deepseek")

FRIENDLY_ERRORS = {
    400: "请求格式有问题（多半是上下文太长或参数非法）",
    401: "API Key 无效，检查 config.json 里的 deepseek.api_key",
    402: "DeepSeek 账户余额不足，去 platform.deepseek.com 充值",
    422: "参数错误",
    429: "请求太频繁，被限流了，稍后再试",
    500: "DeepSeek 服务器内部错误",
    503: "DeepSeek 服务繁忙，稍后再试",
}


class DeepSeekError(RuntimeError):
    def __init__(self, message: str, status: int | None = None):
        self.status = status
        super().__init__(message)


@dataclass
class Reply:
    content: str
    reasoning: str = ""
    model: str = ""
    prompt_tokens: int = 0
    completion_tokens: int = 0
    cached_tokens: int = 0
    raw: dict = field(default_factory=dict)

    @property
    def total_tokens(self) -> int:
        return self.prompt_tokens + self.completion_tokens


class DeepSeek:
    def __init__(self, cfg):
        self.api_key: str = cfg.get("deepseek.api_key", "")
        self.base_url: str = str(cfg.get("deepseek.base_url", "https://api.deepseek.com")).rstrip("/")
        self.model: str = cfg.get("deepseek.model", "deepseek-chat")
        self.reasoner_model: str = cfg.get("deepseek.reasoner_model", "deepseek-reasoner")
        self.temperature: float = float(cfg.get("deepseek.temperature", 1.1))
        self.max_tokens: int = int(cfg.get("deepseek.max_tokens", 800))
        self.timeout: float = float(cfg.get("deepseek.timeout", 90))
        self.retries: int = int(cfg.get("deepseek.retries", 2))
        self._session: aiohttp.ClientSession | None = None
        self.stats = {"calls": 0, "tokens": 0, "errors": 0}

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=self.timeout),
                headers={
                    "Authorization": f"Bearer {self.api_key}",
                    "Content-Type": "application/json",
                },
            )
        return self._session

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()

    def reload(self, cfg) -> None:
        """热重载配置。注意 Authorization 头是建 session 时写死的，
        key/base_url 变了必须丢掉旧 session，否则新 key 不生效。"""
        old = (self.api_key, self.base_url)
        self.api_key = cfg.get("deepseek.api_key", "")
        self.base_url = str(cfg.get("deepseek.base_url", "https://api.deepseek.com")).rstrip("/")
        self.model = cfg.get("deepseek.model", self.model)
        self.reasoner_model = cfg.get("deepseek.reasoner_model", self.reasoner_model)
        self.temperature = float(cfg.get("deepseek.temperature", self.temperature))
        self.max_tokens = int(cfg.get("deepseek.max_tokens", self.max_tokens))
        self.timeout = float(cfg.get("deepseek.timeout", self.timeout))
        self.retries = int(cfg.get("deepseek.retries", self.retries))
        if (self.api_key, self.base_url) != old and self._session and not self._session.closed:
            asyncio.create_task(self._session.close())
            self._session = None

    @property
    def ready(self) -> bool:
        return bool(self.api_key)

    async def chat(
        self,
        messages: list[dict],
        *,
        model: str | None = None,
        temperature: float | None = None,
        max_tokens: int | None = None,
        json_mode: bool = False,
    ) -> Reply:
        if not self.api_key:
            raise DeepSeekError("还没配 DeepSeek API Key")

        model = model or self.model
        payload: dict[str, Any] = {
            "model": model,
            "messages": messages,
            "max_tokens": int(max_tokens or self.max_tokens),
            "stream": False,
        }
        # 审核判定要结构化输出。不开这个模式模型会回「好的，我判断如下：」然后一堆废话，
        # 解析起来极脆；开了之后它至少保证是个 JSON 对象。
        if json_mode:
            payload["response_format"] = {"type": "json_object"}
        # reasoner 不吃 temperature/top_p，带上会被忽略但也可能报错，索性不带
        if "reasoner" not in model:
            payload["temperature"] = float(temperature if temperature is not None else self.temperature)

        url = f"{self.base_url}/chat/completions"
        last_exc: Exception | None = None

        for attempt in range(self.retries + 1):
            try:
                session = await self._get_session()
                async with session.post(url, json=payload) as resp:
                    body = await resp.text()
                    if resp.status != 200:
                        hint = FRIENDLY_ERRORS.get(resp.status, "未知错误")
                        log.warning("DeepSeek %s：%s", resp.status, body[:300])
                        # 4xx 里只有 429 值得重试
                        if resp.status in (429, 500, 502, 503, 504) and attempt < self.retries:
                            await asyncio.sleep(1.5 * (attempt + 1))
                            continue
                        raise DeepSeekError(f"{hint}（HTTP {resp.status}）", resp.status)
                    data = json.loads(body)
            except asyncio.TimeoutError as exc:
                last_exc = exc
                if attempt < self.retries:
                    await asyncio.sleep(1.5 * (attempt + 1))
                    continue
                raise DeepSeekError(f"DeepSeek 响应超时（>{self.timeout:.0f}s）") from exc
            except aiohttp.ClientError as exc:
                last_exc = exc
                if attempt < self.retries:
                    await asyncio.sleep(1.5 * (attempt + 1))
                    continue
                raise DeepSeekError(f"网络错误：{exc}") from exc

            self.stats["calls"] += 1
            choice = (data.get("choices") or [{}])[0]
            msg = choice.get("message") or {}
            usage = data.get("usage") or {}
            reply = Reply(
                content=str(msg.get("content") or "").strip(),
                reasoning=str(msg.get("reasoning_content") or "").strip(),
                model=str(data.get("model") or model),
                prompt_tokens=int(usage.get("prompt_tokens") or 0),
                completion_tokens=int(usage.get("completion_tokens") or 0),
                cached_tokens=int((usage.get("prompt_cache_hit_tokens") or 0)),
                raw=data,
            )
            self.stats["tokens"] += reply.total_tokens
            return reply

        raise DeepSeekError(f"重试 {self.retries} 次仍失败：{last_exc}")
