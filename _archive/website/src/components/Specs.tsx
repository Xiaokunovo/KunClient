import { Reveal } from "./3d/Reveal";
import { SPECS, stageCopy, type SiteInfo, type VersionInfo } from "@/lib/data";

interface SpecsProps {
  info: VersionInfo;
  site: SiteInfo;
}

/**
 * 技术规格（v6）。
 * 用定义列表（dt/dd）而不是四张卡：规格是拿来逐行核对的，表格比卡片好扫。
 * 「使用许可」跟公测/收费开关走；「当前版本」跟官网下载开关走。
 */
export function Specs({ info, site }: SpecsProps) {
  const open = site.downloadEnabled !== false;
  const rows = [
    ...SPECS.filter((s) => open || !s.isVersion),
    { label: "使用许可", value: stageCopy(site).license, isVersion: false },
  ];

  return (
    <section id="specs" className="sec">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>06</i>技术规格</span>
            <h2 className="sec-title">运行环境一目了然</h2>
          </div>
        </Reveal>

        <Reveal delay={0.06}>
          <dl className="specs">
            {rows.map((s) => (
              <div className="spec" key={s.label}>
                <dt>{s.label}</dt>
                <dd className={s.isVersion ? "mono" : ""}>
                  {s.isVersion ? `v${info.version}` : s.value}
                </dd>
              </div>
            ))}
          </dl>
        </Reveal>
      </div>
    </section>
  );
}
