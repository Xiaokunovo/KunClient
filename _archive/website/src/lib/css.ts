import type { CSSProperties } from "react";

/** 让 TS 接受自定义 CSS 变量（--x）的内联声明。 */
export function cssVars(vars: Record<string, string | number | undefined>): CSSProperties {
  return vars as CSSProperties;
}
