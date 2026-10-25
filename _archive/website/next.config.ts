import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  output: "export",
  // 法规页需要 /privacy/ 这类「目录形式」的可访问路径：
  // 导出的产物是 out/privacy/index.html，server.js 用 express.static 托管时
  // 默认只会为目录找 index.html，不会把 /privacy 映射到 privacy.html。
  // 不开这个开关就会 404。
  trailingSlash: true,
  images: {
    unoptimized: true,
  },
};

export default nextConfig;
