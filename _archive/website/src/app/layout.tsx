import type { Metadata, Viewport } from "next";
import "./globals.css";
import { ScrollProgress } from "@/components/ScrollProgress";
import { ToTop } from "@/components/ToTop";
import { PageTransition } from "@/components/PageTransition";

export const metadata: Metadata = {
  title: "KunClient - 高通 / 联发科刷机工具",
  description: "KunClient 是面向高通与联发科设备的刷机工具，支持 ADB / Fastboot / EDL 9008 / MTK BROM，欧加与小米线刷、可视分区管理与投屏控制。",
  keywords: ["KunClient", "刷机工具", "高通刷机", "联发科刷机", "MTK", "EDL", "9008", "Fastboot", "OFP", "救砖"],
  icons: { icon: "/favicon.ico" },
  openGraph: {
    title: "KunClient - 高通 / 联发科刷机工具",
    description: "支持 ADB / Fastboot / EDL / MTK BROM 多模式刷写，全部功能免费，无需会员或卡密。",
    type: "website",
  },
};

export const viewport: Viewport = {
  width: "device-width",
  initialScale: 1,
  themeColor: "#ffffff",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN" suppressHydrationWarning>
      <body>
        {/* 🔴 入场动画的初始隐藏态写在 `.js .rv` 下，这个类必须**在首帧之前**加上：
            否则会先画出完整内容、再瞬间消失、再淡入（白闪一下）。
            脚本被禁用时没有 .js，内容默认可见 —— 不需要 noscript 兜底。 */}
        <script
          dangerouslySetInnerHTML={{ __html: "document.documentElement.classList.add('js')" }}
        />
        <ScrollProgress />
        <PageTransition />
        {children}
        <ToTop />
      </body>
    </html>
  );
}
