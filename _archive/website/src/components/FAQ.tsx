import { useState } from "react";
import { Reveal } from "./3d/Reveal";
import { FAQ as FAQ_DATA, stageFaqs, type SiteInfo } from "@/lib/data";

/** 常见问题（v6）：编号 + 发丝线，默认展开第一条。 */
export function FAQ({ site }: { site: SiteInfo }) {
  const [open, setOpen] = useState<number | null>(0);
  const items = [...stageFaqs(site), ...FAQ_DATA];

  return (
    <section id="faq" className="sec">
      <div className="wrap">
        <Reveal>
          <div className="sec-head">
            <span className="eyebrow"><i>06</i>常见问题</span>
            <h2 className="sec-title">使用前先看这里</h2>
          </div>
        </Reveal>

        <Reveal delay={0.06}>
          <div className="faq-list">
            {items.map((f, i) => (
              <div className={`faq-item${open === i ? " open" : ""}`} key={f.q}>
                <button
                  type="button"
                  className="faq-q"
                  aria-expanded={open === i}
                  onClick={() => setOpen(open === i ? null : i)}
                >
                  <span className="qno">{String(i + 1).padStart(2, "0")}</span>
                  <span className="qt">{f.q}</span>
                  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round" aria-hidden="true">
                    <path d="M6 9l6 6 6-6" />
                  </svg>
                </button>
                <div className="faq-a">
                  <div>
                    <p>{f.a}</p>
                  </div>
                </div>
              </div>
            ))}
          </div>
        </Reveal>
      </div>
    </section>
  );
}
