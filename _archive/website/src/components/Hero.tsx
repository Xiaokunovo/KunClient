import { Deck } from "./3d/Deck";
import { GridFloor } from "./3d/GridFloor";
import { CONSOLE_URL, stageCopy, type SiteInfo, type VersionInfo } from "@/lib/data";

interface HeroProps {
  info: VersionInfo;
  site: SiteInfo;
}

/**
 * 首屏（v6 工程制图）。
 *
 * 结构：左侧文案 / 右侧 3D 作业台 / 下方协议条。
 * 与 v5 的差别：v5 是「居中大字号 + 下方堆叠窗口」，整页像海报；
 * 这一版把窗口挪到右侧并真正按 Z 轴分层，左侧留给一句能被记住的话。
 *
 * 文案口径：标题讲「结果」（把设备从砖里救回来），副标讲「怎么做到」（模式识别 → 分区 → 刷写）。
 * 下载关闭时不渲染任何下载入口与版本号，只留内测说明（与控制台「官网展示」开关同口径）。
 */
export function Hero({ info, site }: HeroProps) {
  const copy = stageCopy(site);
  const open = site.downloadEnabled !== false;

  return (
    <section className="hero" id="top">
      <div className="wrap hero-inner">
        <div className="hero-copy">
          <span className={`hero-pill${open ? "" : " closed"}`}>
            <i className="dot" />
            {open ? `v${info.version} · ${copy.pill}` : "内测中 · 暂不开放下载"}
          </span>

          <h1>
            <span className="line">把设备</span>
            <span className="line">从<span className="u">砖</span>里救回来</span>
          </h1>

          <p className="hero-tag">Qualcomm · MediaTek / EDL · FASTBOOT · BROM</p>

          <p className="hero-sub">
            接上设备后自动判断它处在 ADB、Fastboot 还是 EDL 9008，联发科机型走 BROM。
            读分区表、勾选分区、解析固件、写入并复位 —— 全在同一个窗口里，
            每一步都留下可保存的日志。
          </p>

          <div className="hero-cta">
            {open ? (
              <a href={info.downloadUrl} className="btn btn-primary btn-lg" download>
                下载 v{info.version}
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
                  <path d="M12 4v11m0 0l-4-4m4 4l4-4M4 20h16" />
                </svg>
              </a>
            ) : (
              /* 关闭态不做成实心主按钮：那是「可点的主操作」的视觉承诺，
                 而这里点了只是跳到下面的说明。改成虚线静态标牌，主操作让给控制台。 */
              <span className="btn btn-lg btn-muted" aria-disabled="true">内测中 · 暂不开放下载</span>
            )}
            <a href={CONSOLE_URL} className={open ? "btn btn-lg" : "btn btn-primary btn-lg"}>进入控制台</a>
          </div>

          <p className="hero-note">单文件免安装 · Windows 10 / 11 64 位 · 无需额外运行时</p>
        </div>

        <div className="hero-visual">
          <GridFloor className="hero-floor" />
          <Deck />
          <span className="dk-float f1"><i className="sq" />EDL 9008 · UFS</span>
        </div>
      </div>

      <div className="proto">
        <div className="wrap">
          <div className="proto-row">
            <div className="proto-cell"><b>EDL 9008</b><span>高通底层写入</span></div>
            <div className="proto-cell"><b>FASTBOOT</b><span>槽位识别与快刷</span></div>
            <div className="proto-cell"><b>MTK BROM</b><span>联发科刷写通道</span></div>
            <div className="proto-cell"><b>ADB</b><span>应用 / 文件 / 命令行</span></div>
            <div className="proto-cell"><b>OFP · OPS</b><span>欧加线刷固件</span></div>
            <div className="proto-cell"><b>SCATTER</b><span>MTK 分区布局</span></div>
          </div>
        </div>
      </div>
    </section>
  );
}
