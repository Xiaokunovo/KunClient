import { useEffect, useRef, useState } from "react";

/**
 * 数字滚动：元素进入视口后，从 0 缓动到目标值（指数缓出，观感丝滑）。
 * 仅用 rAF，无第三方依赖。返回值随帧更新，用于渲染。
 */
export function useCountUp(target: number, duration = 1700, decimals = 0) {
  const ref = useRef<HTMLDivElement>(null);
  const [value, setValue] = useState(0);
  const started = useRef(false);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;

    const reduced =
      typeof window !== "undefined" &&
      window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;

    const run = () => {
      if (started.current) return;
      started.current = true;

      if (reduced) {
        setValue(target);
        return;
      }

      const t0 = performance.now();
      const tick = (now: number) => {
        const p = Math.min(1, (now - t0) / duration);
        // 指数缓出（与 CSS --ease-silk 同调）
        const eased = p === 1 ? 1 : 1 - Math.pow(2, -10 * p);
        setValue(Number((target * eased).toFixed(decimals)));
        if (p < 1) requestAnimationFrame(tick);
        else setValue(target);
      };
      requestAnimationFrame(tick);
    };

    if (typeof IntersectionObserver === "undefined") {
      run();
      return;
    }
    const obs = new IntersectionObserver(
      ([entry]) => {
        if (entry.isIntersecting) {
          run();
          obs.disconnect();
        }
      },
      { threshold: 0.35 }
    );
    obs.observe(el);
    return () => obs.disconnect();
  }, [target, duration, decimals]);

  return { ref, value };
}
