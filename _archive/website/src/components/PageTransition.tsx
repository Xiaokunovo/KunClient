"use client";

import { useEffect } from "react";
import { usePathname } from "next/navigation";

/**
 * 跨页过渡（v6 简化版）。
 *
 * 静态导出下每次点站内链接都是整页重载，所以只做两件事：
 *  ① 发起端：拦下站内跨页链接，给 body 加 `.page-leave`（160ms 淡出）再跳；
 *  ② 到达端：读到 sessionStorage 标记，给 body 加 `.page-enter` 播一次淡入。
 *
 * 旧版那套「镜头下潜 + 旋转」已经撤掉：这一版整站是纸白仪器风，
 * 旋转转场与静态页面的气质不搭，而且要多等 340ms。
 *
 * 只拦真·跨页链接：锚点、外链、mailto、target=_blank、带修饰键的点击一律放行。
 */
const KEY = "kun:nav";

export function PageTransition() {
  const pathname = usePathname();

  useEffect(() => {
    let enter = false;
    try {
      enter = sessionStorage.getItem(KEY) === "1";
      if (enter) sessionStorage.removeItem(KEY);
    } catch { /* 无痕模式忽略 */ }
    if (!enter) return;
    const b = document.body;
    b.classList.add("page-enter");
    const t = window.setTimeout(() => b.classList.remove("page-enter"), 600);
    return () => window.clearTimeout(t);
  }, [pathname]);

  useEffect(() => {
    if (window.matchMedia?.("(prefers-reduced-motion: reduce)").matches) return;

    const onClick = (e: MouseEvent) => {
      if (e.defaultPrevented || e.button !== 0) return;
      if (e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
      const a = (e.target as Element | null)?.closest?.("a") as HTMLAnchorElement | null;
      if (!a) return;
      if (a.target === "_blank" || a.hasAttribute("download")) return;
      const href = a.getAttribute("href");
      if (!href || href.startsWith("#") || href.startsWith("//")) return;
      if (/^(mailto:|tel:|javascript:)/i.test(href)) return;
      if (/^[a-z]+:/i.test(href)) return;

      const strip = (s: string) => s.replace(/[?#].*$/, "").replace(/\/+$/, "");
      if (strip(href) === strip(window.location.pathname)) return;

      e.preventDefault();
      const dest = a.href;
      let done = false;
      const go = () => {
        if (done) return;
        done = true;
        try { sessionStorage.setItem(KEY, "1"); } catch { /* ignore */ }
        window.location.assign(dest);
      };
      document.body.classList.add("page-leave");
      window.setTimeout(go, 150);
      window.setTimeout(go, 420);
    };

    document.addEventListener("click", onClick, true);
    return () => document.removeEventListener("click", onClick, true);
  }, []);

  return null;
}
