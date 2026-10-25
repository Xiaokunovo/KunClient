//! Qualcomm Sahara 协议：[`帧层`](self) 与各报文的**编解码**。
//!
//! ## 来源与口径
//!
//! 常量与报文布局**逐字段抄自本仓库既有的 C# 实现**
//! （`KunClient.Core/Protocol/Sahara/{SaharaCommands,SaharaPackets,SaharaPacketTypes}.cs`），
//! 那份又被注明「与 bkerler/edl 的 `sahara_defs.py` 一致」。
//! **不要凭记忆改这里的数字** —— 协议报文写错的表现不是报错，而是设备收到一堆
//! 长度不对的包之后卡在 9008 不动，排查起来极贵。
//!
//! ## 报文结构
//!
//! 所有报文都是「8 字节头 + 负载」，头是两个小端 u32：
//!
//! ```text
//! +0  command : u32   // 见 [`cmd`]
//! +4  length  : u32   // **含这 8 字节头**（不是负载长度！）
//! +8  payload : …     // 各报文自己的字段
//! ```
//!
//! `length` 含头这一条是最容易写错的地方：写成负载长度时，设备会按自己的口径
//! 少读/多读 8 字节，表现为握手超时。
//!
//! ## 已知的 48 字节 Hello
//!
//! `SAHARA_HELLO_RSP`（0x02）在 C# 里是 `WireSize = 48`：
//! `Command, Length, Version, VersionSupported, CmdPacketLength, Mode, Reserved1..6`。
//! 注意 `VersionSupported` 是设备**支持**的版本（不是最低版本），
//! 决定后面能不能走 64 位寻址（0x12/0x11 一族）。

/// Sahara 命令号。`0x01` 是**设备**发给主机的，主机不发。
pub mod cmd {
    pub const HELLO_REQUEST: u32 = 0x01; // 设备 → 主机
    pub const HELLO_RESPONSE: u32 = 0x02;
    pub const READ_DATA: u32 = 0x03;
    pub const END_TRANSFER: u32 = 0x04;
    pub const DONE_REQUEST: u32 = 0x05;
    pub const DONE_RESPONSE: u32 = 0x06;
    pub const RESET_REQUEST: u32 = 0x07;
    pub const RESET_RESPONSE: u32 = 0x08;
    pub const MEMORY_DEBUG: u32 = 0x09;
    pub const MEMORY_READ: u32 = 0x0A;
    pub const COMMAND_READY: u32 = 0x0B;
    pub const SWITCH_MODE: u32 = 0x0C;
    pub const EXECUTE_REQUEST: u32 = 0x0D;
    pub const EXECUTE_RESPONSE: u32 = 0x0E;
    pub const EXECUTE_DATA: u32 = 0x0F;
    pub const MEMORY_DEBUG_64: u32 = 0x10;
    pub const MEMORY_READ_64: u32 = 0x11;
    pub const MEMORY_READ_DATA_64: u32 = 0x12;
    pub const RESET_STATE_MACHINE_ID: u32 = 0x13;

    /// 单包上限（C# `MaxCommandSize`）。超过就要分片。
    pub const MAX_COMMAND_SIZE: usize = 0x1000;
    /// 单次数据上限（C# `MaxDataSize`）。
    pub const MAX_DATA_SIZE: usize = 0x10000;
}

/// `EXECUTE` 的子命令号。
pub mod exec_cmd {
    pub const NOP: u32 = 0x00;
    pub const SERIAL_NUM_READ: u32 = 0x01;
    pub const MSM_HW_ID_READ: u32 = 0x02;
    pub const OEM_PK_HASH_READ: u32 = 0x03;
    pub const SWITCH_TO_DMSS_DLOAD: u32 = 0x04;
    pub const SWITCH_TO_STREAM_DLOAD: u32 = 0x05;
    pub const READ_DEBUG_DATA: u32 = 0x06;
    pub const GET_SOFTWARE_VERSION_SBL: u32 = 0x07;
    pub const GET_COMMAND_ID_LIST: u32 = 0x08;
    pub const GET_TRAINING_DATA: u32 = 0x09;
    pub const READ_CHIP_ID_V3: u32 = 0x0A;
}

/// Hello / 设备上报的工作模式。
pub mod mode {
    pub const IMAGE_TX_PENDING: u32 = 0x0;
    pub const IMAGE_TX_COMPLETE: u32 = 0x1;
    pub const MEMORY_DEBUG: u32 = 0x2;
    pub const COMMAND: u32 = 0x3;
}

/// 报文头长度：所有 Sahea 报文都有这 8 字节。
pub const HEADER_LEN: usize = 8;
/// `SAHARA_HELLO_RSP` 的线上长度（C# `SaharaHelloResponse.WireSize`）。
pub const HELLO_RESPONSE_WIRE_SIZE: usize = 48;

// ── 小端读取助手：越界一律 Err，绝不 panic（这一层直接对着设备送来的字节） ──

fn rd_u32(data: &[u8], off: usize) -> Result<u32, String> {
    data.get(off..off + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        .ok_or_else(|| format!("报文在偏移 {off} 处需要 4 字节，实际只剩 {}", data.len().saturating_sub(off)))
}

fn rd_u64(data: &[u8], off: usize) -> Result<u64, String> {
    data.get(off..off + 8)
        .map(|b| u64::from_le_bytes(b.try_into().expect("已按 8 字节切片")))
        .ok_or_else(|| format!("报文在偏移 {off} 处需要 8 字节，实际只剩 {}", data.len().saturating_sub(off)))
}

/// 报文头。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PacketHeader {
    pub command: u32,
    /// **含 8 字节头**（见模块注释）。
    pub length: u32,
}

impl PacketHeader {
    pub fn parse(data: &[u8]) -> Result<PacketHeader, String> {
        if data.len() < HEADER_LEN {
            return Err(format!("报文短于最小头（8 字节），实际 {} 字节", data.len()));
        }
        Ok(PacketHeader { command: rd_u32(data, 0)?, length: rd_u32(data, 4)? })
    }

    /// 负载长度 = 总长 − 8。`length < 8` 视为非法（设备发坏包时不panic）。
    pub fn payload_len(&self) -> Result<usize, String> {
        (self.length as usize)
            .checked_sub(HEADER_LEN)
            .ok_or_else(|| format!("报文头声明 length={} 小于 8，非法", self.length))
    }
}

/// 组一个报文：包头 `length = 8 + payload.len()`。
pub fn build(command: u32, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER_LEN + payload.len());
    out.extend_from_slice(&command.to_le_bytes());
    out.extend_from_slice(&((HEADER_LEN + payload.len()) as u32).to_le_bytes());
    out.extend_from_slice(payload);
    out
}

/// 只带头、无负载的报文（`DONE_REQ` / `RESET_REQ` / `CMD_READY` 等）。
pub fn build_bare(command: u32) -> Vec<u8> {
    build(command, &[])
}

/// `SAHARA_HELLO_RSP`（0x02）。**48 字节**，字段顺序见模块注释。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HelloResponse {
    pub command: u32,
    pub length: u32,
    pub version: u32,
    /// 设备**支持**的版本；>= 3 才谈得上 64 位寻址那一族命令。
    pub version_supported: u32,
    pub cmd_packet_length: u32,
    pub mode: u32,
    pub reserved: [u32; 6],
}

impl HelloResponse {
    pub fn parse(data: &[u8]) -> Result<HelloResponse, String> {
        if data.len() < HELLO_RESPONSE_WIRE_SIZE {
            return Err(format!(
                "SAHARA_HELLO_RSP 需要 {HELLO_RESPONSE_WIRE_SIZE} 字节，实际 {} 字节",
                data.len()
            ));
        }
        let mut reserved = [0u32; 6];
        for (i, slot) in reserved.iter_mut().enumerate() {
            *slot = rd_u32(data, 24 + i * 4)?;
        }
        Ok(HelloResponse {
            command: rd_u32(data, 0)?,
            length: rd_u32(data, 4)?,
            version: rd_u32(data, 8)?,
            version_supported: rd_u32(data, 12)?,
            cmd_packet_length: rd_u32(data, 16)?,
            mode: rd_u32(data, 20)?,
            reserved,
        })
    }

    /// 设备是否支持 64 位寻址（决定走 0x12 还是 0x03）。
    pub fn supports_64bit(&self) -> bool {
        self.version_supported >= 3
    }

    /// 设备上报的 `CmdPacketLength` 是否可信（太大时不能拿它当分片依据）。
    pub fn max_packet_len(&self) -> usize {
        (self.cmd_packet_length as usize).min(cmd::MAX_DATA_SIZE)
    }
}

/// `SAHARA_READ_DATA`（0x03，32 位寻址）。**20 字节**。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ReadData {
    pub command: u32,
    pub length: u32,
    pub image_id: u32,
    pub data_offset: u32,
    pub data_length: u32,
}

impl ReadData {
    pub fn parse(data: &[u8]) -> Result<ReadData, String> {
        if data.len() < 20 {
            return Err(format!("SAHARA_READ_DATA 需要 20 字节，实际 {} 字节", data.len()));
        }
        Ok(ReadData {
            command: rd_u32(data, 0)?,
            length: rd_u32(data, 4)?,
            image_id: rd_u32(data, 8)?,
            data_offset: rd_u32(data, 12)?,
            data_length: rd_u32(data, 16)?,
        })
    }

    /// 请求的 [起, 止) 区间（用 u64 算，避免 offset+length 溢出 u32）。
    pub fn requested_range(&self) -> (u64, u64) {
        let start = self.data_offset as u64;
        (start, start + self.data_length as u64)
    }
}

/// `SAHARA_64BIT_MEMORY_READ_DATA`（0x12，64 位寻址）。**32 字节**。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ReadData64 {
    pub command: u32,
    pub length: u32,
    pub image_id: u64,
    pub data_offset: u64,
    pub data_length: u64,
}

impl ReadData64 {
    pub fn parse(data: &[u8]) -> Result<ReadData64, String> {
        if data.len() < 32 {
            return Err(format!("0x12 READ_DATA(64) 需要 32 字节，实际 {} 字节", data.len()));
        }
        Ok(ReadData64 {
            command: rd_u32(data, 0)?,
            length: rd_u32(data, 4)?,
            image_id: rd_u64(data, 8)?,
            data_offset: rd_u64(data, 16)?,
            data_length: rd_u64(data, 24)?,
        })
    }

    pub fn requested_range(&self) -> (u64, u64) {
        (self.data_offset, self.data_offset + self.data_length)
    }
}

/// `SAHARA_END_TRANSFER`（0x04）。**16 字节**。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EndTransfer {
    pub command: u32,
    pub length: u32,
    pub image_id: u32,
    pub status: u32,
}

impl EndTransfer {
    pub fn parse(data: &[u8]) -> Result<EndTransfer, String> {
        if data.len() < 16 {
            return Err(format!("SAHARA_END_TRANSFER 需要 16 字节，实际 {} 字节", data.len()));
        }
        Ok(EndTransfer {
            command: rd_u32(data, 0)?,
            length: rd_u32(data, 4)?,
            image_id: rd_u32(data, 8)?,
            status: rd_u32(data, 12)?,
        })
    }
}

/// `SAHARA_DONE_RSP`（0x06）。**16 字节**。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DoneResponse {
    pub command: u32,
    pub length: u32,
    pub image_transaction_id: u32,
    pub status: u32,
}

impl DoneResponse {
    pub fn parse(data: &[u8]) -> Result<DoneResponse, String> {
        if data.len() < 16 {
            return Err(format!("SAHARA_DONE_RSP 需要 16 字节，实际 {} 字节", data.len()));
        }
        Ok(DoneResponse {
            command: rd_u32(data, 0)?,
            length: rd_u32(data, 4)?,
            image_transaction_id: rd_u32(data, 8)?,
            status: rd_u32(data, 12)?,
        })
    }
}

/// `SAHARA_EXECUTE_RSP`（0x0E）。**16 字节**。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExecuteResponse {
    pub command: u32,
    pub length: u32,
    pub client_command: u32,
    pub data_length: u32,
}

impl ExecuteResponse {
    pub fn parse(data: &[u8]) -> Result<ExecuteResponse, String> {
        if data.len() < 16 {
            return Err(format!("SAHARA_EXECUTE_RSP 需要 16 字节，实际 {} 字节", data.len()));
        }
        Ok(ExecuteResponse {
            command: rd_u32(data, 0)?,
            length: rd_u32(data, 4)?,
            client_command: rd_u32(data, 8)?,
            data_length: rd_u32(data, 12)?,
        })
    }
}

/// `EXECUTE` 返回的设备信息（**变长**：不同子命令返回不同长度）。
///
/// 因此这里逐字段按「长度够就读」解析 —— 照搬 C# `SaharaExecuteData.Parse` 的口径。
/// 缺的字段保持 0，**不报错**：设备返回短包是常态，报错会让本来能用的流程失败。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ExecuteData {
    pub offset_0x00: u32,
    pub serial_number: u32,
    pub msm_hw_id: u32,
    pub unknown: u32,
    pub pk_hash: [u32; 8],
}

impl ExecuteData {
    pub fn parse(data: &[u8]) -> ExecuteData {
        let mut out = ExecuteData::default();
        let take_u32 = |off: usize, slot: &mut u32| {
            if let Ok(v) = rd_u32(data, off) {
                *slot = v;
            }
        };
        take_u32(0, &mut out.offset_0x00);
        take_u32(4, &mut out.serial_number);
        take_u32(8, &mut out.msm_hw_id);
        take_u32(12, &mut out.unknown);
        for i in 0..8 {
            let mut v = 0u32;
            take_u32(16 + i * 4, &mut v);
            out.pk_hash[i] = v;
        }
        out
    }

    /// 序列号按 8 位十六进制显示（C# 侧日志就是这个形状）。
    pub fn serial_hex(&self) -> String {
        format!("{:08X}", self.serial_number)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 组一个 Hello：
    /// 48 字节 = 8 头 + version/version_supported/cmd_packet_length/mode + 6 保留字
    fn hello_bytes(version: u32, supported: u32, max_len: u32, mode_v: u32) -> Vec<u8> {
        let mut payload = Vec::new();
        for v in [version, supported, max_len, mode_v] {
            payload.extend_from_slice(&v.to_le_bytes());
        }
        payload.extend_from_slice(&[0u8; 24]); // 6 个保留字
        build(cmd::HELLO_RESPONSE, &payload)
    }

    #[test]
    fn header_length_includes_the_header_itself() {
        // 这条最容易写错：length 含 8 字节头，不是负载长度
        let pkt = build(cmd::READ_DATA, &[0u8; 12]);
        assert_eq!(pkt.len(), 20);
        let h = PacketHeader::parse(&pkt).unwrap();
        assert_eq!(h.command, cmd::READ_DATA);
        assert_eq!(h.length, 20);
        assert_eq!(h.payload_len().unwrap(), 12);
        // 裸包 length == 8
        assert_eq!(PacketHeader::parse(&build_bare(cmd::DONE_REQUEST)).unwrap().length, 8);
    }

    #[test]
    fn header_rejects_short_and_illegal_length() {
        assert!(PacketHeader::parse(&[0u8; 7]).is_err(), "短于 8 字节必须报错");
        let mut bad = build_bare(cmd::DONE_REQUEST);
        bad[4..8].copy_from_slice(&4u32.to_le_bytes()); // length < 8
        let h = PacketHeader::parse(&bad).unwrap();
        assert!(h.payload_len().is_err(), "length < 8 必须被判非法，而不是下溢 panic");
    }

    #[test]
    fn parses_48_byte_hello() {
        let h = HelloResponse::parse(&hello_bytes(2, 3, 0x1000, mode::IMAGE_TX_PENDING)).unwrap();
        assert_eq!(h.command, cmd::HELLO_RESPONSE);
        assert_eq!(h.length, 48);
        assert_eq!(h.version, 2);
        assert_eq!(h.version_supported, 3);
        assert_eq!(h.cmd_packet_length, 0x1000);
        assert_eq!(h.mode, mode::IMAGE_TX_PENDING);
        // 版本 3 → 支持 64 位寻址（决定后面走 0x12 还是 0x03）
        assert!(h.supports_64bit());
        assert!(!HelloResponse::parse(&hello_bytes(2, 2, 0x1000, 0)).unwrap().supports_64bit());
    }

    #[test]
    fn hello_shorter_than_48_is_rejected() {
        // 47 字节必须被拒 —— 少读一个保留字会让后面所有字段错位
        let mut pkt = hello_bytes(2, 3, 0x1000, 0);
        pkt.truncate(47);
        assert!(HelloResponse::parse(&pkt).is_err());
    }

    #[test]
    fn hello_caps_absurd_packet_length() {
        // 设备上报离谱的包长时不能直接采信（会被拿去当分片依据）
        let h = HelloResponse::parse(&hello_bytes(2, 3, 0xFFFF_FFFF, 0)).unwrap();
        assert_eq!(h.max_packet_len(), cmd::MAX_DATA_SIZE);
    }

    #[test]
    fn parses_read_data_32bit_and_computes_range() {
        let mut payload = Vec::new();
        for v in [7u32, 0x1000, 0x2000] {
            payload.extend_from_slice(&v.to_le_bytes());
        }
        let pkt = build(cmd::READ_DATA, &payload);
        let r = ReadData::parse(&pkt).unwrap();
        assert_eq!(r.image_id, 7);
        assert_eq!(r.requested_range(), (0x1000, 0x3000));
        assert!(ReadData::parse(&pkt[..19]).is_err(), "19 字节必须被拒");
    }

    #[test]
    fn parses_read_data_64bit() {
        let mut payload = Vec::new();
        payload.extend_from_slice(&7u64.to_le_bytes());
        payload.extend_from_slice(&0x1_0000_0000u64.to_le_bytes());
        payload.extend_from_slice(&0x1000u64.to_le_bytes());
        let pkt = build(cmd::MEMORY_READ_DATA_64, &payload);
        let r = ReadData64::parse(&pkt).unwrap();
        assert_eq!(r.image_id, 7);
        // 32 位实现下这个 offset 会溢出，64 位版必须原样带住
        assert_eq!(r.requested_range(), (0x1_0000_0000, 0x1_0000_1000));
        assert!(ReadData64::parse(&pkt[..31]).is_err());
    }

    #[test]
    fn parses_end_transfer_done_and_execute() {
        // 负载要按显式小端造：`[1u8; 8]` 读出来是 0x01010101，那是自欺欺人
        let mut p = Vec::new();
        p.extend_from_slice(&13u32.to_le_bytes()); // image_id
        p.extend_from_slice(&0u32.to_le_bytes());  // status = 成功
        let e = EndTransfer::parse(&build(cmd::END_TRANSFER, &p)).unwrap();
        assert_eq!(e.image_id, 13);
        assert_eq!(e.status, 0);
        assert_eq!(e.length, 16);

        let mut p2 = Vec::new();
        p2.extend_from_slice(&1u32.to_le_bytes()); // transaction id
        p2.extend_from_slice(&0u32.to_le_bytes());
        let d = DoneResponse::parse(&build(cmd::DONE_RESPONSE, &p2)).unwrap();
        assert_eq!(d.command, cmd::DONE_RESPONSE);
        assert_eq!(d.image_transaction_id, 1);

        let mut p3 = Vec::new();
        p3.extend_from_slice(&2u32.to_le_bytes()); // client command = MSM_HW_ID_READ
        p3.extend_from_slice(&16u32.to_le_bytes());
        let x = ExecuteResponse::parse(&build(cmd::EXECUTE_RESPONSE, &p3)).unwrap();
        assert_eq!(x.command, cmd::EXECUTE_RESPONSE);
        assert_eq!(x.client_command, exec_cmd::MSM_HW_ID_READ);
        assert_eq!(x.data_length, 16);
    }

    #[test]
    fn execute_data_tolerates_short_payloads() {
        // 设备返回短包是常态：缺的字段补 0，不报错
        // ⚠️ 收的是**负载**，不是整包 —— C# `SaharaClient.ExecuteCommand` 传进来的
        //    data 已经是 EXECUTE_DATA 的负载（包头在它上一层被吃掉了）。
        //    传整包会把 command/length 当成前两个字段读 —— 本测试第一版正是这么错的。
        let full = {
            let mut v = Vec::new();
            for i in 0..12u32 {
                v.extend_from_slice(&i.to_le_bytes());
            }
            build(cmd::EXECUTE_DATA, &v)
        };
        let d = ExecuteData::parse(&full[HEADER_LEN..]);
        assert_eq!(d.offset_0x00, 0);
        assert_eq!(d.serial_number, 1);
        assert_eq!(d.msm_hw_id, 2);
        assert_eq!(d.pk_hash[0], 4);
        assert_eq!(d.pk_hash[7], 11);

        // 只有 8 字节负载：前两个字段有值，其余保持 0（**不报错** —— 设备返回短包是常态）
        let short: Vec<u8> = [0u8, 1, 2, 3, 9, 9, 9, 9].to_vec();
        let s = ExecuteData::parse(&short);
        assert_eq!(s.offset_0x00, 0x0302_0100);
        assert_eq!(s.serial_number, 0x0909_0909);
        assert_eq!(s.msm_hw_id, 0, "长度不够的字段必须是 0，而不是垃圾值");
        assert_eq!(s.pk_hash, [0u32; 8]);
        // 空负载也不许崩
        assert_eq!(ExecuteData::parse(&[]).serial_number, 0);
    }

    #[test]
    fn serial_is_rendered_as_eight_hex_digits() {
        let d = ExecuteData { serial_number: 0x2268_B9D7, ..Default::default() };
        assert_eq!(d.serial_hex(), "2268B9D7");
    }
}
