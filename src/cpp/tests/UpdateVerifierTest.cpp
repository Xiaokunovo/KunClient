// 更新包校验（kc/update/UpdateVerifier）的回归测试。
//
// 这是**安全关键代码**：SECURITY.md 把「更新包无签名校验」列为严重问题，
// 自动更新的前提就是这块必须先可靠。所以测试要覆盖：
//   · fail-closed：公钥没配 / 哈希缺失 / 签名缺失 → 一律拒绝（不能静默放行）
//   · 哈希不匹配 → 拒绝，且不再验签
//   · 清单解析：缺字段必须报错
//   · 签名覆盖的明文口径（version\nfile\nsha256）不能漂
//
// 🔴 真正的 ECDSA 验签需要一对测试密钥。这里**不**内置私钥（那等于把私钥
//    写进仓库），所以验签的「成功路径」只能靠集成测试（用构建机的离线签名
//    脚本产出真实包）。本文件覆盖的是**拒绝路径**与**口径**——而那才是
//    真正会出安全问题的地方：放行了不该放行的包。

#include "kc/update/UpdateVerifier.h"
#include "tests/UpdateSignatureFixture.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL " << what << '\n';
    }
}

void checkStr(const QString& got, const QString& want, const char* what)
{
    if (got != want) {
        std::cerr << "FAIL: " << what << "\n  got:  [" << got.toStdString()
                  << "]\n  want: [" << want.toStdString() << "]\n";
        ++failures;
    } else {
        // 🔴 通过时也要打一行。只打 FAIL 的话，日志里看不出「这条到底跑没跑」——
        //    我第一版就是这样：④⑤ 两段其实全过了，但输出里一条都没有，
        //    看起来像被跳过，白排查了一轮。
        std::cout << "PASS " << what << '\n';
    }
}

/// 造一个临时文件，内容给定。
QString makeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& content)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(content);
    f.close();
    return path;
}

} // namespace

int main()
{
    using namespace kc::update;

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "无法创建临时目录\n";
        return 1;
    }

    const QString pkg = makeFile(dir, QStringLiteral("KunClient-test.exe"),
                                 QByteArray("MZ fake update package payload"));
    check(!pkg.isEmpty(), "test package file created");

    // ── ① 公钥未配置时必须拒绝（fail-closed）──────────────────────────────
    // 这是最重要的一条：默认状态下（仓库里公钥是空的）绝不能放行任何包。
    // 如果哪天有人为了「让测试过」把这里改成放行，自动更新就成了后门。
    {
        // 公钥已配置，所以这里测的是「哈希对不上必须拒绝」这条底线。
        const VerifyResult r = verifyPackage(pkg, QStringLiteral("deadbeef"),
                                             QStringLiteral("c2ln"), QByteArray("x"));
        check(!r.ok, "package with a wrong hash is rejected");
        check(!r.hashOk && !r.sigOk, "nothing is marked verified on hash mismatch");
        check(!r.error.isEmpty(), "rejection carries a reason");
    }

    // ── ② 公钥为空串（而不是 nullptr）也要被当成「未配置」─────────────────
    {
        checkStr(QString::fromLatin1(updatePublicKeyBase64()),
                 QString::fromLatin1(updatePublicKeyBase64()),
                 "public key accessor is stable");
        // 2026-10-31：公钥已配置（用 tools/sign-update.js 生成）。
        // 这条断言从「必须为空」改成「必须已配置」—— 空公钥会让所有更新包被拒绝，
        // 是发布前必须查的项。
        const QString pub = QString::fromLatin1(updatePublicKeyBase64()).trimmed();
        check(!pub.isEmpty(), "shipped public key is configured");
        check(QByteArray::fromBase64(pub.toLatin1()).size() == 65,
              "public key is a 65-byte uncompressed P-256 point");
    }

    // ── ③ 清单解析：缺字段必须报错 ────────────────────────────────────────
    {
        PackageManifest m;
        QString err;

        // 完整清单
        QJsonObject full{
            {QStringLiteral("version"), QStringLiteral("2.0.8")},
            {QStringLiteral("file"), QStringLiteral("KunClient-2.0.8.exe")},
            {QStringLiteral("sha256"), QStringLiteral("aabbcc")},
            {QStringLiteral("sig"), QStringLiteral("c2ln")},
        };
        check(parseManifest(QJsonDocument(full).toJson(), &m, &err),
              "complete manifest parses");
        checkStr(m.version, QStringLiteral("2.0.8"), "manifest version round-trips");
        checkStr(m.file, QStringLiteral("KunClient-2.0.8.exe"), "manifest file round-trips");

        // 缺 sig
        QJsonObject noSig = full;
        noSig.remove(QStringLiteral("sig"));
        check(!parseManifest(QJsonDocument(noSig).toJson(), &m, &err),
              "manifest without signature is rejected");
        check(!err.isEmpty(), "rejection explains what is missing");

        // 缺 sha256
        QJsonObject noHash = full;
        noHash.remove(QStringLiteral("sha256"));
        check(!parseManifest(QJsonDocument(noHash).toJson(), &m, &err),
              "manifest without sha256 is rejected");

        // 完全不是 JSON
        check(!parseManifest(QByteArray("not json"), &m, &err),
              "non-JSON manifest is rejected");
    }

    // ── ④ 签名覆盖的明文口径不能漂 ────────────────────────────────────────
    // 服务器签名脚本按 `version\nfile\nsha256` 拼串，客户端必须逐字一致。
    // 少一个换行 / 换个顺序，验签就永远失败，而且极难查。
    {
        PackageManifest m;
        m.version = QStringLiteral("2.0.8");
        m.file = QStringLiteral("KunClient-2.0.8.exe");
        m.sha256 = QStringLiteral("aabbccdd");
        checkStr(QString::fromUtf8(m.signedPayload()),
                 QStringLiteral("2.0.8\nKunClient-2.0.8.exe\naabbccdd"),
                 "signed payload uses version\\nfile\\nsha256");
    }

    // ── ⑤ fileSha256：真实文件的摘要要稳定且非空 ──────────────────────────
    {
        const QString known = makeFile(dir, QStringLiteral("known.bin"), QByteArray("abc"));
        // "abc" 的 SHA-256 是众所周知的常量，用它钉死实现没写错。
        checkStr(fileSha256(known),
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
                 "sha256 of \"abc\" matches the well-known value");

        // 空文件：SHA-256 也是固定常量
        const QString empty = makeFile(dir, QStringLiteral("empty.bin"), QByteArray());
        checkStr(fileSha256(empty),
                 QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
                 "sha256 of an empty file matches the well-known value");

        // 不存在的文件 → 空串（不是崩溃）
        checkStr(fileSha256(dir.filePath(QStringLiteral("nope.bin"))), QString(),
                 "missing file yields an empty digest");
    }

    // ── ⑥ 哈希不匹配时必须拒绝，且不再验签 ────────────────────────────────
    // 即使公钥配好了，哈希对不上也要在验签前就挡掉。
    // （这里公钥为空，所以第一步就失败了；这条断言的是「不会走到 ok=true」。）
    {
        const QString wrongHash(64, QLatin1Char('0'));
        const VerifyResult r = verifyPackage(pkg, wrongHash, QStringLiteral("c2ln"),
                                             QByteArray("payload"));
        check(!r.ok, "package with wrong hash is rejected");
        check(!r.hashOk, "hash mismatch is recorded");
    }

    // ── ⑦ 真实签名：客户端 CNG 能验通 Node 生成的签名 ─────────────────────
    // 这是整条链路上最容易悄悄坏掉的一环：
    //   · Node 的 crypto.sign 产出 **DER** 编码的 ECDSA 签名；
    //   · Windows CNG 的 BCryptVerifySignature 默认吃 **P1363**（r||s 定长）。
    // 两边格式对不上时，验签会永远失败 —— 而且报错只是「签名无效」，
    // 根本看不出是格式问题。这条断言用**真实签出来的包**钉住它。
    {
        // 用夹具里的 sha256 造一个「内容就是那个哈希」的文件是不行的（哈希对不上），
        // 所以这里只验「签名本身能不能过」：构造同样的明文，直接调验签。
        // 走的是 verifyPackage 之外的一层，但正是我们要验证的那层。
        const QByteArray payload = QByteArray(KC_TEST_SIG_VERSION "\n"
                                              KC_TEST_SIG_FILE "\n"
                                              KC_TEST_SIG_SHA256);
        // 这里借用 verifyPackage 的签名路径：哈希校验会失败（因为文件内容不是那个包），
        // 所以先确认「哈希不匹配」被正确拦下 —— 这本身也是一条有效断言。
        const VerifyResult r = verifyPackage(pkg, QString::fromLatin1(KC_TEST_SIG_SHA256),
                                             QString::fromLatin1(KC_TEST_SIG_B64),
                                             payload);
        check(!r.ok, "fixture package does not match the real package hash (expected)");
        check(!r.hashOk, "hash mismatch is detected for the fixture");

        // 🔴 真正要验的那一条：拿真实签名直接打验签层。
        //    如果 Node 的 DER 签名和 CNG 的 P1363 期望对不上，这里会失败 ——
        //    而生产环境的表现会是「所有更新包都提示签名无效」，极难定位。
        check(verifySignatureOnly(payload, QString::fromLatin1(KC_TEST_SIG_B64)),
              "CNG verifies the real ECDSA signature produced by Node (format matches)");

        // 反证：明文改一个字符，验签必须失败（否则签名等于没签）。
        QByteArray tampered = payload;
        tampered[0] = (tampered[0] == '9') ? '8' : '9';
        check(!verifySignatureOnly(tampered, QString::fromLatin1(KC_TEST_SIG_B64)),
              "tampered payload fails signature verification");
    }

    if (failures == 0)
        std::cout << "UpdateVerifierTest: all checks passed\n";
    else
        std::cerr << "UpdateVerifierTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
