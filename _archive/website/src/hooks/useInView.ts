import { useEffect, useRef, useState } from "react";

/**
 * 元素进入视口时返回 visible=true（只触发一次后断开观察者）。
 * 采用 IntersectionObserver，配合 CSS 的 .reveal 过渡实现滚动渐显。
 */
export function useInView<T extends HTMLElement = HTMLDivElement>(threshold = 0.16, rootMargin = "0px 0px -8% 0px") {
  const ref = useRef<T>(null);
  const [visible, setVisible] = useState(false);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;

    // 不支持 IntersectionObserver 时直接可见
    if (typeof IntersectionObserver === "undefined") {
      setVisible(true);
      return;
    }

    // 首屏已在视口内（含带 #锚点直接进入、浏览器恢复滚动位置）时同步显示，
    // 避免这一帧仍是隐藏态。
    const r = el.getBoundingClientRect();
    if (r.top < window.innerHeight && r.bottom > 0) {
      setVisible(true);
      return;
    }

    const obs = new IntersectionObserver(
      ([entry]) => {
        if (entry.isIntersecting) {
          setVisible(true);
          obs.disconnect();
        }
      },
      { threshold, rootMargin }
    );
    obs.observe(el);
    return () => obs.disconnect();
  }, [threshold, rootMargin]);

  return { ref, visible };
}
