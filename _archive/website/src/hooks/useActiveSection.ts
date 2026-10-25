import { useEffect, useState } from "react";

/**
 * 导航高亮（v6 重写）。
 *
 * 旧实现用 IntersectionObserver 的 intersectionRatio 选「最可见」的区段。
 * 在长区块（会员权益那张两千像素高的表）上并不可靠：观察带只有视口 20% 高，
 * 区块的 ratio 恒小于最小阈值 0.08，回调不再触发 → 高亮停在上一节
 * （实测：滚到「会员权益」时导航还亮着「下载安装」）。
 *
 * 这一版改成**滚动位置判定**：取「顶部已越过参考线」的最后一个区块。
 * 长区块天然正确，也不用维护阈值数组。
 */
export function useActiveSection(ids: string[], offset = 0.35) {
  const [active, setActive] = useState<string>("");

  useEffect(() => {
    if (typeof window === "undefined") return;

    let raf = 0;
    const calc = () => {
      raf = 0;
      // 参考线：视口 35% 处（加固定导航高度），越过它的最后一个区块即当前区块
      const line = window.innerHeight * offset + 64;
      let cur = "";
      for (const id of ids) {
        const el = document.getElementById(id);
        if (!el) continue;
        if (el.getBoundingClientRect().top <= line) cur = id;
      }
      setActive(cur);
    };
    const onScroll = () => {
      if (!raf) raf = requestAnimationFrame(calc);
    };

    calc();
    window.addEventListener("scroll", onScroll, { passive: true });
    window.addEventListener("resize", onScroll, { passive: true });
    return () => {
      if (raf) cancelAnimationFrame(raf);
      window.removeEventListener("scroll", onScroll);
      window.removeEventListener("resize", onScroll);
    };
  }, [ids, offset]);

  return active;
}
