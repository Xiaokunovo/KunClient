# 用户反馈处理：找不到 liblzma-5.dll

> 处理时间：2026-10-31 ｜ 处理人：Lead
> 用户原话：「由于找不到 liblzma-5.dll，无法继续执行代码。重新安装程序可能解决此问题」

## 一句话结论

**不是程序坏了，是 2.0.6 离线版的压缩包里少打包了一个 DLL。** 补上就好，不用重装系统、不用装运行库。

---

## 用户该怎么修（三种，任选一种）

### ① 只补一个文件（最快，86 KB）

1. 下载：`https://kunawa.cn/assets/KunClient-2.0.6-offline-hotfix.zip`
2. 解压得到 `liblzma-5.dll`
3. 把它放到 `KunClient.exe` **同一个文件夹**里（和 Qt6Core.dll 那些放一起）
4. 重新双击 `KunClient.exe`

### ② 重新下完整包

- `https://kunawa.cn/assets/KunClient-2.0.6-offline.7z`
- 若下到的还是旧包（Cloudflare 边缘缓存 4 小时内可能回旧文件），改用：
  `https://kunawa.cn/assets/KunClient-2.0.6-offline-fixed.7z`（同内容，URL 没被缓存）

### ③ 改用静态单文件版（最省事，推荐）

`https://kunawa.cn/assets/KunClient-2.0.8.exe`（8.1 MB）
这是**单个 exe**，导入表里没有任何第三方 DLL，不存在缺 DLL 的可能。
注意：它是 2.0.8，不是 2.0.6，功能更新（多主题包 / 自动更新 / 刷机前备份等）。

---

## 如果是「功能补充.zip」的用户

那个包缺的是另外两个（`Qt6Network.dll`、`libjpeg-8.dll`）。
不补也能启动，但「检查更新」和「jpg 壁纸」会静默失效。

---

## 根因（给开发看）

| 项 | 内容 |
|---|---|
| 直接原因 | `dist-offline/KunClient.exe` 的导入表里有 `liblzma-5.dll`，但包里没有这个文件 |
| 为什么需要它 | `storage/PayloadParser.cpp` 用 `lzma_stream_buffer_decode()` 解 payload 的 REPLACE_XZ |
| 为什么打包漏了 | 离线包是从 `build-offline/` **手工拷**的，没走 `build-release.sh` 的 ldd 闭包；那一次拷贝漏了它 |
| 为什么没测出来 | 开发机 PATH 里有 `/e/msys64/mingw64/bin`，Windows 加载器直接在那里找到 DLL；只有没装 MSYS2 的用户机器才会报错 |
| 顺带发现的同类 bug | `build-release.sh` 插件闭包那段还在用 `awk '$3 ~ b"/"'` 路径比对（`/mingw64/bin/x` vs `/e/msys64/mingw64/bin`，永远不匹配），导致 `dist/` 一直缺 `Qt6Network.dll` + `libjpeg-8.dll` |

## 修了什么

1. 补齐 3 个 DLL（`dist-offline/liblzma-5.dll`、`dist/Qt6Network.dll`、`dist/libjpeg-8.dll`）
2. 重打 5 个包（旧包备份在 `_stale-*`）
3. 修 `build-release.sh` 插件闭包过滤器 + 补「新入队项继续求到底」循环
4. 新增 `tools/check-package-deps.py`：解析 PE 导入表求闭包，判据是「包内有没有」而不是「本机能不能找到」
   - 接进 `build-release.sh`（dist 不完整直接 fail）
   - 接进 `build-static.sh`（单文件出现第三方 DLL 直接 fail）
5. 线上：替换 `assets/KunClient-2.0.6-offline.7z`，新增 hotfix.zip 与 fixed.7z

## 验证证据

```
$ python tools/check-package-deps.py src/cpp/dist src/cpp/dist-offline \
    src/cpp/KunClient-2.0.6-offline.7z src/cpp/KunClient-2.0.6-offline.zip \
    "KunClient-2.0.6-功能补充.zip"
-- src/cpp/dist            文件数 41（PE 41），第三方 DLL 31 个  [OK]
-- src/cpp/dist-offline    文件数 45（PE 45），第三方 DLL 32 个  [OK]
-- KunClient-2.0.6-offline.7z   45 文件  [OK]
-- KunClient-2.0.6-offline.zip  45 文件  [OK]
-- KunClient-2.0.6-功能补充.zip  41 文件  [OK]
[OK] 全部 5 个包依赖完整

$ python tools/check-package-deps.py src/cpp/dist-static/KunClient-2.0.7.exe \
    src/cpp/dist-static/KunClient-2.0.7-upx.exe \
    src/cpp/dist-static/KunClient-2.0.8.exe \
    src/cpp/dist-static/KunClient-2.0.8-upx.exe
[OK] 全部 4 个包依赖完整（导入表无第三方 DLL）
```

干净 PATH 启动实测（剥掉 mingw64 / ucrt64 / usr/bin）：`dist/`、`dist-offline/`、
三个压缩包解出的目录 **全部正常出图**；修复前是退出码 127（`STATUS_DLL_NOT_FOUND`）。

## 遗留

- **Cloudflare 缓存**：`/assets/*` 有 `public, max-age=14400`（4 小时）。
  服务器文件已换、源站 md5 已核对，但 CF 边缘在 TTL 内仍回旧包
  （实测 `cf-cache-status: HIT`，`content-length` 还是旧的 29,513,747）。
  要么在 CF 控制台 purge，要么等 ≤4 小时；急着用就走 `-fixed.7z` 或 `?v=2`。
- **`tls/` 目录**：纯离线版用不到（exe 不导入 Qt6Network，二进制里也没有
  QNetworkAccessManager），814 KB 死重量。补了 DLL 后不再报错，可以留也可以删。
