import { Reveal } from "./3d/Reveal";
import { CAPS } from "@/lib/data";

/**
 * 能力速览（v6）。
 *
 * 六格「编号 / 协议名 / 一句话」。与 v5 的差别：v5 只有两行字（协议 + 标题），
 * 扫一眼知道有这回事，但不知道它到底解决什么问题；这一版每格补一句具体说明，
 * 且整块用发丝线切成表格状 —— 与下一节的三块模块形成「速览 → 全景」的层级。
 */
export function Capabilities() {
  return (
    <section className="sec sec-soft" id="caps">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>01</i>能力速览</span>
            <h2 className="sec-title">十七个页面，收进一个窗口</h2>
            <p className="sec-desc">
              从最底层的 9008 写入，到最表层的投屏操作 —— 整条链路收在同一个界面里，
              不用在几个工具之间来回倒固件。
            </p>
          </div>
        </Reveal>

        <Reveal delay={0.08}>
          <div className="caps-grid">
            {CAPS.map((c) => (
              <article className="cap" key={c.no}>
                <div className="cap-top">
                  <span className="cap-proto">{c.proto}</span>
                  <span className="cap-no">{c.no}</span>
                </div>
                <h3>{c.title}</h3>
                <p>{c.desc}</p>
              </article>
            ))}
          </div>
        </Reveal>
      </div>
    </section>
  );
}
