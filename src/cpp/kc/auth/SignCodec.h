#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  SignCodec —— FDL 签名数据的解析口径（对应 C# 的 KunFlashTool.App.Services.SignCodec）。
//
//  sig / blob 在界面上都是「人复制粘贴来的一段文本」，形态不唯一：
//    · 十六进制：可带空格 / 冒号 / 连字符 / 0x 前缀，也可能被换行折成多行；
//    · Base64：标准字母表，长度通常 ≥ 8。
//
//  🔴 只认这两种。别在这里加「自动猜二进制」「去掉引号」之类的小聪明 ——
//     设备验签失败时唯一的线索就是「字节数对不对」，口径一松就没法排查。
//
//  这里只做纯函数，不碰 UI、不碰文件：FdlAuthDialog（写入前校验）与
//  XiaomiEdlPage（落 sign.bin 前再解析一次）共用同一套，避免两处解析结果不一致。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QString>

namespace kc::auth {

/// 清掉十六进制里的空格 / 冒号 / 连字符 / 制表符 / 换行 / 0x 前缀。
QString cleanHex(QString text);

/// 文本 → 字节（先按十六进制，再按 Base64）；两种都不认识返回空 QByteArray。
QByteArray tryParseBytes(const QString& text);

} // namespace kc::auth
