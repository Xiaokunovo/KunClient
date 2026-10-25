//! QFIL 风格 `rawprogram*.xml` / `patch*.xml` 的属性扫描。
//!
//! 这些文件是**扁平的**：根下一堆同名子元素，每个子元素靠属性描述一个动作。
//! 所以不需要完整的 XML 解析器 —— 一个「找标签 + 收属性」的扫描器就够，
//! 也就顺带满足了 workspace 的零依赖策略。
//!
//! 🔴 三个静默出错的坑，都在这里挡住（对应 C# 侧 `QfilXmlParser` 的既有口径）：
//!
//! 1. **`SECTOR_SIZE_IN_BYTES` 只认全大写**。写成小写会被**静默忽略**并退回 512，
//!    不报任何错 —— 后面所有偏移直接差 8 倍。这条不是我们的怪癖，是 QFIL 的行为，
//!    所以 C# 侧、Rust 侧都保持一致（有测试钉住）。
//! 2. **`start_sector` 的「0 的歧义」**：`NUM_DISK_SECTORS-33` 这种表达式算不出数值，
//!    但 `start_sector="0"` 是真的要写扇区 0。若只传数字，两者都是 0 ——
//!    上层就会把 GPT 表往扇区 0 写。所以出口带 `start_sector_is_literal` 与 `start_sector_expr`。
//! 3. **缺省字段用 `null`，不用空串**：丢掉「有没有值」这个信息后，
//!    上层再也分不清「明确写成空」和「压根没写」。
//!
//! 对应关系（务必与 C# `QfilXmlParser` 保持同步）：
//! `<patches><patch …>` → [`PatchItem`]；`<data><program …>` → [`RawProgramItem`]。

use crate::json::{Obj, array};

/// 扫描出来的一个元素（标签名 + 属性表，按出现顺序）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Element {
    pub name: String,
    pub attrs: Vec<(String, String)>,
}

impl Element {
    pub fn attr(&self, name: &str) -> Option<&str> {
        self.attrs
            .iter()
            .find(|(k, _)| k == name)
            .map(|(_, v)| v.as_str())
    }

    /// 取属性并解析成 u64；缺失或解析失败都返回 `None`（调用方决定回落值）。
    pub fn attr_u64(&self, name: &str) -> Option<u64> {
        self.attr(name)?.trim().parse::<u64>().ok()
    }

    pub fn attr_u32(&self, name: &str) -> Option<u32> {
        self.attr(name)?.trim().parse::<u32>().ok()
    }

    /// 布尔属性：只有明确写成 `true`/`1` 才算真（缺省 = 假）。
    pub fn attr_bool(&self, name: &str) -> bool {
        matches!(self.attr(name).map(str::trim), Some("true") | Some("1") | Some("TRUE"))
    }
}

/// 扫描出所有名为 `tag` 的元素。文档明显不合法（标签未闭合等）时返回 `Err`。
///
/// 只做「够用」的健壮性检查：属性值引号必须闭合、标签必须有 `>`。
/// 刻意不去实现完整的 well-formed 校验 —— 上游给的是厂商生成的 XML，不是攻击面。
pub fn elements(xml: &str, tag: &str) -> Result<Vec<Element>, String> {
    let bytes = xml.as_bytes();
    let mut out = Vec::new();
    let mut i = 0usize;

    while let Some(lt) = xml[i..].find('<') {
        let lt = i + lt;
        // '<' 之后
        let mut j = lt + 1;
        if j >= bytes.len() {
            return Err("XML 在 '<' 处意外结束".to_string());
        }

        // 注释 / 声明 / CDATA：整段跳过
        match bytes[j] {
            b'!' => {
                if xml[j..].starts_with("<!--") {
                    match xml[j..].find("-->") {
                        Some(k) => {
                            i = j + k + 3;
                            continue;
                        }
                        None => return Err("注释未闭合".to_string()),
                    }
                }
                i = skip_to_gt(xml, j)?;
                continue;
            }
            b'?' => {
                i = skip_to_gt(xml, j)?;
                continue;
            }
            b'/' => {
                i = skip_to_gt(xml, j)?;
                continue;
            }
            _ => {}
        }

        // 标签名
        let name_start = j;
        while j < bytes.len() && !is_name_end(bytes[j]) {
            j += 1;
        }
        let name = &xml[name_start..j];
        if name.is_empty() {
            return Err(format!("标签名缺失（偏移 {lt}）"));
        }

        // 属性
        let mut attrs = Vec::new();
        loop {
            while j < bytes.len() && bytes[j].is_ascii_whitespace() {
                j += 1;
            }
            if j >= bytes.len() {
                return Err(format!("标签 <{name}> 未闭合"));
            }
            match bytes[j] {
                b'>' => {
                    j += 1;
                    break;
                }
                b'/' => {
                    // 自闭合：后面必须跟 '>'
                    if j + 1 < bytes.len() && bytes[j + 1] == b'>' {
                        j += 2;
                        break;
                    }
                    return Err(format!("标签 <{name}> 里的 '/' 后缺少 '>'"));
                }
                _ => {}
            }

            // 属性名
            let k_start = j;
            while j < bytes.len() && !is_attr_name_end(bytes[j]) {
                j += 1;
            }
            if j == k_start {
                return Err(format!("标签 <{name}> 处出现无法识别的字符"));
            }
            let k = &xml[k_start..j];

            // '='
            while j < bytes.len() && bytes[j].is_ascii_whitespace() {
                j += 1;
            }
            if j >= bytes.len() || bytes[j] != b'=' {
                return Err(format!("标签 <{name}> 的属性 {k} 缺少 '='"));
            }
            j += 1;
            while j < bytes.len() && bytes[j].is_ascii_whitespace() {
                j += 1;
            }
            if j >= bytes.len() {
                return Err(format!("标签 <{name}> 的属性 {k} 缺少取值"));
            }

            // 取值：单引号或双引号
            let quote = bytes[j];
            if quote != b'"' && quote != b'\'' {
                return Err(format!("标签 <{name}> 的属性 {k} 取值未加引号"));
            }
            j += 1;
            let v_start = j;
            while j < bytes.len() && bytes[j] != quote {
                j += 1;
            }
            if j >= bytes.len() {
                return Err(format!("标签 <{name}> 的属性 {k} 引号未闭合"));
            }
            let raw = &xml[v_start..j];
            j += 1;

            if name == tag {
                attrs.push((k.to_string(), decode_entities(raw)));
            }
        }

        if name == tag {
            out.push(Element { name: name.to_string(), attrs });
        }
        i = j;
    }

    Ok(out)
}

fn skip_to_gt(xml: &str, from: usize) -> Result<usize, String> {
    match xml[from..].find('>') {
        Some(k) => Ok(from + k + 1),
        None => Err("标签未闭合（找不到 '>'）".to_string()),
    }
}

fn is_name_end(b: u8) -> bool {
    b.is_ascii_whitespace() || b == b'>' || b == b'/'
}

fn is_attr_name_end(b: u8) -> bool {
    b.is_ascii_whitespace() || b == b'=' || b == b'/' || b == b'>'
}

/// 解开 XML 预定义实体与数字引用（`&#48;` / `&#x30;`）。
///
/// 厂商 XML 里 `&amp;` 很常见（例如 `label="a&amp;b"`）。不解会把 `&amp;` 原样带过界，
/// 上层当成字面量 —— 分区名对不上，而且不报错。
fn decode_entities(s: &str) -> String {
    if !s.contains('&') {
        return s.to_string();
    }
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(pos) = rest.find('&') {
        out.push_str(&rest[..pos]);
        let tail = &rest[pos..];
        let Some(semi) = tail.find(';') else {
            out.push_str(tail);
            return out;
        };
        let entity = &tail[1..semi];
        let decoded = match entity {
            "amp" => Some('&'),
            "lt" => Some('<'),
            "gt" => Some('>'),
            "quot" => Some('"'),
            "apos" => Some('\''),
            _ => entity
                .strip_prefix('#')
                .and_then(|num| {
                    if let Some(hex) = num.strip_prefix(['x', 'X']) {
                        u32::from_str_radix(hex, 16).ok()
                    } else {
                        num.parse::<u32>().ok()
                    }
                })
                .and_then(char::from_u32),
        };
        match decoded {
            Some(c) => {
                out.push(c);
                rest = &tail[semi + 1..];
            }
            None => {
                // 不是已知实体：原样保留（别把用户数据吃掉）
                out.push('&');
                rest = &tail[1..];
            }
        }
    }
    out.push_str(rest);
    out
}

// ───────────────────────────── patch.xml ─────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchItem {
    pub filename: String,
    pub start_sector: Option<String>,
    pub size_in_bytes: u64,
    pub what: Option<String>,
    pub value: Option<String>,
    pub offset: Option<String>,
    pub begin_blocks: Option<String>,
    pub physical_partition_number: Option<String>,
    pub id: Option<String>,
    /// ⚠️ **不是** `offset` 的数值形式。
    /// 契约上它只从 `byte_offset` 属性取，而 QFIL 生成的 patch.xml 从不写这个属性
    /// —— 所以出口恒为 `null`（`ffi_probe` 有断言钉住这一点：带 `offset="48"` 的条目
    /// 其 `byte_offset` 仍必须是 null）。别「顺手」把它接上 `offset`。
    pub byte_offset: Option<u64>,
}

impl PatchItem {
    pub fn to_json(&self) -> String {
        Obj::new()
            .string("filename", &self.filename)
            .opt_string("start_sector", self.start_sector.as_deref())
            .u64("size_in_bytes", self.size_in_bytes)
            .opt_string("what", self.what.as_deref())
            .opt_string("value", self.value.as_deref())
            .opt_string("offset", self.offset.as_deref())
            .opt_string("begin_blocks", self.begin_blocks.as_deref())
            .opt_string("physical_partition_number", self.physical_partition_number.as_deref())
            .opt_string("id", self.id.as_deref())
            .opt_u64("byte_offset", self.byte_offset)
            .finish()
    }
}

pub fn parse_patch(xml: &str) -> Result<Vec<PatchItem>, String> {
    Ok(elements(xml, "patch")?
        .into_iter()
        .map(|e| PatchItem {
            filename: e.attr("filename").unwrap_or("").to_string(),
            start_sector: e.attr("start_sector").map(str::to_string),
            size_in_bytes: e.attr_u64("size_in_bytes").unwrap_or(0),
            what: e.attr("what").map(str::to_string),
            value: e.attr("value").map(str::to_string),
            offset: e.attr("offset").map(str::to_string),
            begin_blocks: e.attr("begin_blocks").map(str::to_string),
            physical_partition_number: e.attr("physical_partition_number").map(str::to_string),
            id: e.attr("id").map(str::to_string),
            byte_offset: e.attr_u64("byte_offset"),
        })
        .collect())
}

// ─────────────────────────── rawprogram.xml ───────────────────────────

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawProgramItem {
    pub filename: String,
    pub label: Option<String>,
    pub physical_partition_number: Option<String>,
    pub start_sector: u64,
    pub start_sector_is_literal: bool,
    pub start_sector_expr: Option<String>,
    pub num_partition_sectors: u64,
    pub sector_size_in_bytes: u32,
    pub is_sparse: bool,
}

impl RawProgramItem {
    pub fn to_json(&self) -> String {
        Obj::new()
            .string("filename", &self.filename)
            .opt_string("label", self.label.as_deref())
            .opt_string("physical_partition_number", self.physical_partition_number.as_deref())
            .u64("start_sector", self.start_sector)
            .bool("start_sector_is_literal", self.start_sector_is_literal)
            .opt_string("start_sector_expr", self.start_sector_expr.as_deref())
            .u64("num_partition_sectors", self.num_partition_sectors)
            .u32("sector_size_in_bytes", self.sector_size_in_bytes)
            .bool("is_sparse", self.is_sparse)
            .finish()
    }
}

pub fn parse_rawprogram(xml: &str) -> Result<Vec<RawProgramItem>, String> {
    Ok(elements(xml, "program")?
        .into_iter()
        .map(|e| {
            let raw_start = e.attr("start_sector");
            // 「0 的歧义」：能解析成纯数字才算字面量，表达式一律 0 + is_literal=false
            let literal = raw_start.and_then(|s| s.trim().parse::<u64>().ok());
            RawProgramItem {
                filename: e.attr("filename").unwrap_or("").to_string(),
                label: e.attr("label").map(str::to_string),
                physical_partition_number: e.attr("physical_partition_number").map(str::to_string),
                start_sector: literal.unwrap_or(0),
                start_sector_is_literal: literal.is_some(),
                start_sector_expr: raw_start.map(str::to_string),
                num_partition_sectors: e.attr_u64("num_partition_sectors").unwrap_or(0),
                // ⚠️ 只认全大写；小写被静默忽略 → 512
                sector_size_in_bytes: e.attr_u32("SECTOR_SIZE_IN_BYTES").unwrap_or(512),
                is_sparse: e.attr_bool("sparse"),
            }
        })
        .collect())
}

/// 把条目列表序列化成 FFI 出口要的 JSON 数组。
pub fn patches_to_json_array(items: &[PatchItem]) -> String {
    array(items.iter().map(PatchItem::to_json))
}

pub fn rawprogram_to_json_array(items: &[RawProgramItem]) -> String {
    array(items.iter().map(RawProgramItem::to_json))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::quoted;

    const PATCH_XML: &str = r#"<?xml version="1.0" ?>
<patches>
  <patch filename="gpt_main0.bin" start_sector="1" size_in_bytes="4096"
         what="Update last partition 13 'userdata' with actual size in Primary Header."
         value="NUM_DISK_SECTORS-33" offset="48" physical_partition_number="0" />
  <patch filename="DISK" start_sector="0" size_in_bytes="8"
         what="Zero out BackupGPT header" offset="0" physical_partition_number="0" />
</patches>"#;

    const RAW_XML: &str = r#"<?xml version="1.0" ?>
<data>
  <program filename="x.bin" label="Basic for 'gpt'" physical_partition_number="0"
           start_sector="NUM_DISK_SECTORS-33" num_partition_sectors="6"
           SECTOR_SIZE_IN_BYTES="4096" />
  <program filename="y.bin" label="y" physical_partition_number="0"
           start_sector="2048" num_partition_sectors="10"
           SECTOR_SIZE_IN_BYTES="4096" />
  <program filename="z.bin" label="z" physical_partition_number="0"
           start_sector="0" num_partition_sectors="1"
           SECTOR_SIZE_IN_BYTES="4096" />
</data>"#;

    #[test]
    fn patch_count_and_fields() {
        let items = parse_patch(PATCH_XML).expect("应能解析");
        assert_eq!(items.len(), 2);
        assert_eq!(items[0].size_in_bytes, 4096);
        assert_eq!(items[0].physical_partition_number.as_deref(), Some("0"));
        assert_eq!(items[0].offset.as_deref(), Some("48"));
        assert_eq!(
            items[0].what.as_deref(),
            Some("Update last partition 13 'userdata' with actual size in Primary Header.")
        );
        // 契约：byte_offset 不从 offset 推导 —— 恒为 null
        assert_eq!(items[0].byte_offset, None);
        assert!(items[0].to_json().contains(r#""byte_offset":null"#));
    }

    #[test]
    fn rawprogram_resolves_zero_ambiguity() {
        let items = parse_rawprogram(RAW_XML).expect("应能解析");
        assert_eq!(items.len(), 3);
        // 表达式：数字 0，但**不是**字面量
        assert_eq!(items[0].start_sector, 0);
        assert!(!items[0].start_sector_is_literal);
        assert_eq!(items[0].start_sector_expr.as_deref(), Some("NUM_DISK_SECTORS-33"));
        // 字面量
        assert_eq!(items[1].start_sector, 2048);
        assert!(items[1].start_sector_is_literal);
        // 真的在扇区 0：数字同样是 0，但 is_literal=true
        assert_eq!(items[2].start_sector, 0);
        assert!(items[2].start_sector_is_literal);
        // label 里的单引号原样保留
        assert_eq!(items[0].label.as_deref(), Some("Basic for 'gpt'"));
        assert_eq!(items[0].num_partition_sectors, 6);
        assert_eq!(items[0].sector_size_in_bytes, 4096);
    }

    #[test]
    fn lowercase_sector_size_is_silently_ignored() {
        // 这是**故意**和 QFIL 对齐的行为：不报错，退回 512
        let xml = r#"<data><program filename="a.bin" start_sector="1"
                      num_partition_sectors="1" sector_size_in_bytes="4096" /></data>"#;
        let items = parse_rawprogram(xml).unwrap();
        assert_eq!(items[0].sector_size_in_bytes, 512);
    }

    #[test]
    fn missing_optional_fields_are_null_not_empty() {
        let xml = r#"<patches><patch filename="a.bin" size_in_bytes="8" /></patches>"#;
        let json = parse_patch(xml).unwrap()[0].to_json();
        assert!(json.contains(r#""what":null"#), "{json}");
        assert!(json.contains(r#""start_sector":null"#), "{json}");
        // 必填字段仍然给（空）字符串，而不是 null
        assert!(json.contains(&format!("\"filename\":{}", quoted("a.bin"))), "{json}");
    }

    #[test]
    fn malformed_xml_is_an_error_not_a_panic() {
        assert!(parse_rawprogram("<data><program").is_err());
        assert!(parse_rawprogram("<data><program filename=oops /></data>").is_err());
        assert!(parse_patch("<patches><patch filename='a").is_err());
    }

    #[test]
    fn entities_are_decoded() {
        let xml = r#"<patches><patch filename="a&amp;b.bin" size_in_bytes="1" /></patches>"#;
        assert_eq!(parse_patch(xml).unwrap()[0].filename, "a&b.bin");
    }

    #[test]
    fn single_quoted_attributes_are_supported() {
        let xml = "<patches><patch filename='a.bin' size_in_bytes='16' /></patches>";
        assert_eq!(parse_patch(xml).unwrap()[0].size_in_bytes, 16);
    }

    #[test]
    fn empty_document_returns_empty_list() {
        assert!(parse_patch("<patches></patches>").unwrap().is_empty());
        assert!(parse_rawprogram("<data/>").unwrap().is_empty());
    }

    #[test]
    fn json_array_shape() {
        let items = parse_patch(PATCH_XML).unwrap();
        let json = patches_to_json_array(&items);
        assert!(json.starts_with('[') && json.ends_with(']'));
        assert_eq!(json.matches("\"filename\"").count(), 2);
    }
}
