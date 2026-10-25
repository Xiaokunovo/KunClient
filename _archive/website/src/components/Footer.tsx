import { LEGAL_LINKS } from "./LegalShell";
import { NAV_LINKS, CONSOLE_URL, ADMIN_URL, ADMIN_EMAIL, type VersionInfo, type SiteInfo } from "@/lib/data";

interface FooterProps {
  info: VersionInfo;
  site: SiteInfo;
}

/**
 * 页脚（v6）：深色收口。
 * 整站是纸白，页脚用一块深色 + 网格底纹把页面「合上」—— 深色只在这一个位置，
 * 与首屏作业台、下载发布卡构成三处同源的仪器面板。
 */
export function Footer({ info, site }: FooterProps) {
  const year = new Date().getFullYear();
  const open = site.downloadEnabled !== false;

  return (
    <footer className="footer">
      <div className="wrap footer-wrap">
        <div className="footer-mark" aria-hidden="true">Kun<em>Client</em></div>

        <div className="footer-top">
          <a href="/" className="brand" aria-label="KunClient 首页">
            <img className="brand-mark" src="/logo.png" alt="" width={26} height={26} />
            <span className="brand-text">Kun<em>Client</em></span>
          </a>

          <nav className="footer-links">
            {NAV_LINKS.map((l) => (
              <a key={l.href} href={l.href}>{l.label}</a>
            ))}
            <a href={CONSOLE_URL}>控制台</a>
            <a href={ADMIN_URL}>管理后台</a>
            <a href={`mailto:${ADMIN_EMAIL}`}>邮箱联系</a>
          </nav>
        </div>

        <div className="footer-top footer-legal-row">
          <span className="footer-legal-title">法律与合规</span>
          <nav className="footer-links">
            {LEGAL_LINKS.map((l) => (
              <a key={l.href} href={l.href}>{l.label}</a>
            ))}
          </nav>
        </div>

        <div className="footer-bottom">
          <p className="footer-note">
            本工具仅供学习交流使用，仅限对本人合法拥有、或已获得所有者明确授权的设备使用，
            严禁用于来历不明、遗失或被盗的设备，严禁用于绕开设备的防盗与账号保护机制。刷机存在风险，
            可能导致数据丢失或设备无法开机，请在操作前备份重要数据并自行承担相应后果。
            详见<a href="/disclaimer/">《免责声明》</a>、<a href="/terms/">《用户协议》</a>与
            <a href="/privacy/">《隐私政策》</a>。
          </p>
          <p className="footer-copy">
            © {year} KunClient{open ? ` · v${info.version}` : ""}
          </p>
        </div>
      </div>
    </footer>
  );
}
