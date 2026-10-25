import { Reveal } from "./3d/Reveal";
import { Tilt } from "./3d/Tilt";
import { useInView } from "@/hooks/useInView";

/**
 * 界面一览（v6）。
 *
 * 🔴 这里的每一行字都必须能在客户端源码里找到出处，不许编：
 *   · Qualcomm/EDL 页：ui/MainWindow.cpp —— 列头「分区名 / 文件名 / 大小 / 状态」，
 *     按钮「读取分区表 / 读取版本 / 读取分区 / 擦除分区 / 写入分区」，
 *     复选框「自动重启 / 保护分区 / 读取信息」，计数「已勾选 N 个分区」。
 *   · MTK 页：ui/pages/MtkFlashPage.cpp —— scatter 文件行、读数「芯片 / 存储 / 设备 / HW code」、
 *     按钮「连接设备 / 下载选中 / 读回选中 / 全选 / 全不选」、状态「就绪 / 保护分区」。
 *   · 刷写日志格式：app/EdlWorker.cpp —— `写入 %1/%2：%3 → %4（LUN %5）`，速率由 MainWindow 显示为
 *     `速率  XX.XX MB/s`，总进度与当前条目进度是两条独立的进度条。
 *
 * 版式：EDL 占 7 列、MTK 占 5 列、刷写进度整行 12 列 —— 三张图对应「读 → 写 → 看结果」
 * 的实际使用顺序，而不是三张一样大的并排卡。
 */

function Win({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="win">
      <div className="win-bar">
        <span className="win-dots"><i /><i /><i /></span>
        <span className="win-title">{title}</span>
      </div>
      <div className="win-body">{children}</div>
    </div>
  );
}

function ShotEdl() {
  const rows = [
    ["☑", "super", "super_4.img", "5.8 GB", "就绪"],
    ["☑", "userdata", "userdata_1.img", "116 GB", "就绪"],
    ["☑", "boot_a", "boot_a.img", "96 MB", "就绪"],
    ["☑", "misc", "misc.img", "1 MB", "就绪"],
    ["☐", "xbl_a", "xbl_a.img", "3.5 MB", "就绪"],
    ["☐", "modem_a", "modem_a.img", "195 MB", "保护分区"],
  ];
  return (
    <Win title="KunClient — Qualcomm · EDL 刷写">
      <div className="win-toolbar">
        <span className="win-pill"><i className="dot" />已连接 · 9008 / UFS</span>
        <span className="win-dim">已勾选 86 个分区</span>
        <span className="win-search">搜索分区</span>
      </div>
      <div className="win-checks">
        <span>☑ 自动重启</span>
        <span>☑ 保护分区</span>
        <span>☑ 读取信息</span>
      </div>
      <div className="win-table">
        <div className="win-tr head"><span>选</span><span>分区名</span><span>文件名</span><span>大小</span><span>状态</span></div>
        {rows.map((r) => (
          <div className="win-tr" key={r[1]}>
            <span>{r[0]}</span>
            <span>{r[1]}</span>
            <span>{r[2]}</span>
            <span>{r[3]}</span>
            <span className={r[4] === "保护分区" ? "warn" : "ok"}>{r[4]}</span>
          </div>
        ))}
      </div>
      <div className="win-foot">
        <span className="win-btn">读取分区表</span>
        <span className="win-btn">读取版本</span>
        <span className="win-btn">读取分区</span>
        <span className="win-btn">擦除分区</span>
        <span className="win-btn pri">写入分区</span>
      </div>
    </Win>
  );
}

function ShotMtk() {
  const rows = [
    ["☑", "preloader", "0x0", "512 KB", "就绪"],
    ["☑", "boot", "0x800000", "64 MB", "就绪"],
    ["☑", "system", "0x4800000", "1.2 GB", "就绪"],
    ["☐", "userdata", "0x1A000000", "48 GB", "就绪"],
    ["☐", "nvram", "0x2C000000", "5 MB", "保护分区"],
  ];
  return (
    <Win title="KunClient — MTK 刷写">
      <div className="win-toolbar">
        <span className="win-pill"><i className="dot" />BROM 已连接</span>
        <span className="win-dim">scatter.txt</span>
        <span className="win-search">搜索分区</span>
      </div>
      <div className="win-checks">
        <span>芯片: MT6789</span>
        <span>存储: UFS</span>
        <span>HW code: 0x0E8D</span>
      </div>
      <div className="win-table">
        <div className="win-tr mtk head"><span>选</span><span>分区</span><span>起始地址</span><span>大小</span><span>状态</span></div>
        {rows.map((r) => (
          <div className="win-tr mtk" key={r[1]}>
            <span>{r[0]}</span>
            <span>{r[1]}</span>
            <span>{r[2]}</span>
            <span>{r[3]}</span>
            <span className={r[4] === "保护分区" ? "warn" : "ok"}>{r[4]}</span>
          </div>
        ))}
      </div>
      <div className="win-foot">
        <span className="win-btn">全选</span>
        <span className="win-btn">全不选</span>
        <span className="win-btn">读回选中</span>
        <span className="win-btn pri">下载选中</span>
      </div>
    </Win>
  );
}

function ShotFlash() {
  const { ref, visible } = useInView<HTMLDivElement>(0.25);
  return (
    <div ref={ref}>
      <Win title="KunClient — 正在刷写">
        <div className="win-prog">
          <div className="win-pl"><span>总进度</span><b>{visible ? "72%" : "0%"}</b></div>
          <div className="win-track"><i style={{ width: visible ? "72%" : "0%" }} /></div>
          <div className="win-pl"><span>super_4.img</span><b>{visible ? "61%" : "0%"}</b></div>
          <div className="win-track"><i className="green" style={{ width: visible ? "61%" : "0%" }} /></div>
        </div>
        <div className="win-checks">
          <span>速率 38.42 MB/s</span>
          <span>已写入 4497.6 MB / 6144.0 MB</span>
        </div>
        <div className="win-log">
          <div>写入 12/47：super ← super_4.img（LUN 0）</div>
          <div className="dim">Android Sparse：1 个块，展开 4497.6 MB</div>
          <div>检测到 1 个 super 条目：启用免合并分段刷写</div>
          <div className="dim">读写任务完成，按设置自动重启到系统…</div>
        </div>
        <div className="win-foot">
          <span className="win-btn pri">刷写中…</span>
          <span className="win-btn">取消</span>
        </div>
      </Win>
    </div>
  );
}

export function Shots() {
  return (
    <section id="shots" className="sec">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>04</i>界面一览</span>
            <h2 className="sec-title">两条芯片线，同一套界面</h2>
            <p className="sec-desc">
              高通 EDL 与联发科 MTK 是两套底层协议，但分区表、勾选方式、进度与日志的读法是一致的 ——
              换机型不用重新学一遍。
            </p>
          </div>
        </Reveal>

        <div className="shots">
          <Reveal className="shot-cell a" delay={0.04}>
            <Tilt max={3}><ShotEdl /></Tilt>
          </Reveal>
          <Reveal className="shot-cell b" delay={0.12}>
            <Tilt max={3}><ShotMtk /></Tilt>
          </Reveal>
          <Reveal className="shot-cell c" delay={0.18}>
            <Tilt max={2}><ShotFlash /></Tilt>
          </Reveal>
        </div>
      </div>
    </section>
  );
}
