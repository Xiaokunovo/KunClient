"use client";

import { useEffect, useState } from "react";
import { API_BASE, FALLBACK_VERSION, FALLBACK_SITE, type SiteInfo, type VersionInfo } from "@/lib/data";
import { useScroll } from "@/hooks/useScroll";
import { Nav } from "@/components/Nav";
import { Hero } from "@/components/Hero";
import { Capabilities } from "@/components/Capabilities";
import { Pillars } from "@/components/Pillars";
import { Stats, Flow, Team } from "@/components/Showcase";
import { Shots } from "@/components/Shots";
import { Download } from "@/components/Download";
import { Specs } from "@/components/Specs";
import { FAQ } from "@/components/FAQ";
import { Footer } from "@/components/Footer";

export default function Home() {
  const [info, setInfo] = useState<VersionInfo>(FALLBACK_VERSION);
  const [site, setSite] = useState<SiteInfo>(FALLBACK_SITE);
  const scrolled = useScroll();

  useEffect(() => {
    let alive = true;
    fetch(`${API_BASE}/api/app/version`)
      .then((r) => r.json())
      .then((d) => {
        if (!alive || !d?.success || !d.data) return;
        setInfo({
          version: d.data.version || FALLBACK_VERSION.version,
          buildDate: d.data.buildDate || "",
          changelog: d.data.changelog || "",
          downloadUrl: d.data.downloadUrl || FALLBACK_VERSION.downloadUrl,
          forceUpdate: !!d.data.forceUpdate,
        });
      })
      .catch(() => {});
    return () => { alive = false; };
  }, []);

  // 公测免费 / 正式收费 由控制台「官网展示」开关决定，这里实时拉取，
  // 管理员切换后访客刷新即生效 —— 不需要重新构建或部署本站。
  useEffect(() => {
    let alive = true;
    fetch(`${API_BASE}/api/site/info`)
      .then((r) => r.json())
      .then((d) => {
        if (!alive || !d?.success || !d.data) return;
        const s = d.data;
        setSite({
          // 2026-10-20：付费下线，站点只剩 beta / free 两档（服务端已固定返回 free）
          stage: s.stage === "free" ? "free" : "beta",
          freeNow: s.freeNow !== false,
          paidMode: !!s.paidMode,
          vipRequired: !!s.vipRequired,
          trialDays: Number(s.trialDays) || 0,
          priceNote: s.priceNote || "",
          version: s.version || FALLBACK_SITE.version,
          downloadEnabled: s.downloadEnabled !== false,
        });
      })
      .catch(() => {});
    return () => { alive = false; };
  }, []);

  return (
    <>
      <Nav scrolled={scrolled} version={site.downloadEnabled !== false ? info.version : ""} />
      <main>
        {/* v6 段序（编号即页面上的章节号）：
             首屏 → 01 能力速览 → 02 能力全景 → 数据带 → 03 使用流程 →
             04 界面一览 → 05 下载安装 → 06 技术规格 → 07 会员权益 →
             08 常见问题 → 09 关于 → 页脚。
            浅色带（sec-soft）与白底交替，靠发丝线切段，不再有「深色段 + 浅色段」拼接。 */}
        <Hero info={info} site={site} />
        <Capabilities />
        <Pillars />
        <Stats site={site} />
        <Flow />
        <Shots />
        <Download info={info} site={site} />
        <Specs info={info} site={site} />
        <FAQ site={site} />
        <Team />
      </main>
      <Footer info={info} site={site} />
    </>
  );
}
