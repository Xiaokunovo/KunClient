#include "kc/update/UpdateService.h"

#include "kc/config/AppStatePaths.h"
#include "kc/net/ApiClient.h"
#include "kc/update/UpdateVerifier.h"
#include "kc/version.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QVersionNumber>

namespace kc::update {

namespace {

/// 版本号比较。用 QVersionNumber 而不是字符串比 —— "2.0.10" > "2.0.9"，
/// 字符串比会得出相反的结论。
bool isNewer(const QString& latest, const QString& current)
{
    const QVersionNumber a = QVersionNumber::fromString(latest);
    const QVersionNumber b = QVersionNumber::fromString(current);
    if (a.isNull() || b.isNull()) return false;
    return a > b;
}

/// 从 JSON 里取字符串字段（不存在返回空）。
QString str(const QJsonObject& o, const char* key)
{
    return o.value(QLatin1String(key)).toString();
}

} // namespace

class UpdateService::Impl {
public:
    net::ApiClient* api = nullptr;
    QString currentVersion = QString::fromLatin1(kc::kVersionString);
};

UpdateService::UpdateService(QObject* parent) : QObject(parent)
{
    d = new Impl;
    d->api = new net::ApiClient(this);
}

UpdateService::~UpdateService()
{
    delete d;
}

void UpdateService::setBaseUrl(const QString& base)
{
    d->api->setBaseUrl(base);
}

void UpdateService::setCurrentVersion(const QString& version)
{
    if (!version.trimmed().isEmpty()) d->currentVersion = version.trimmed();
}

QString UpdateService::updateTempDir()
{
    const QString dir = kc::AppStatePaths::Combine(QStringLiteral("update"));
    QDir().mkpath(dir);
    return dir;
}

void UpdateService::check(std::function<void(const UpdateCheckResult&)> done)
{
    UpdateCheckResult out;
    out.currentVersion = d->currentVersion;

    // 先握手：版本接口虽然对旧客户端公开，但我们既然要做自动更新，
    // 就该带上签名 —— 这样服务端会回 verified=true 并（若已登录）给下载票据。
    d->api->handshake([this, done, out](bool hsOk, const QString& hsErr) mutable {
        // 握手失败**不阻断**检查更新：版本接口本身是公开的，
        // 拿不到签名只是拿不到票据，仍能查版本。
        if (!hsOk) {
            qWarning("[update] 握手失败（仍继续检查版本）：%s", qUtf8Printable(hsErr));
        }

        d->api->get(QStringLiteral("/api/v2/app/version"),
                    [done, out](const net::ApiResult& r) mutable {
            if (!r.ok) {
                out.ok = false;
                out.error = r.error.isEmpty()
                                ? QStringLiteral("无法连接更新服务器")
                                : r.error;
                // 服务端明确说「通道关闭」时把原因带出来
                if (!r.errorCode.isEmpty()) out.closedReason = r.errorCode;
                done(out);
                return;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(r.body);
            if (!doc.isObject()) {
                out.error = QStringLiteral("服务器返回格式异常");
                done(out);
                return;
            }
            const QJsonObject root = doc.object();
            const QJsonObject data = root.value(QStringLiteral("data")).toObject();

            out.latestVersion = str(data, "version");
            out.updateEnabled = data.value(QStringLiteral("updateEnabled")).toBool(true);
            out.closedReason = str(data, "updateClosedReason");
            out.changelog = str(data, "changelog");
            out.downloadUrl = str(data, "downloadUrl");
            out.forceUpdate = data.value(QStringLiteral("forceUpdate")).toBool(false);
            // 校验数据（服务端 2.0.8 起下发；老服务端没有 → 保持空，
            // 之后 downloadAndVerify 会因为缺哈希而拒绝，这是**正确**的 fail-closed）
            out.sha256 = str(data, "sha256");
            out.signatureBase64 = str(data, "sig");

            out.ok = true;
            out.hasUpdate = !out.latestVersion.isEmpty()
                            && isNewer(out.latestVersion, out.currentVersion);
            done(out);
        });
    });
}

void UpdateService::downloadAndVerify(const UpdateCheckResult& info,
                                      ProgressFn progress,
                                      std::function<void(bool, const QString&, const QString&)> done)
{
    if (info.downloadUrl.trimmed().isEmpty()) {
        done(false, QString(), QStringLiteral("服务器未提供下载地址"));
        return;
    }
    if (info.sha256.trimmed().isEmpty() || info.signatureBase64.trimmed().isEmpty()) {
        // 🔴 没有校验数据就**不下载**。这不是「降级」而是「拒绝」：
        //    下载一个无法验证的 exe 并执行，等于把 SECURITY.md 那个严重问题
        //    原样搬回来。宁可让用户手动去官网下。
        done(false, QString(),
             QStringLiteral("服务器未提供校验信息，无法安全地自动更新。\n"
                            "请到官网手动下载新版本。"));
        return;
    }

    const QString dir = updateTempDir();
    const QString fileName = QStringLiteral("KunClient-%1.exe").arg(info.latestVersion);
    const QString target = QDir(dir).filePath(fileName);

    // 先删旧文件：同名残留会让「下载成功」变成「写了一半的旧包」。
    QFile::remove(target);

    d->api->download(info.downloadUrl, target, progress,
                     [target, info, done](const net::ApiResult& r) {
        if (!r.ok) {
            QFile::remove(target);
            done(false, QString(), r.error.isEmpty()
                                        ? QStringLiteral("下载失败")
                                        : r.error);
            return;
        }

        // 组装被签名的明文：version\nfile\nsha256
        // 文件名要与服务器签名时用的一致（服务器签的是远端文件名，不是本地路径）。
        PackageManifest mf;
        mf.version = info.latestVersion;
        mf.file = info.downloadUrl.section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
        mf.sha256 = info.sha256;
        mf.signatureBase64 = info.signatureBase64;

        const VerifyResult vr = verifyPackage(target, info.sha256,
                                              info.signatureBase64,
                                              mf.signedPayload());
        if (!vr.ok) {
            // 校验不过 → 立刻删除，绝不留下一个「可能被执行」的文件。
            QFile::remove(target);
            done(false, QString(), vr.error);
            return;
        }
        done(true, target, QString());
    });
}

bool UpdateService::applyUpdate(const QString& verifiedPackagePath, QString* error)
{
    if (!QFile::exists(verifiedPackagePath)) {
        if (error) *error = QStringLiteral("更新包不存在");
        return false;
    }

    // 当前 exe 路径。注意：替换的是**正在运行的**那个文件。
    const QString currentExe = QCoreApplication::applicationFilePath();
    if (currentExe.isEmpty()) {
        if (error) *error = QStringLiteral("无法定位当前程序路径");
        return false;
    }

    const QString scriptPath = QDir(updateTempDir())
                                   .filePath(QStringLiteral("apply-update.cmd"));

    // 🔴 替换脚本的写法要点：
    //    · 必须先等本进程退出（timeout + 重试），否则文件被占用改不了；
    //    · 用 move 而不是 copy：旧文件要留作回滚底本；
    //    · 每一步都检查 errorlevel，失败就回滚并把窗口留住（让用户看到原因）；
    //    · 最后自删 —— 用 `start /b cmd /c del` 让删除动作脱离本脚本。
    const QString script = QStringLiteral(
        "@echo off\r\n"
        "chcp 65001 >nul\r\n"
        "setlocal\r\n"
        "set \"OLD=%1\"\r\n"
        "set \"NEW=%2\"\r\n"
        "set \"BAK=%1.bak\"\r\n"
        "\r\n"
        "rem 等原进程退出（最多 30 秒）\r\n"
        "set /a tries=0\r\n"
        ":wait\r\n"
        "set /a tries+=1\r\n"
        "if %tries% gtr 60 goto fail\r\n"
        "move /y \"%OLD%\" \"%BAK%\" >nul 2>&1\r\n"
        "if errorlevel 1 (\r\n"
        "  ping -n 1 -w 500 127.0.0.1 >nul\r\n"
        "  goto wait\r\n"
        ")\r\n"
        "\r\n"
        "rem 放新文件\r\n"
        "copy /y \"%NEW%\" \"%OLD%\" >nul 2>&1\r\n"
        "if errorlevel 1 goto rollback\r\n"
        "\r\n"
        "start \"\" \"%OLD%\"\r\n"
        "del \"%NEW%\" >nul 2>&1\r\n"
        "start /b cmd /c \"del \"%~f0\"\" >nul 2>&1\r\n"
        "exit /b 0\r\n"
        "\r\n"
        ":rollback\r\n"
        "move /y \"%BAK%\" \"%OLD%\" >nul 2>&1\r\n"
        ":fail\r\n"
        "echo.\r\n"
        "echo 更新失败，已回滚到原版本。请到官网手动下载新版本。\r\n"
        "pause\r\n"
        "exit /b 1\r\n")
        .arg(currentExe, QDir::toNativeSeparators(verifiedPackagePath));

    QFile f(scriptPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法创建更新脚本（临时目录不可写）");
        return false;
    }
    f.write(script.toUtf8());
    f.close();

    // 脱离启动：不等待、不捕获输出 —— 本进程马上要退出。
    const bool started = QProcess::startDetached(
        QStringLiteral("cmd.exe"), {QStringLiteral("/c"), scriptPath});
    if (!started) {
        if (error) *error = QStringLiteral("无法启动更新脚本");
        return false;
    }
    return true;
}

} // namespace kc::update
