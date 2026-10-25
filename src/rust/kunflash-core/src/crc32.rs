//! 标准 IEEE CRC-32（多项式 `0xEDB88320`，反射算法），与 QFIL / bkerler-edl 同口径。
//!
//! 用途：Firehose 的 `program` / `patch` 命令里校验数据块。算错了不会报错，
//! 只会让设备端判定校验失败 —— 所以这里必须和厂商工具**逐位一致**。
//!
//! 表驱动实现：256 项查表，编译期常量，运行时零分配。

const TABLE: [u32; 256] = build_table();

const fn build_table() -> [u32; 256] {
    let mut table = [0u32; 256];
    let mut i = 0usize;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            // 反射型：最低位为 1 就右移后异或多项式
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        table[i] = c;
        i += 1;
    }
    table
}

/// 计算 CRC-32。结果与 `zlib.crc32`、`CRC32("123456789") == 0xCBF43926` 一致。
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc = TABLE[((crc ^ b as u32) & 0xFF) as usize] ^ (crc >> 8);
    }
    crc ^ 0xFFFF_FFFF
}

#[cfg(test)]
mod tests {
    use super::crc32;

    #[test]
    fn standard_vector() {
        // 标准测试向量：算不对就是多项式/反射方向搞错了
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn empty_is_zero() {
        assert_eq!(crc32(b""), 0);
    }

    #[test]
    fn table_loop_matches_and_is_deterministic() {
        // 分块喂数据（刷写是按块调的）结果必须稳定：这里手工重跑一遍表循环做交叉验证，
        // 防止将来有人「优化」成查表以外的算法而破坏与厂商工具的一致性。
        let data: Vec<u8> = (0u8..=255).cycle().take(10_000).collect();
        let mut again = 0xFFFF_FFFFu32;
        for &b in &data {
            again = super::TABLE[((again ^ b as u32) & 0xFF) as usize] ^ (again >> 8);
        }
        assert_eq!(crc32(&data), again ^ 0xFFFF_FFFF);
        assert_eq!(crc32(&data), crc32(&data), "同样输入必须同样输出");
    }
}
