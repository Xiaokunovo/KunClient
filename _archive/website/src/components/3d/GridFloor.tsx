"use client";

import { useEffect, useRef } from "react";

/**
 * 首屏的透视网格地面（Canvas 2D，v6 新增）。
 *
 * 为什么要重画而不是复用旧 WireGrid / ParticleField：
 *   · 旧版是「静态 SVG 网格 + 粒子星座」，和产品没有关系，只是背景装饰；
 *   · 这一版画的是**仪器刻度**：向灭点收束的纵向刻度 + 按透视压缩的横向刻度，
 *     外加三颗沿刻度线向观察者移动的数据点 —— 读起来像一台正在扫描的设备，
 *     与刷机这件事同构。
 *
 * 性能约定：
 *   · 只在可见时跑 rAF（IntersectionObserver），页面切走时停；
 *   · DPR 封顶 2，避免 4K 屏上按 3 倍分辨率白烧 GPU；
 *   · prefers-reduced-motion 只画一帧静态网格，不启动循环。
 */
export function GridFloor({ className = "" }: { className?: string }) {
  const wrapRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const wrap = wrapRef.current;
    if (!wrap) return;
    const canvas = wrap.querySelector("canvas");
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const reduce = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches ?? false;

    let w = 0;
    let h = 0;
    let dpr = 1;
    let raf = 0;
    let running = false;
    let t = 0;

    const resize = () => {
      const r = wrap.getBoundingClientRect();
      if (!r.width || !r.height) return;
      dpr = Math.min(2, window.devicePixelRatio || 1);
      w = r.width;
      h = r.height;
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
      canvas.style.width = w + "px";
      canvas.style.height = h + "px";
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    };

    const draw = () => {
      if (!w || !h) return;
      ctx.clearRect(0, 0, w, h);

      const vx = w * 0.52;
      const vy = h * 0.26;
      const bottom = h * 1.02;

      // 纵向刻度：从灭点散开，越靠边越淡
      const cols = 15;
      for (let i = -cols; i <= cols; i++) {
        const k = i / cols;
        const x = vx + k * w * 1.15;
        const a = 0.16 * (1 - Math.abs(k) * 0.72);
        if (a <= 0.004) continue;
        const g = ctx.createLinearGradient(vx, vy, x, bottom);
        g.addColorStop(0, `rgba(0, 113, 227, 0)`);
        g.addColorStop(0.45, `rgba(0, 113, 227, ${a * 0.55})`);
        g.addColorStop(1, `rgba(0, 113, 227, ${a})`);
        ctx.strokeStyle = g;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(vx, vy);
        ctx.lineTo(x, bottom);
        ctx.stroke();
      }

      // 横向刻度：按 1/z 压缩，向下滚动（t 推进）
      const rows = 16;
      for (let i = 0; i < rows; i++) {
        const p = ((i / rows + t) % 1);
        const z = 1 - p;
        const y = vy + (bottom - vy) * Math.pow(z, 2.35);
        const a = 0.03 + 0.2 * (1 - z);
        ctx.strokeStyle = `rgba(11, 13, 17, ${a})`;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
      }

      // 灭点横向地平线
      ctx.strokeStyle = "rgba(11, 13, 17, 0.14)";
      ctx.beginPath();
      ctx.moveTo(0, vy);
      ctx.lineTo(w, vy);
      ctx.stroke();

      // 三颗沿刻度线移动的数据点
      const lanes = [-0.55, 0.08, 0.46];
      for (let i = 0; i < lanes.length; i++) {
        const k = lanes[i];
        const p = ((t * 1.35 + i * 0.31) % 1);
        const z = 1 - p;
        const y = vy + (bottom - vy) * Math.pow(z, 2.35);
        const x = vx + k * w * 1.15 * (y - vy) / (bottom - vy);
        const r = 1.2 + 2.2 * p;
        ctx.fillStyle = `rgba(0, 113, 227, ${0.12 + 0.5 * p})`;
        ctx.beginPath();
        ctx.arc(x, y, r, 0, Math.PI * 2);
        ctx.fill();
      }
    };

    const loop = () => {
      if (!running) return;
      t = (t + 0.0016) % 1;
      draw();
      raf = requestAnimationFrame(loop);
    };

    const start = () => {
      if (reduce || running) return;
      running = true;
      raf = requestAnimationFrame(loop);
    };
    const stop = () => {
      running = false;
      cancelAnimationFrame(raf);
      raf = 0;
    };

    resize();
    draw();

    let io: IntersectionObserver | null = null;
    if (!reduce && typeof IntersectionObserver !== "undefined") {
      io = new IntersectionObserver(
        ([e]) => (e.isIntersecting ? start() : stop()),
        { threshold: 0 }
      );
      io.observe(wrap);
    }

    const onResize = () => { resize(); draw(); };
    const onVis = () => { if (document.hidden) stop(); else if (io) start(); };
    window.addEventListener("resize", onResize, { passive: true });
    document.addEventListener("visibilitychange", onVis);

    return () => {
      stop();
      io?.disconnect();
      window.removeEventListener("resize", onResize);
      document.removeEventListener("visibilitychange", onVis);
    };
  }, []);

  return (
    <div ref={wrapRef} className={className} aria-hidden="true">
      <canvas />
    </div>
  );
}
