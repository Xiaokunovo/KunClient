"""QQ 官方机器人 access_token 管理。

官方文档（2026）：用 AppID + AppSecret 换取 access_token，
请求头使用 `Authorization: QQBot {access_token}`。
旧域名 bots.qq.com 保留为兜底。
"""
import asyncio
import logging
import time

import aiohttp

log = logging.getLogger("qq.auth")

TOKEN_URL = "https://api.bot.qq.com/app/getAppAccessToken"
TOKEN_URL_FALLBACK = "https://bots.qq.com/app/getAppAccessToken"

# 提前 2 分钟刷新，避免边界过期
REFRESH_MARGIN = 120


class AuthError(RuntimeError):
    pass


class TokenManager:
    def __init__(self, app_id: str, app_secret: str):
        self.app_id = str(app_id)
        self.app_secret = str(app_secret)
        self._token: str | None = None
        self._expire_at: float = 0.0
        self._lock = asyncio.Lock()

    def update_credentials(self, app_id: str, app_secret: str) -> bool:
        """热重载时换凭证。返回 True 表示凭证真的变了。

        ⚠️ 必须**作废**已缓存的 token：_token/_expire_at 只在「凭证未变」时才有意义，
        AppSecret 改了还拿旧 token 调 API 会一路 401 然后无限刷新重试。
        这里保留同一个实例（而不是 new 一个），因为 QQApi / Gateway 都持有它的引用。
        """
        changed = str(app_id) != self.app_id or str(app_secret) != self.app_secret
        self.app_id = str(app_id)
        self.app_secret = str(app_secret)
        if changed:
            self._token = None
            self._expire_at = 0.0
            log.info("AppID/AppSecret 已更新，已作废缓存的 access_token")
        return changed

    async def _fetch(self) -> tuple[str, int]:
        body = {"appId": self.app_id, "clientSecret": self.app_secret}
        last_err = "未知错误"
        async with aiohttp.ClientSession() as session:
            for url in (TOKEN_URL, TOKEN_URL_FALLBACK):
                try:
                    async with session.post(url, json=body, timeout=aiohttp.ClientTimeout(total=15)) as resp:
                        data = await resp.json(content_type=None)
                except Exception as e:
                    last_err = f"{url} 请求失败: {e}"
                    continue
                token = data.get("access_token") if isinstance(data, dict) else None
                if token:
                    try:
                        expires_in = int(data.get("expires_in", 7200))
                    except (TypeError, ValueError):
                        expires_in = 7200
                    return token, expires_in
                last_err = f"{url} 返回异常: {data}"
        raise AuthError(f"获取 access_token 失败: {last_err}")

    async def get_token(self, force: bool = False) -> str:
        async with self._lock:
            if not force and self._token and time.time() < self._expire_at:
                return self._token
            token, expires_in = await self._fetch()
            self._token = token
            self._expire_at = time.time() + max(expires_in - REFRESH_MARGIN, 60)
            log.info("已获取 access_token（有效期 %s 秒）", expires_in)
            return token
