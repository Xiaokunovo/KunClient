"use client";

import { useEffect, useState } from "react";

/** 返回顶部：滚过一屏后淡入。 */
export function ToTop() {
  const [show, setShow] = useState(false);

  useEffect(() => {
    const measure = () => setShow(window.scrollY > 620);
    measure();
    window.addEventListener("scroll", measure, { passive: true });
    return () => window.removeEventListener("scroll", measure);
  }, []);

  return (
    <button
      type="button"
      className={`totop${show ? " on" : ""}`}
      aria-label="返回顶部"
      onClick={() => window.scrollTo({ top: 0, behavior: "smooth" })}
    >
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <path d="M12 19V5" />
        <path d="m5 12 7-7 7 7" />
      </svg>
    </button>
  );
}
