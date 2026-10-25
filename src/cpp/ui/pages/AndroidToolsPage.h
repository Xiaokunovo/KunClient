#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AndroidToolsPage — Android 工具箱
//
//  移植自 tests/src/KunClient.App/Pages/AndroidToolsPage.{xaml,xaml.cs}。
//  C# 页面是「左列 设备信息+日志 / 右列 5 个工具分组框（可滚）」的两列版式，
//  这里照搬：
//   · OPLUS Super 合并 —— SuperMergeService（super_def.*.json 解析 +
//     simg2img 逐分区转 raw + lpmake 合并，工具从 exe/Tools 或 PATH 找）；
//   · OFP / OPS 解包 —— 真解包（移植自 YukiClient 的 OfpExtractor / OpsExtractor）：
//     OFP 三种形态（加密 zip / 联发科 / 高通）自动识别，OPS 走 MBox 4/5/6 自定义密钥流；
//   · Payload 转 Fastboot —— 先试 adb shell payload_dumper，失败则按
//     payload.bin 头 4KB 的分区名生成 flash_all.sh（与 C# 完全一致的命令）；
//   · OCDT 生成 —— 真 TDCO 容器（移植自 YukiClient `Core/Violet/OcdtGenerator.cs`）：
//     机型代号 → 项目号 → 8MB 联发科 / 128KB 高通整幅镜像，配置块走 geyixue
//     加密，旧款联发科另写 OSIG 块；同卡另带「分析 OCDT」反查项目号与 OSIG。
//   · TWRP 刷写 —— 分区读取（dd → /sdcard → pull）/ 写入（push → dd）/
//     擦除（dd /dev/zero），命令与 C# 逐字一致。
//
//  全部长操作走 QThreadPool + runOnUi（等价 C# 的 async/await）；
//  会员闸门 MembershipService::guardAndConsume（同步阻塞，只在 worker 线程调）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include "storage/OcdtGenerator.h"

#include <QAtomicInt>
#include <QVector>

class QComboBox;
class QLineEdit;
class QPushButton;

namespace kc::ui {

class NotificationBar;

class AndroidToolsPage : public BasePage {
    Q_OBJECT

public:
    explicit AndroidToolsPage(QWidget* parent = nullptr);
    ~AndroidToolsPage() override;

private:
    void buildUi();

    // ── UI 线程入口（校验输入 → 弹对话框 → 线程池跑核心）────────────────
    void browseSuper();
    void mergeSuper();
    void browseOfp();
    void unpackOfp();
    void unpackOps();
    void browsePayload();
    void convertPayload();
    void generateOcdt();
    void analyzeOcdt();      ///< 选一个 OCDT 镜像 → 反查项目号 / 变体 / OSIG
    void browseTwrpImage();
    void twrpRead();
    void twrpWrite();
    void twrpErase();
    void twrpFlashZip();     ///< 卡刷包（.zip）→ adb push + twrp install

    // ── 固件校验（MD5 / SHA-256）────────────────────────────────────────
    //  移植自 TMFTool 的「MD5校验 / 校验Sha256 / 生成校验文件」三项。
    //  纯本地计算，不走会员额度闸门（不产生服务端成本，也不是付费能力）。
    void browseChecksumTarget();   ///< 选单个文件填进输入框
    void browseChecksumDir();      ///< 选目录填进输入框（批量模式）
    void runChecksum();            ///< 算摘要并打印
    void writeChecksumFile();      ///< 算摘要并写 .md5 / .sha256
    void verifyChecksumFile();     ///< 选校验文件 → 逐条核对同目录文件

    // ── 线程池执行体（阻塞实现，禁止在 UI 线程调用）───────────────────────
    void mergeCore(const QString& romDir);
    void convertPayloadCore(const QString& payloadFile);
    void unpackOfpCore(const QString& sourcePath, const QString& outputDirectory);
    void unpackOpsCore(const QString& sourcePath, const QString& outputDirectory);
    void generateOcdtCore(int projectId, swt::edl::OcdtPlatform platform,
                          swt::edl::OcdtVariant variant, const QString& modelCode,
                          const QString& osigPath, const QString& outputPath);
    void analyzeOcdtCore(const QString& imagePath);
    void twrpReadCore(const QString& part);
    void twrpWriteCore(const QString& part, const QString& imageFile);
    void twrpEraseCore(const QString& part);
    void twrpFlashZipCore(const QString& zipFile);
    void checksumCore(const QString& path, int algoMode, bool writeFile);
    void verifyChecksumCore(const QString& checksumFile);

    void setBusy(bool busy);

    NotificationBar* m_notify = nullptr;
    QLineEdit* m_superEdit = nullptr;
    QLineEdit* m_ofpEdit = nullptr;
    QLineEdit* m_payloadEdit = nullptr;
    QLineEdit* m_twrpPartitionEdit = nullptr;
    QLineEdit* m_checksumEdit = nullptr;     ///< 固件校验的目标（文件或目录）
    QComboBox* m_checksumAlgo = nullptr;     ///< SHA-256 / MD5 / 两者都算

    // OCDT 生成卡（机型代号 → 项目号 → 整幅镜像）
    QComboBox* m_ocdtPlatform = nullptr;     ///< 8MB 联发科 / 128KB 高通
    QLineEdit* m_ocdtModel = nullptr;        ///< 机型代号（带内置表补全）
    QLineEdit* m_ocdtProjectId = nullptr;    ///< 项目号（选中机型时自动带出）
    QComboBox* m_ocdtVariant = nullptr;      ///< standard / fill02 / geyixue_fill / mixed_projid
    QLineEdit* m_ocdtOsigEdit = nullptr;     ///< OSIG 备份镜像（可选）

    QVector<QPushButton*> m_busyButtons;
    QAtomicInt m_busy{0};
};

} // namespace kc::ui
