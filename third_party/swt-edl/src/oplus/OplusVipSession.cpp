#include "oplus/OplusVipSession.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace swt::edl {
namespace {

bool containsAny(const QString& value, std::initializer_list<const char*> needles)
{
    for (const char* needle : needles) {
        if (value.contains(QLatin1StringView(needle), Qt::CaseInsensitive)) return true;
    }
    return false;
}

struct ProgrammerCandidate {
    QString path;
    QString directory;
    qint64 size = 0;
    int score = 0;
};

int numberedProgrammerPriority(const QString& stem)
{
    static const QRegularExpression suffix(
        QStringLiteral("(?:devprg)?programmer(\\d+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = suffix.match(stem);
    if (!match.hasMatch()) return 0;
    bool ok = false;
    const int number = match.captured(1).toInt(&ok);
    // Programmer1 is the canonical package entry.  Later files usually carry
    // alternate certificate chains for another hardware/root-key revision.
    return ok ? qMax(1, 100 - number) : 0;
}

bool betterPath(int score, const QString& path, int currentScore,
                const QString& currentPath)
{
    return score > currentScore ||
           (score == currentScore &&
            (currentPath.isEmpty() ||
             path.compare(currentPath, Qt::CaseInsensitive) < 0));
}

bool isStandaloneSignatureArtifact(const QString& stem)
{
    return stem == QStringLiteral("sig") || stem.startsWith(QStringLiteral("sig_")) ||
           stem.startsWith(QStringLiteral("sign")) ||
           stem.startsWith(QStringLiteral("signature"));
}

} // namespace

bool OplusVipSession::discoverPackage(const QString& directory, StorageType storage,
                                      OplusVipPackage* package, QString* error,
                                      bool recursive,
                                      const QString& preferredProgrammerPath)
{
    if (!package) return false;
    const QDir root(directory);
    if (!root.exists()) {
        if (error) *error = QStringLiteral("VIP 引导包目录不存在");
        return false;
    }

    // Common packages keep all three components together. Resolve that layout
    // before walking a potentially huge firmware tree on the GUI thread.
    if (recursive) {
        OplusVipPackage directPackage;
        if (discoverPackage(root.absolutePath(), storage, &directPackage, nullptr,
                            false, preferredProgrammerPath)) {
            *package = directPackage;
            return true;
        }
    }

    QString programmer;
    QString digest;
    QString signature;
    QVector<ProgrammerCandidate> discoveredProgrammers;
    int programmerScore = -1;
    int digestScore = -1;
    int signatureScore = -1;

    const QString preferredPath = QFileInfo(preferredProgrammerPath).absoluteFilePath();
    const QDirIterator::IteratorFlags iteratorFlags = recursive
        ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator iterator(root.absolutePath(), QDir::Files, iteratorFlags);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        const QString stem = info.completeBaseName().toLower();
        const QString suffix = info.suffix().toLower();

        if (suffix == QStringLiteral("elf") || suffix == QStringLiteral("melf") ||
            suffix == QStringLiteral("mbn") || suffix == QStringLiteral("bin")) {
            int score = 0;
            if (stem.contains(QStringLiteral("devprg"))) score += 180;
            if (stem.contains(QStringLiteral("programmer"))) score += 160;
            if (stem.contains(QStringLiteral("prog"))) score += 120;
            if (stem.contains(QStringLiteral("firehose"))) score += 100;
            if (stem.contains(QStringLiteral("ddr"))) score += 30;
            if (storage == StorageType::Ufs && stem.contains(QStringLiteral("ufs"))) score += 50;
            if (storage == StorageType::Emmc && stem.contains(QStringLiteral("emmc"))) score += 50;
            if (suffix == QStringLiteral("elf")) score += 20;
            if (suffix == QStringLiteral("melf")) score += 30;
            score += numberedProgrammerPriority(stem);
            if (!preferredProgrammerPath.isEmpty() &&
                info.absoluteFilePath().compare(preferredPath, Qt::CaseInsensitive) == 0) {
                score += 1000;
            }
            if (!containsAny(stem, {"digest", "design", "signature"}) &&
                !isStandaloneSignatureArtifact(stem) && score > 0) {
                discoveredProgrammers.push_back({info.absoluteFilePath(), info.absolutePath(),
                                                 info.size(), score});
                if (betterPath(score, info.absoluteFilePath(), programmerScore, programmer)) {
                    programmer = info.absoluteFilePath();
                    programmerScore = score;
                }
            }
        }

        int currentDigestScore = -1;
        if (stem.contains(QStringLiteral("digest"))) currentDigestScore = 220;
        else if (stem.contains(QStringLiteral("design"))) currentDigestScore = 150;
        if (currentDigestScore >= 0) {
            if (stem == QStringLiteral("digest")) currentDigestScore += 100;
            else if (stem.startsWith(QStringLiteral("digest_"))) currentDigestScore += 70;
            if (stem.contains(QStringLiteral("ufs")) && storage == StorageType::Ufs) currentDigestScore += 30;
            if (stem.contains(QStringLiteral("emmc")) && storage == StorageType::Emmc) currentDigestScore += 30;
            if (betterPath(currentDigestScore, info.absoluteFilePath(), digestScore, digest)) {
                digest = info.absoluteFilePath();
                digestScore = currentDigestScore;
            }
        }

        int currentSignatureScore = -1;
        if (stem.contains(QStringLiteral("signature"))) currentSignatureScore = 240;
        else if (stem.contains(QStringLiteral("sign")) &&
                 !containsAny(stem, {"digest", "design"})) currentSignatureScore = 180;
        else if (stem == QStringLiteral("sig") || stem.startsWith(QStringLiteral("sig_")) ||
                 stem.endsWith(QStringLiteral("_sig"))) currentSignatureScore = 120;
        if (stem == QStringLiteral("sign")) currentSignatureScore += 100;
        else if (stem.startsWith(QStringLiteral("sign_"))) currentSignatureScore += 70;
        if (betterPath(currentSignatureScore, info.absoluteFilePath(), signatureScore, signature)) {
            signature = info.absoluteFilePath();
            signatureScore = currentSignatureScore;
        }
    }

    if (programmerScore <= 0 || digest.isEmpty() || signature.isEmpty()) {
        if (error) {
            *error = QStringLiteral("目录中需要 Programmer、Digest 和 Sign 三类文件；当前识别：%1 / %2 / %3")
                         .arg(programmer.isEmpty() ? QStringLiteral("—") : QFileInfo(programmer).fileName(),
                              digest.isEmpty() ? QStringLiteral("—") : QFileInfo(digest).fileName(),
                              signature.isEmpty() ? QStringLiteral("—") : QFileInfo(signature).fileName());
        }
        return false;
    }

    package->enabled = true;
    package->packageDirectory = root.absolutePath();
    package->programmerPath = programmer;
    package->programmerCandidates.clear();
    const QFileInfo preferred(programmer);
    std::sort(discoveredProgrammers.begin(), discoveredProgrammers.end(),
              [](const ProgrammerCandidate& left, const ProgrammerCandidate& right) {
                  if (left.score != right.score) return left.score > right.score;
                  return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
              });
    // Only equal-size files beside the preferred image are considered automatic
    // certificate alternatives. Different-size Programmer3/4 builds may require
    // a different Digest/Sign pair and remain available for explicit selection.
    for (const ProgrammerCandidate& candidate : discoveredProgrammers) {
        if (candidate.directory.compare(preferred.absolutePath(), Qt::CaseInsensitive) == 0 &&
            candidate.size == preferred.size()) {
            package->programmerCandidates.push_back(candidate.path);
        }
    }
    if (!package->programmerCandidates.contains(programmer, Qt::CaseInsensitive)) {
        package->programmerCandidates.prepend(programmer);
    }
    package->digestPath = digest;
    package->signaturePath = signature;
    return true;
}

} // namespace swt::edl
