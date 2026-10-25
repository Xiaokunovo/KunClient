import { useEffect, useRef } from "react";

/**
 * 轻量视差：滚动时按 speed 系数平移元素（rAF 阻尼插值，避免抖动）。
 * 仅作用于 transform，不触发重排。speed > 0 随滚动上移。
 */
export function useParallax<T extends HTMLElement = HTMLDivElement>(speed = 0.12) {
  const ref = useRef<T>(null);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    if (window.matchMedia?.("(prefers-reduced-motion: reduce)").matches) return;

    let raf = 0;
    let current = 0;
    let target = 0;

    const measure = () => {
      const rect = el.getBoundingClientRect();
      // 以元素中心相对视口中心的偏移为基准
      const center = rect.top + rect.height / 2 - window.innerHeight / 2;
      target = -center * speed;
    };

    const loop = () => {
      current += (target - current) * 0.09; // 阻尼
      el.style.transform = `translate3d(0, ${current.toFixed(2)}px, 0)`;
      raf = requestAnimationFrame(loop);
    };

    const onScroll = () => measure();

    measure();
    raf = requestAnimationFrame(loop);
    window.addEventListener("scroll", onScroll, { passive: true });
    window.addEventListener("resize", onScroll, { passive: true });

    return () => {
      cancelAnimationFrame(raf);
      window.removeEventListener("scroll", onScroll);
      window.removeEventListener("resize", onScroll);
      el.style.transform = "";
    };
  }, [speed]);

  return ref;
}
