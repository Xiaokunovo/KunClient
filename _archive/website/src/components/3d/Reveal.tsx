"use client";

import { useEffect, useRef, type CSSProperties, type ReactNode } from "react";

/**
 * 入场原语（v6 重写）。
 *
 * 与旧 Reveal3D 的差别：旧版是「每帧按元素在视口里的位置连续算 transform」，
 * 全站几十个块共用一个 rAF 循环 —— 滚动时每帧都要读几十个 rect。新版改成
 * **IntersectionObserver + 一次 CSS 过渡**：
 *   · 进入视口 → 加 `.in` → 浏览器自己补间（合成层上跑，不占主线程）；
 *   · 只播一次，播完断开观察，长页面滚到底也不会有常驻循环。
 *
 * 3D 感来自 CSS：初始态带 `perspective(900px) rotateX(3.5deg) translateY(18px)`，
 * 落定时转正。位移量刻意做小（3.5° / 18px）—— 这是产品页，不是幻灯片。
 *
 * 🔴 初始隐藏态写在 `.js .rv` 下：`<html class="js">` 由 layout 的内联脚本在首帧前加上。
 *    脚本被禁用时没有这个类，内容**默认可见**，不需要 noscript 兜底。
 */
export function Reveal({
  children,
  delay = 0,
  className = "",
}: {
  children: ReactNode;
  /** 秒，块内错峰 */
  delay?: number;
  className?: string;
}) {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;

    if (window.matchMedia?.("(prefers-reduced-motion: reduce)").matches) {
      el.classList.add("in");
      return;
    }
    if (typeof IntersectionObserver === "undefined") {
      el.classList.add("in");
      return;
    }

    const io = new IntersectionObserver(
      (entries) => {
        for (const e of entries) {
          if (!e.isIntersecting) continue;
          e.target.classList.add("in");
          io.disconnect();
        }
      },
      { threshold: 0.08, rootMargin: "0px 0px -8% 0px" }
    );
    io.observe(el);
    return () => io.disconnect();
  }, []);

  return (
    <div
      ref={ref}
      className={`rv${className ? " " + className : ""}`}
      style={{ "--d": `${delay}s` } as CSSProperties}
    >
      {children}
    </div>
  );
}
