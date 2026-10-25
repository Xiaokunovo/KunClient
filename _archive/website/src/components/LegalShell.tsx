import { ADMIN_EMAIL } from "@/lib/data";

/**
 * 法务页（隐私政策 / 用户协议 / 免责声明）的公共外壳（v6）。
 *
 * 首页是「仪器面板」，法务页则退回阅读器：760px 行宽、1.9 行高、发丝线分节。
 * 顶栏保留品牌与三份条款的互链，正文只负责好读 —— 这类页面不需要 3D、不需要动效。
 */
export const LEGAL_LINKS = [
  { href: "/privacy/", label: "隐私政策" },
  { href: "/terms/", label: "用户协议" },
  { href: "/disclaimer/", label: "免责声明" },
];

export function LegalShell({
  title,
  updated,
  children,
}: {
  title: string;
  updated: string;
  children: React.ReactNode;
}) {
  return (
    <>
      <header className="legal-top">
        <div className="wrap legal-top-inner">
          <a href="/" className="brand">
            <img className="brand-mark" src="/logo.png" alt="" width={26} height={26} />
            <span className="brand-text">Kun<em>Client</em></span>
          </a>
          <nav className="legal-top-links">
            {LEGAL_LINKS.map((l) => (
              <a key={l.href} href={l.href}>{l.label}</a>
            ))}
            <a href="/">返回首页</a>
          </nav>
        </div>
      </header>

      <main className="legal-main">
        <div className="wrap">
          <div className="legal-head">
            <span className="eyebrow">法律条款</span>
            <h1 className="sec-title">{title}</h1>
            <p className="sec-desc">最后更新：{updated}　·　生效日期：{updated}</p>
          </div>

          <article className="legal-body">{children}</article>

          <div className="legal-foot">
            <p>
              运营主体：个人开发者「小坤ovo」　·　联系方式：
              <a href={`mailto:${ADMIN_EMAIL}`}>{ADMIN_EMAIL}</a>
            </p>
            <p>
              {LEGAL_LINKS.map((l, i) => (
                <span key={l.href}>
                  {i > 0 ? "　·　" : ""}
                  <a href={l.href}>{l.label}</a>
                </span>
              ))}
              {"　·　"}
              <a href="/">返回首页</a>
            </p>
          </div>
        </div>
      </main>
    </>
  );
}
