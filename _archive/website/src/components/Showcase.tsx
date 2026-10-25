import { Reveal } from "./3d/Reveal";
import { useInView } from "@/hooks/useInView";
import { useCountUp } from "@/hooks/useCountUp";
import { SITE_STATS, stageCopy, type SiteInfo } from "@/lib/data";

/* ───────────────────────── 数据带 ───────────────────────── */

function StatCell({ to, suffix, label }: { to: number; suffix: string; label: string }) {
  const { ref, value } = useCountUp(to, 1500);
  return (
    <div className="stat" ref={ref}>
      <b>
        {Math.round(value)}
        {suffix}
      </b>
      <i>{label}</i>
    </div>
  );
}

export function Stats({ site }: { site: SiteInfo }) {
  // 前三格是结构性事实（见 data.ts SITE_STATS 注释），第四格随公测 / 收费开关变化。
  const stats = [
    ...SITE_STATS,
    { to: 100, suffix: "%", label: stageCopy(site).statsLabel },
  ];
  return (
    <section className="sec" style={{ padding: "64px 0" }}>
      <div className="wrap">
        <Reveal>
          <div className="stats">
            {stats.map((s) => (
              <StatCell key={s.label} to={s.to} suffix={s.suffix} label={s.label} />
            ))}
          </div>
        </Reveal>
      </div>
    </section>
  );
}

/* ───────────────────── 使用流程 ───────────────────── */

const FLOW = [
  { n: "01", title: "连接设备", desc: "自动检测 ADB / Fastboot / EDL 9008 与联发科 BROM 设备，认出芯片型号，驱动异常时给出明确指引。" },
  { n: "02", title: "读取分区表", desc: "逐 LUN 解析 GPT，分区按组呈现，可搜索、可勾选 —— 只动你选中的那几个。" },
  { n: "03", title: "解析固件", desc: "rawprogram / patch XML 与 MTK scatter 全量解析，分段镜像按原始扇区完整刷入。" },
  { n: "04", title: "刷写与复位", desc: "实时进度与速率反馈，完成后自动重启；全程日志可保存回溯。" },
];

export function Flow() {
  const { ref, visible } = useInView<HTMLDivElement>(0.12);

  return (
    <section id="flow" className="sec sec-soft">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>03</i>使用流程</span>
            <h2 className="sec-title">四步走完一次刷机</h2>
            <p className="sec-desc">
              每一步都可以停下来看日志、看进度，出了问题能回到上一步；
              不需要刷机时，工具组的七个页面可以单独使用。
            </p>
          </div>
        </Reveal>

        <div className="flow" ref={ref}>
          {FLOW.map((s) => (
            <div className={`flow-item${visible ? " in" : ""}`} key={s.n}>
              <span className="flow-node">{s.n}</span>
              <h3>{s.title}</h3>
              <p>{s.desc}</p>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

/* ───────────────────────── 关于 / 团队 ───────────────────────── */

function RoleIcon({ kind }: { kind: string }) {
  const common = {
    width: 22,
    height: 22,
    viewBox: "0 0 24 24",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: 1.7,
    strokeLinecap: "round" as const,
    strokeLinejoin: "round" as const,
  };
  if (kind === "dev") {
    return (
      <svg {...common} aria-hidden="true">
        <path d="M8.5 8.5 5 12l3.5 3.5" />
        <path d="M15.5 8.5 19 12l-3.5 3.5" />
        <path d="M13.4 6.5 10.6 17.5" />
      </svg>
    );
  }
  if (kind === "server") {
    return (
      <svg {...common} aria-hidden="true">
        <rect x="3.5" y="4" width="17" height="6.5" rx="1.5" />
        <rect x="3.5" y="13.5" width="17" height="6.5" rx="1.5" />
        <path d="M7 7.25h.01" />
        <path d="M7 16.75h.01" />
      </svg>
    );
  }
  return (
    <svg {...common} aria-hidden="true">
      <rect x="3.5" y="4" width="17" height="16" rx="2" />
      <path d="M3.5 9h17" />
      <path d="M9.5 9v11" />
      <path d="M13 14.5h4" />
    </svg>
  );
}

export function Team() {
  const team = [
    { icon: "dev", title: "开发者", name: "小坤ovo", sub: "QQ: 2089449185" },
    { icon: "server", title: "服务器支持", name: "香菜", sub: "QQ: 3328761646" },
    { icon: "design", title: "UI 设计", name: "魚", sub: "QQ: 1708315952" },
  ];
  const thanks = [
    { name: "张天师", qq: "3098258542" },
    { name: "Ramdisk", qq: "2669016593" },
    { name: "小楠xiaonan", qq: "487825006" },
    { name: "清辞", qq: "3756541352" },
    { name: "皓皓的小月", qq: "1541420886" },
  ];
  const reporters = [
    { name: "河南省省长-青墨", qq: "2117598242" },
    { name: "♂甩@棍🇨🇳", qq: "2433862624" },
    { name: "是宝不是贝^", qq: "1781653834" },
  ];

  return (
    <section id="about" className="sec sec-soft">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>07</i>关于</span>
            <h2 className="sec-title">由爱好者维护的工具</h2>
            <p className="sec-desc">
              KunClient 由刷机爱好者一起开发维护，服务器、界面与测试都由社区成员志愿投入。
            </p>
          </div>
        </Reveal>

        <div className="team">
          {team.map((t, i) => (
            <Reveal key={t.title} delay={0.06 * i}>
              <div className="team-card">
                <span className="team-ico"><RoleIcon kind={t.icon} /></span>
                <h3>{t.title}</h3>
                <p>{t.name}</p>
                <small>{t.sub}</small>
              </div>
            </Reveal>
          ))}
        </div>

        <Reveal delay={0.1}>
          <div className="thanks">
            <div className="thanks-title">鸣谢</div>
            <div className="thanks-list">
              {thanks.map((t) => (
                <div className="thanks-item" key={t.name}>
                  <b>{t.name}</b>
                  <span>QQ: {t.qq}</span>
                </div>
              ))}
            </div>
          </div>
        </Reveal>

        <Reveal delay={0.14}>
          <div className="thanks">
            <div className="thanks-title">
              反馈人员
              <i className="thanks-note">提交缺陷与体验问题</i>
            </div>
            <div className="thanks-list">
              {reporters.map((t) => (
                <div className="thanks-item" key={t.name}>
                  <b>{t.name}</b>
                  <span>QQ: {t.qq}</span>
                </div>
              ))}
            </div>
          </div>
        </Reveal>
      </div>
    </section>
  );
}
