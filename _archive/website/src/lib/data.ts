// 站点与接口配置。默认使用同源相对路径（官网由 server.js 同源托管），
// 换域名（IP / kunflashtool.com）都无需改动；
// 如需指向独立后端或调试环境，可在构建时用环境变量覆盖（next.config 静态导出会在构建期内联）：
//   NEXT_PUBLIC_API_BASE / NEXT_PUBLIC_CONSOLE_URL / NEXT_PUBLIC_ADMIN_URL / NEXT_PUBLIC_ADMIN_EMAIL
export const API_BASE = process.env.NEXT_PUBLIC_API_BASE ?? "";

export const CONSOLE_URL = process.env.NEXT_PUBLIC_CONSOLE_URL ?? "/console/";
export const ADMIN_URL = process.env.NEXT_PUBLIC_ADMIN_URL ?? "/admin/";
// 对外联系邮箱（法务文档、页脚「邮箱联系」都用它）。
// 注意：它**不**等于服务端 config 的 admin_email（那是收信/发通知用的机器邮箱），
// 用户明确要求对外展示只暴露下面这个 QQ 邮箱，别擅自"统一"成 admin_email。
export const ADMIN_EMAIL = process.env.NEXT_PUBLIC_ADMIN_EMAIL ?? "2089449185@qq.com";

// 接口不可用时的兜底下载地址。🔴 必须用带版本号的文件名：
// /assets/KunClient.exe 被 Cloudflare 缓存成旧 .NET 包且无 purge 权限，复用等于发不出去。
// 版本号要跟线上现行版本对齐（发版时由 src/cpp/tools/set-version.js 一并写入）。
export const FALLBACK_DOWNLOAD = "/assets/KunClient-2.0.4.exe";

export const NAV_LINKS = [
  { href: "#caps", label: "能力速览" },
  { href: "#features", label: "能力全景" },
  { href: "#flow", label: "使用流程" },
  { href: "#shots", label: "界面一览" },
  { href: "#download", label: "下载安装" },
  { href: "#faq", label: "常见问题" },
  { href: "#about", label: "关于" },
];

/* ==========================================================================
   能力面口径 —— 全部对齐客户端真源，改这里之前先看这两处：
     ① 客户端导航表：src/cpp/ui/MainShell.cpp  registerPages()
        刷机 6 页 Qualcomm / FASTBOOT / Payload / 欧加线刷 / 小米线刷 / MTK
              （另有 FDL、可视刷写两个隐藏页，由 Qualcomm / FASTBOOT 页内入口跳转）
        工具 7 页 主页 / 安卓工具 / 投屏 / 应用管理 / 文件传输 / 命令行 / 云端驱动
        其他 2 页 设置 / 关于                                    → 共 17 页
     ② 服务端功能目录：server/server.js  FEATURE_CATALOG（20 项）/ MODULE_CATALOG（m1 m2 m3）
        功能清单由 /api/app/limits 实时下发，不在本站写死任何数字。
   🔴 2026-10-13：功能目录对齐过一轮 —— 删掉从未实现的 edl.oneclick（EDL 一键刷写），
      补上真实存在却漏登的小米线刷（xiaomi.flash）。「能力全景」三卡改为跟
      /api/app/limits 实时渲染（见 components/Pillars.tsx）：控制台里迁移 / 取消
      功能归属后，官网刷新即同步，不需要重新构建部署。
   ========================================================================== */

/** 「能力速览」六格。编号、协议名、一句话说明 —— 只讲这台工具真正做的事。
 *  完整能力面见下面的 MODULES（由 /api/app/limits 实时下发）。 */
export const CAPS = [
  { no: "01", proto: "EDL 9008", title: "救砖级底层写入", desc: "绕过系统直接对 UFS / eMMC 写分区，连 Fastboot 都进不去的机器也能拉回来。" },
  { no: "02", proto: "FASTBOOT", title: "槽位识别与快刷", desc: "自动识别 A/B 槽位与当前活动槽，boot / dtbo / vbmeta 按槽位写入。" },
  { no: "03", proto: "MTK BROM", title: "联发科刷写通道", desc: "BROM 握手与 DA 高速通道，解析 scatter，下载、读回、擦除分区。" },
  { no: "04", proto: "GPT / LUN", title: "按分区勾选即刷", desc: "逐 LUN 解析分区表，可搜索、可分组勾选，只动你选中的那几个。" },
  { no: "05", proto: "OFP / OPS", title: "欧加线刷固件", desc: "直接解析厂商 OFP / OPS 包，不必先手工转成 rawprogram 再刷。" },
  { no: "06", proto: "SCRCPY", title: "投屏与键鼠接管", desc: "内置 scrcpy，刷机前后都能在电脑上直接操作设备、看进度。" },
];

/** 功能 key → 「能力全景」卡片里的一行文案。
 *  键必须是服务端 FEATURE_CATALOG 里真实存在的 key；卡片由 Pillars.tsx 按
 *  /api/app/limits 下发的模块归属实时拼装，本表只负责「每个功能怎么描述」。
 *  新增 / 删除功能时：先改 server.js 的 FEATURE_CATALOG，再补这里的一行。 */
export const FEATURE_HINTS: Record<string, string> = {
  // 模块1 刷写与擦除
  'edl.write': 'Qualcomm · EDL 9008 写入指定分区',
  'edl.erase': 'Qualcomm · 擦除 / 格式化分区',
  'edl.gpt': 'Qualcomm · GPT 分区表写入 / 修复',
  'fastboot.flash': 'FASTBOOT · 槽位识别与快刷',
  'fastboot.erase': 'FASTBOOT · 擦除 / 格式化',
  'fastboot.bl': 'FASTBOOT · 引导锁解锁 / 回锁',
  'oneplus.flash': '欧加线刷 · OFP / OPS 一键刷写',
  'xiaomi.flash': '小米线刷 · flash_all 三档脚本',
  'mtk.flash': 'MTK · BROM 握手与 scatter 刷写',
  // 模块2 读取与备份
  'edl.readtable': '分区表 · 按 LUN 分组读取，可搜索',
  'edl.backup': '分区回读 · 导出为镜像文件',
  'payload.extract': 'Payload · 超级镜像分区提取',
  'mtk.read': 'MTK · BROM 直读分区',
  // 模块3 专项与工具
  'android.tools': '安卓工具 · OFP / OPS / Super 处理',
  'mirror.scrcpy': '投屏 · scrcpy 键鼠接管',
  'app.manage': '应用管理 · 卸载 / 冻结 / 备份 APK',
  'file.transfer': '文件传输 · 拖放与批量安装',
  'cmd.shell': '命令行 · ADB 与 Fastboot 指令台',
  'driver.cloud': '云端驱动 · USB / ADB / MTK 驱动下载',
};

/** 三块模块的静态回落版（仅 /api/app/limits 拉不到时使用）。
 *  🔴 必须与 FEATURE_HINTS + 服务端默认归属保持一致：Pillars.tsx 优先渲染实时配置，
 *     这里只是「接口不可达」时的兜底，别把它当成第二份真相。
 *     校验入口：tools/check-modules-drift.js（对着 server.js 的默认分类逐项比对）。 */
export const MODULES = [
  {
    no: '01',
    glyph: 'chip',
    moduleId: 'm1',
    title: '刷写与擦除',
    desc: 'EDL / Fastboot / MTK 刷写、擦除、GPT 与引导锁操作。',
    items: [
      'Qualcomm · EDL 9008 写入指定分区',
      'Qualcomm · 擦除 / 格式化分区',
      'Qualcomm · GPT 分区表写入 / 修复',
      'FASTBOOT · 槽位识别与快刷',
      'FASTBOOT · 擦除 / 格式化',
      'FASTBOOT · 引导锁解锁 / 回锁',
      '欧加线刷 · OFP / OPS 一键刷写',
      '小米线刷 · flash_all 三档脚本',
      'MTK · BROM 握手与 scatter 刷写',
    ],
  },
  {
    no: '02',
    glyph: 'layers',
    moduleId: 'm2',
    title: '读取与备份',
    desc: '读取分区表、分区备份 / 导出镜像、Payload 分区提取、MTK 分区读回。',
    items: [
      '分区表 · 按 LUN 分组读取，可搜索',
      '分区回读 · 导出为镜像文件',
      'Payload · 超级镜像分区提取',
      'MTK · BROM 直读分区',
    ],
  },
  {
    no: '03',
    glyph: 'grid',
    moduleId: 'm3',
    title: '专项与工具',
    desc: 'Android 工具（OFP / OPS / Super）、屏幕镜像、应用管理、文件传输、命令行、云端驱动。',
    items: [
      '安卓工具 · OFP / OPS / Super 处理',
      '投屏 · scrcpy 键鼠接管',
      '应用管理 · 卸载 / 冻结 / 备份 APK',
      '文件传输 · 拖放与批量安装',
      '命令行 · ADB 与 Fastboot 指令台',
      '云端驱动 · USB / ADB / MTK 驱动下载',
    ],
  },
];

/** 数据带数字。⚠️ 这几个数是**结构性事实**，不是营销数字：
 *  17 = MainShell.cpp 注册页面数；6 = 刷机通道数；4 = 设备模式数（ADB/Fastboot/EDL/BROM）。
 *  改动客户端导航表时同步这里。（第 4 格是随公测/收费开关变化的，由组件追加。） */
export const SITE_STATS = [
  { to: 17, suffix: "", label: "功能页面 · 三组导航" },
  { to: 6, suffix: "", label: "刷机通道 · 高通 / 联发科 / 厂商专用" },
  { to: 4, suffix: "", label: "设备模式 · 自动识别" },
];

export const STEPS = [
  {
    title: "下载程序",
    desc: "本页下载最新版 KunClient.exe，原生单文件免安装，双击即可运行；对体积敏感可改用 7z 压缩包，内容完全一致。",
  },
  {
    title: "注册并登录",
    desc: "在控制台用邮箱注册（需邮件验证码），回到程序登录即可开始。账号同时管理密钥与使用记录。",
  },
  {
    title: "连接设备",
    desc: "数据线连接手机，程序自动判断它处在 ADB、Fastboot 还是 EDL 9008；联发科机型走 BROM 通道。读分区表、勾选分区、写入并复位。",
  },
];

export const SPECS = [
  { label: "支持系统", value: "Windows 10 / 11 64 位" },
  { label: "程序大小", value: "约 39 MB · 免安装单文件" },
  { label: "当前版本", value: "", isVersion: true },
  { label: "芯片支持", value: "高通 Qualcomm · 联发科 MTK" },
  { label: "设备模式", value: "ADB · Fastboot · EDL 9008 · MTK BROM" },
  // 「使用许可」不写在这里：它随控制台的公测/收费开关变化，由 Specs 组件按阶段追加
];

export const FAQ = [
  { q: "需要安装额外的运行环境吗？", a: "不需要。自 2.0.0 起 KunClient 为原生单文件程序，下载后双击即可运行，无需安装 .NET 或其他运行时。" },
  { q: "文件太大，有没有更小的下载？", a: "有。下载区提供 7z 压缩包（约 10.6 MB，约为完整包的 1/4），用 7-Zip 或 WinRAR 解压后得到完全相同的 KunClient.exe；两种包内容一致，只是分发形态不同。" },
  { q: "支持哪些设备？", a: "分两条芯片线：高通机型走 Fastboot / EDL，常见品牌包括 OPPO、OnePlus、realme、小米、vivo、iQOO、三星等；联发科机型走 MTK BROM 通道。实际能否刷写取决于机型固件与解锁状态。" },
  { q: "支持联发科（MTK）机型吗？", a: "支持。MTK 页负责设备发现、BROM 握手与 scatter 解析，已打通 DA 高速通道，可直接下载、读回与擦除分区。" },
  { q: "EDL 模式是什么？", a: "EDL（Emergency Download）是高通芯片的紧急下载模式，设备管理器中显示为 9008 端口，用于深度刷机与救砖。联发科对应的底层通道是 BROM。" },
  { q: "命令行页能做什么？", a: "它是一个 ADB / Fastboot 指令台：常用指令有可视化入口，也可直接输入原始命令，输出原样回显，适合排查设备状态或执行工具里没做成按钮的操作。" },
  { q: "云端驱动是什么？", a: "把常用的 USB / ADB / Fastboot / 联发科驱动集中在一处下载安装，避免装机时到处找驱动；驱动包由服务器统一分发，打开即可取用。" },
  { q: "账号在哪里注册？", a: "在控制台首页即可注册，注册需要填写邮箱并输入邮件验证码；登录邮件通知可在注册时自行选择是否开启。" },
  { q: "刷机有风险吗？", a: "有风险。刷机可能导致数据丢失或设备无法开机，操作前请务必备份重要数据，并自行承担相应后果。本工具仅供学习交流使用。" },
];

export interface VersionInfo {
  version: string;
  buildDate: string;
  changelog: string;
  downloadUrl: string;
  forceUpdate: boolean;
}

export const FALLBACK_VERSION: VersionInfo = {
  version: "2.0.4",
  buildDate: "",
  changelog: "",
  downloadUrl: FALLBACK_DOWNLOAD,
  forceUpdate: false,
};

/* ==========================================================================
   对外口径（公测免费 / 正式收费）——由控制台「官网展示」开关决定
   官网首页运行时拉 /api/site/info，所以管理员切换后刷新即生效，
   不需要重新构建/部署本站；这里的 FALLBACK_SITE 只在接口不可达时兜底。
   ========================================================================== */

// 2026-10-20：付费下线，站点只剩「公测中」与「完全免费」两档。
export type SiteStage = "beta" | "free";

export interface SiteInfo {
  stage: SiteStage;
  freeNow: boolean;
  paidMode: boolean;
  vipRequired: boolean;
  trialDays: number;
  priceNote: string;
  version: string;
  /** 官网是否开放下载（控制台「运营 › 官网展示 › 官网下载」开关）。
   *  关掉后官网收起下载区、首屏下载按钮与全部版本号 —— 用于内测期 / 暂不发版。 */
  downloadEnabled: boolean;
}

export const FALLBACK_SITE: SiteInfo = {
  stage: "beta",
  freeNow: true,
  paidMode: false,
  vipRequired: false,
  trialDays: 0,
  priceNote: "",
  version: FALLBACK_VERSION.version,
  // 静态兜底默认「关闭下载」（fail-closed）：/api/site/info 回来之前先不展示下载入口，
  // 内测期在首屏 / 无 JS 场景下也不会把下载露出去；接口一回来即以服务端为准。
  downloadEnabled: false,
};

/** 官网下载关闭时的对外文案（控制台「运营 › 官网展示 › 官网下载」关掉后显示这两句）。 */
export const DOWNLOAD_CLOSED_TITLE = "内测中";
export const DOWNLOAD_CLOSED_DESC =
  "当前为内测阶段，暂不开放下载；开放时间请关注控制台公告。";

/** 收费口径说明 —— 与控制台/客户端关于页的「关于收费」保持一字不差，避免三端口径打架 */
// 2026-10-20：付费体系下线，原来的「收费只用于维护」口径已不适用。
const CHARGE_NOTE = "KunClient 全部功能免费，没有内购、卡密或会员等级。";

/** 三个阶段的所有对外文案集中在这一张表里，避免官网各处口径打架 */
const STAGE_COPY: Record<SiteStage, {
  pill: string;        // Hero 徽标「当前版本 vX · ___」
  license: string;     // 技术规格「使用许可」
  statsLabel: string;  // 数据带第 4 项
  faqQ: string;
  faqA: string;
}> = {
  beta: {
    pill: "公测期免费使用",
    license: "公测期免费使用",
    statsLabel: "公测期免费开放",
    faqQ: "公测期收费吗？",
    faqA: "完全免费，全部功能开放，不需要付费、不需要卡密、也没有会员等级。" + CHARGE_NOTE,
  },
  free: {
    pill: "免费使用",
    license: "免费使用",
    statsLabel: "全部功能免费开放",
    faqQ: "KunClient 需要付费吗？",
    faqA: "不需要。全部功能免费开放：没有会员等级、没有卡密、没有每日次数限制，登录账号即可使用。" + CHARGE_NOTE,
  },
};

export function stageCopy(site: SiteInfo) {
  return STAGE_COPY[site.stage] ?? STAGE_COPY.beta;
}

/** 常见问题里与收费相关的条目按阶段生成，跟 Hero / 规格表保持同一口径。
 *  2026-10-20：付费体系下线，不再生成「会员 / 卡密怎么获取」那一条。 */
export function stageFaqs(site: SiteInfo): { q: string; a: string }[] {
  const c = stageCopy(site);
  return [{ q: c.faqQ, a: c.faqA }];
}
