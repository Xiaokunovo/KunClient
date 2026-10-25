//! 极小的 JSON **输出**助手（只写不读）。
//!
//! 为什么手写而不上 serde_json：
//! 1. FFI 出口恒为「统一信封」，形状只有两种，不需要通用序列化；
//! 2. workspace 依赖策略是**零依赖**（`cargo build --offline` 必须过）；
//! 3. 体积 —— serde + serde_json 会给 dll 加几百 KB，而这个 dll 是要塞进单文件 exe 的。
//!
//! 唯一的真风险是**转义**：分区名 / `what` 字段里带引号、反斜杠、控制字符时漏转义
//! 会让 C# 侧 `JsonDocument.Parse` 抛异常（而且不一定告诉你哪个字段坏了）。
//! 所以转义逻辑单独成函数并带测试，别在业务里各写一遍。

/// 把 `s` 按 JSON 字符串规则转义后，**带引号**追加到 `out`。
///
/// 处理：`"` `\` 控制字符（`\u00XX`）；`/` 不转义（JSON 允许，人也更好读）。
/// 非 BMP 字符（emoji、部分中文扩展区）保持原样输出 —— Rust 的 `String` 已经是 UTF-8，
/// JSON 也接受直接输出 UTF-8，没必要转成代理对。
pub fn push_quoted(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0C}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// 便捷版：返回带引号的 JSON 字符串字面量。
pub fn quoted(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    push_quoted(&mut out, s);
    out
}

/// 扁平对象的增量构造器。
///
/// 设计意图：让「哪些字段漏了 / 该输出 null 还是空串」这件事在**调用点一眼可见** ——
/// `opt_str` 的 `None` 一定变成 `null`，不会悄悄变成 `""`（那会丢掉「有没有值」的信息，
/// 上游再也没法区分「明确写成空」和「压根没写」）。
pub struct Obj {
    out: String,
    first: bool,
}

impl Default for Obj {
    fn default() -> Self {
        Self::new()
    }
}

impl Obj {
    pub fn new() -> Self {
        Self { out: String::from("{"), first: true }
    }

    fn key(&mut self, k: &str) {
        if self.first {
            self.first = false;
        } else {
            self.out.push(',');
        }
        push_quoted(&mut self.out, k);
        self.out.push(':');
    }

    pub fn string(mut self, k: &str, v: &str) -> Self {
        self.key(k);
        push_quoted(&mut self.out, v);
        self
    }

    /// `None` → JSON `null`（不是空串），见类型注释。
    pub fn opt_string(mut self, k: &str, v: Option<&str>) -> Self {
        self.key(k);
        match v {
            Some(s) => push_quoted(&mut self.out, s),
            None => self.out.push_str("null"),
        }
        self
    }

    pub fn opt_u64(mut self, k: &str, v: Option<u64>) -> Self {
        self.key(k);
        match v {
            Some(n) => self.out.push_str(&n.to_string()),
            None => self.out.push_str("null"),
        }
        self
    }

    pub fn u64(mut self, k: &str, v: u64) -> Self {
        self.key(k);
        self.out.push_str(&v.to_string());
        self
    }

    pub fn u32(mut self, k: &str, v: u32) -> Self {
        self.key(k);
        self.out.push_str(&v.to_string());
        self
    }

    pub fn bool(mut self, k: &str, v: bool) -> Self {
        self.key(k);
        self.out.push_str(if v { "true" } else { "false" });
        self
    }

    pub fn null(mut self, k: &str) -> Self {
        self.key(k);
        self.out.push_str("null");
        self
    }

    /// 直接塞一段**已经是合法 JSON** 的片段（数组/嵌套对象用）。
    pub fn raw(mut self, k: &str, json_fragment: &str) -> Self {
        self.key(k);
        self.out.push_str(json_fragment);
        self
    }

    pub fn finish(mut self) -> String {
        self.out.push('}');
        self.out
    }
}

/// 把若干已序列化的片段拼成 JSON 数组。
pub fn array(items: impl IntoIterator<Item = String>) -> String {
    let mut out = String::from("[");
    for (i, item) in items.into_iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&item);
    }
    out.push(']');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn escapes_quotes_and_backslashes() {
        // 这两类漏转义 = C# 侧 JsonDocument.Parse 直接抛，且往往不知道坏在哪个字段
        assert_eq!(quoted(r#"Basic for 'gpt'"#), r#""Basic for 'gpt'""#);
        assert_eq!(quoted(r#"a"b\c"#), r#""a\"b\\c""#);
    }

    #[test]
    fn escapes_control_chars() {
        assert_eq!(quoted("a\nb\tc"), "\"a\\nb\\tc\"");
        assert_eq!(quoted("\u{01}"), "\"\\u0001\"");
    }

    #[test]
    fn keeps_utf8_as_is() {
        // 中文不转义 —— JSON 接受 UTF-8 直出，转义反而让人读不了日志
        assert_eq!(quoted("分区管理"), "\"分区管理\"");
    }

    #[test]
    fn none_becomes_null_not_empty_string() {
        let json = Obj::new().opt_string("byte_offset", None).finish();
        assert_eq!(json, r#"{"byte_offset":null}"#);
    }

    #[test]
    fn array_joins_without_trailing_comma() {
        assert_eq!(array(vec!["{}".to_string(), "{}".to_string()]), "[{},{}]");
        assert_eq!(array(Vec::<String>::new()), "[]");
    }
}
