"use client";

import { useEffect, useRef } from "react";

/**
 * 首屏 3D 作业台（v6 重写）。
 *
 * 三块窗口按真实 Z 轴错开（`translateZ` + 父级 `preserve-3d`），随指针偏航。
 * 与旧 Deck3D 的差别：
 *   · 旧版是「三块等大窗口均匀叠放」，读起来像海报；
 *   · 新版按**工作顺序**分层：最后面的固件解析（远景）、中间的分区表（工作面）、
 *     最前面的刷写进度（最近）—— 视线从右后方走到左前方，正好是一次刷机的顺序。
 *   · 角度幅度收小（±6°），并把过渡交给 CSS，指针离开后自然回正。
 *
 * 只在元素进入视口时挂 pointermove，滚出即摘 —— 长页面不必为一个首屏组件常驻监听。
 */
export function Deck() {
  const deckRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const deck = deckRef.current;
    const host = deck?.parentElement;
    if (!deck || !host) return;
    if (window.matchMedia?.("(prefers-reduced-motion: reduce)").matches) return;
    if (window.matchMedia?.("(pointer: coarse)").matches) return;

    let raf = 0;
    let tx = 0;
    let ty = 0;
    let cx = 0;
    let cy = 0;
    let active = false;
    let visible = false;

    const apply = () => {
      const ry = -14 + cx * 6;
      const rx = 8 - cy * 5;
      deck.style.setProperty("--ry", `${ry.toFixed(2)}deg`);
      deck.style.setProperty("--rx", `${rx.toFixed(2)}deg`);
    };

    const loop = () => {
      cx += (tx - cx) * 0.09;
      cy += (ty - cy) * 0.09;
      apply();
      if (Math.abs(tx - cx) < 0.002 && Math.abs(ty - cy) < 0.002) {
        raf = 0;
        return;
      }
      raf = requestAnimationFrame(loop);
    };

    const wake = () => {
      if (!raf && active && visible) raf = requestAnimationFrame(loop);
    };

    const onMove = (e: PointerEvent) => {
      const r = host.getBoundingClientRect();
      if (!r.width || !r.height) return;
      tx = Math.max(-1, Math.min(1, ((e.clientX - (r.left + r.width / 2)) / r.width) * 2));
      ty = Math.max(-1, Math.min(1, ((e.clientY - (r.top + r.height / 2)) / r.height) * 2));
      wake();
    };

    const onLeave = () => {
      tx = 0;
      ty = 0;
      wake();
    };

    const onEnter = () => {
      active = true;
      host.addEventListener("pointermove", onMove, { passive: true });
      host.addEventListener("pointerleave", onLeave, { passive: true });
    };
    const onExit = () => {
      active = false;
      host.removeEventListener("pointermove", onMove);
      host.removeEventListener("pointerleave", onLeave);
      onLeave();
    };

    let io: IntersectionObserver | null = null;
    if (typeof IntersectionObserver !== "undefined") {
      io = new IntersectionObserver(([e]) => {
        visible = e.isIntersecting;
        if (visible) onEnter();
        else onExit();
      }, { threshold: 0.05 });
      io.observe(host);
    } else {
      visible = true;
      onEnter();
    }

    return () => {
      if (raf) cancelAnimationFrame(raf);
      io?.disconnect();
      onExit();
    };
  }, []);

  return (
    <div className="deck" ref={deckRef} aria-hidden="true">
      {/* 远景：固件解析 */}
      <div className="dk dk-3">
        <div className="dk-bar">
          <span className="dk-dots"><i /><i /><i /></span>
          <span className="dk-title">固件解析</span>
        </div>
        <div className="dk-body">
          <div className="dk-row"><b>rawprogram</b><span>6 条 · OK</span></div>
          <div className="dk-row"><b>patch</b><span>12 条 · OK</span></div>
          <div className="dk-row"><b>super_4.img</b><span>4497.6 MB</span></div>
          <div className="dk-row"><b>scatter</b><span>MT6789</span></div>
        </div>
      </div>

      {/* 工作面：分区表 */}
      <div className="dk dk-2">
        <div className="dk-bar">
          <span className="dk-dots"><i /><i /><i /></span>
          <span className="dk-title">分区管理 · LUN</span>
        </div>
        <div className="dk-body">
          <div className="dk-row head"><span>LUN</span><span>分区</span><span>大小</span></div>
          <div className="dk-row"><b>0</b><span>super</span><span>5.8 GB</span></div>
          <div className="dk-row"><b>4</b><span>boot_a</span><span>96 MB</span></div>
          <div className="dk-row"><b>4</b><span>modem_a</span><span>195 MB</span></div>
          <div className="dk-row"><b>4</b><span>misc</span><span>1 MB</span></div>
        </div>
      </div>

      {/* 最近：刷写进度 */}
      <div className="dk dk-1">
        <div className="dk-bar">
          <span className="dk-dots"><i /><i /><i /></span>
          <span className="dk-title">正在刷写</span>
        </div>
        <div className="dk-body">
          <div className="dk-pl"><span>总进度</span><b>72%</b></div>
          <div className="dk-track"><i style={{ width: "72%" }} /></div>
          <div className="dk-pl"><span>super_4.img</span><b>61%</b></div>
          <div className="dk-track"><i className="green" style={{ width: "61%" }} /></div>
          <div className="dk-badge">38.4 MB/s · 剩余 04:12</div>
        </div>
      </div>
    </div>
  );
}
