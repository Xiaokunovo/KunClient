import { Reveal } from "./3d/Reveal";
import {
  STEPS, CONSOLE_URL, DOWNLOAD_CLOSED_TITLE, DOWNLOAD_CLOSED_DESC,
  type VersionInfo, type SiteInfo,
} from "@/lib/data";

interface DownloadProps {
  info: VersionInfo;
  site: SiteInfo;
}

/**
 * 下载安装（v6）。
 *
 * 左侧是三步流程 + 一条「遇到问题」，右侧是深色发布卡（全站仅有的两处深色之一，
 * 另一处是首屏作业台）。发布卡承担版本号、两个下载入口、更新日志与合规声明 ——
 * 所有与「拿包」有关的信息都在一张卡里，不用上下找。
 *
 * 7z 链接由 exe 地址派生（换版本号不会 stale）；官网下载关闭时整块只留内测说明。
 */
export function Download({ info, site }: DownloadProps) {
  const zipUrl = info.downloadUrl.replace(/\.exe$/, ".7z");
  const open = site.downloadEnabled !== false;

  if (!open) {
    return (
      <section id="download" className="sec sec-soft">
        <div className="wrap">
          <Reveal>
            <div className="sec-head">
              <span className="eyebrow"><i>05</i>下载安装</span>
              <h2 className="sec-title">{DOWNLOAD_CLOSED_TITLE}</h2>
              <p className="sec-desc">{DOWNLOAD_CLOSED_DESC}</p>
            </div>
          </Reveal>
        </div>
      </section>
    );
  }

  return (
    <section id="download" className="sec sec-soft">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>05</i>下载安装</span>
            <h2 className="sec-title">三步开始使用</h2>
            <p className="sec-desc">
              程序为单文件发布，不做安装写入；请始终使用最新版本以获得完整功能与登录支持。
            </p>
          </div>
        </Reveal>

        <div className="dl">
          <Reveal delay={0.06}>
            <div className="steps">
              {STEPS.map((s, i) => (
                <div className="step" key={s.title}>
                  <span className="step-num">{i + 1}</span>
                  <div>
                    <h3>{s.title}</h3>
                    <p>
                      {s.desc}
                      {i === 1 && (
                        <> <a href={CONSOLE_URL}>前往控制台注册账号</a></>
                      )}
                    </p>
                  </div>
                </div>
              ))}
              <div className="step step-warn">
                <span className="step-num">!</span>
                <div>
                  <h3>遇到问题？</h3>
                  <p>
                    登录失败或提示版本过低，在本页重新下载最新版本即可；
                    使用中的疑问可在控制台反馈，或邮件联系站长。
                  </p>
                </div>
              </div>
            </div>
          </Reveal>

          <Reveal delay={0.12}>
            <div className="release">
              <div className="release-top">
                <div className="release-ver">
                  <b>v{info.version}</b>
                  {info.forceUpdate && <span>推荐更新</span>}
                </div>
                <div className="release-meta">
                  {info.buildDate ? `发布日期：${info.buildDate}` : "发布时间以服务器为准"}
                </div>
                <a href={info.downloadUrl} className="btn btn-primary btn-block" download>
                  下载 KunClient.exe
                  <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
                    <path d="M12 4v11m0 0l-4-4m4 4l4-4M4 20h16" />
                  </svg>
                </a>
                <p className="release-alt">
                  体积敏感？<a href={zipUrl} download>下载 7z 压缩包</a>（约 10.6 MB，解压后与上方完全一致）。
                </p>
                <p className="release-legal">
                  仅限对本人合法拥有、或已获得所有者明确授权的设备使用；
                  请勿用于来历不明、遗失或被盗的设备，或用于绕开设备的防盗与账号保护机制。
                </p>
              </div>

              <div className="release-body">
                <div className="log-title">更新内容</div>
                <pre className="log">{info.changelog?.trim() || "暂无更新说明。"}</pre>
              </div>
            </div>
          </Reveal>
        </div>
      </div>
    </section>
  );
}
