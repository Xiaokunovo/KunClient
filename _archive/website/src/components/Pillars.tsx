"use client";

import { useEffect, useState } from "react";
import { Reveal } from "./3d/Reveal";
import { API_BASE, FEATURE_HINTS, MODULES } from "@/lib/data";

/* ───────────────────────── 三块模块（实时配置 + 静态兜底） ───────────────────────── */

interface PillarData {
  no: string;
  title: string;
  desc: string;
  items: string[];
}

interface LiveModule {
  id: string;
  name: string;
  title: string;
  desc?: string;
  features?: string[];
}

interface LiveLimits {
  modules?: LiveModule[];
  features?: { key: string; label: string }[];
}

const MODULE_NO: Record<string, string> = { m1: "01", m2: "02", m3: "03" };

/**
 * 能力全景（v6）。
 *
 * 数据契约不变：优先读 /api/app/limits 下发的模块与功能归属（控制台里迁移功能后
 * 官网刷新即同步），拉不到就用 data.ts 的 MODULES 兜底（由 check-modules-drift.js 对账）。
 *
 * 视觉上从「三张带图标的卡」改成「三列功能清单」：
 *   · 模块名 / 编号 / 功能数在顶部一行，下面是密排的功能行 —— 用户是来查「这个功能在不在」的，
 *     不是来看图标的；
 *   · 卡底标注归属模块，与控制台「增值模块」里的编号对得上。
 */
export function Pillars() {
  const [pillars, setPillars] = useState<PillarData[] | null>(null);

  useEffect(() => {
    let alive = true;
    fetch(`${API_BASE}/api/app/limits`)
      .then((r) => r.json())
      .then((d: { success?: boolean; data?: LiveLimits }) => {
        if (!alive || !d?.success || !d.data) return;
        const mods = Array.isArray(d.data.modules) ? d.data.modules : [];
        const labels = new Map<string, string>();
        (d.data.features || []).forEach((f) => labels.set(f.key, f.label || f.key));
        if (!mods.length) return;
        setPillars(
          mods.map((m, i) => ({
            no: MODULE_NO[m.id] || String(i + 1).padStart(2, "0"),
            title: m.title || m.name,
            desc: m.desc || "",
            items: (m.features || []).map((k) => FEATURE_HINTS[k] || labels.get(k) || k),
          }))
        );
      })
      .catch(() => {});
    return () => { alive = false; };
  }, []);

  const shown: PillarData[] = pillars || MODULES;
  const titles = shown.map((p) => p.title).join("、");

  return (
    <section id="features" className="sec">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>02</i>能力全景</span>
            <h2 className="sec-title">三块模块，覆盖全部功能</h2>
            <p className="sec-desc">
              {titles} —— 客户端十七个页面按这三块组织，
              会员授权也按这三块开通，官网、控制台与程序里看到的分类是同一套。
            </p>
          </div>
        </Reveal>

        <div className="mods">
          {shown.map((p, i) => (
            <Reveal key={p.no} delay={0.06 * i}>
              <article className="mod">
                <div className="mod-head">
                  <div className="mod-top">
                    <span className="mod-no">MODULE {p.no}</span>
                    <span className="mod-count">{p.items.length} 项</span>
                  </div>
                  <h3>{p.title}</h3>
                  <p className="mod-desc">{p.desc}</p>
                </div>
                <ul className="mod-list">
                  {p.items.length === 0 ? (
                    <li style={{ color: "var(--ink-3)" }}>暂未包含功能</li>
                  ) : (
                    p.items.map((it) => <li key={it}>{it}</li>)
                  )}
                </ul>
                <div className="mod-foot">归入模块 {p.no} · 会员授权按模块开通</div>
              </article>
            </Reveal>
          ))}
        </div>
      </div>
    </section>
  );
}
