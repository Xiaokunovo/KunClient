"use client";

import { useMemo, useState } from "react";
import { NAV_LINKS, CONSOLE_URL } from "@/lib/data";
import { useActiveSection } from "@/hooks/useActiveSection";

interface NavProps {
  scrolled: boolean;
  /** 官网下载关闭时传空串：版本号一起收起（与控制台「官网展示」开关同口径）。 */
  version?: string;
}

/**
 * 顶部导航（v6）。
 * 与旧版差别：链接增加到 8 个（能力速览 / 全景 / 流程 / 界面 / 下载 / 会员 / 问题 / 关于），
 * 右侧多一枚等宽版本号 —— 产品页把「当前版本」放在导航里，用户第一眼就能确认自己看的是哪一版。
 */
export function Nav({ scrolled, version = "" }: NavProps) {
  const [menu, setMenu] = useState(false);
  const ids = useMemo(() => NAV_LINKS.map((l) => l.href.replace("#", "")), []);
  const active = useActiveSection(ids);

  return (
    <nav className={`nav${scrolled || menu ? " scrolled" : ""}`}>
      <div className="wrap nav-inner">
        <a href="/" className="brand" aria-label="KunClient 首页">
          <img className="brand-mark" src="/logo.png" alt="" width={26} height={26} />
          <span className="brand-text">Kun<em>Client</em></span>
        </a>

        <div className="nav-links">
          {NAV_LINKS.map((l) => (
            <a
              key={l.href}
              href={l.href}
              className={active === l.href.replace("#", "") ? "active" : ""}
            >
              {l.label}
            </a>
          ))}
        </div>

        <div className="nav-right">
          {version ? <span className="nav-ver">v{version}</span> : null}
          <a href={CONSOLE_URL} className="btn btn-sm">控制台</a>
          <button
            type="button"
            className="nav-toggle"
            aria-label="菜单"
            aria-expanded={menu}
            onClick={() => setMenu((v) => !v)}
          >
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round">
              {menu ? <path d="M6 6l12 12M6 18L18 6" /> : <path d="M4 7h16M4 12h16M4 17h16" />}
            </svg>
          </button>
        </div>
      </div>

      <div className={`mobile-menu${menu ? " open" : ""}`}>
        <div>
          <div className="wrap">
            {NAV_LINKS.map((l) => (
              <a key={l.href} href={l.href} onClick={() => setMenu(false)}>{l.label}</a>
            ))}
            <a href={CONSOLE_URL} className="mm-cta" onClick={() => setMenu(false)}>进入控制台</a>
          </div>
        </div>
      </div>
    </nav>
  );
}
