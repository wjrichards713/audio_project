//! Wire protocol definitions for both UDP routing headers and WebSocket JSON messages.

use serde::{Deserialize, Serialize};

// ---------------------------------------------------------------------------
// UDP routing header — 20 bytes, always unencrypted
// ---------------------------------------------------------------------------

/// Size of the routing header that precedes every UDP packet.
pub const ROUTING_HEADER_SIZE: usize = 20;

/// Maximum UDP packet size we will accept (header + encrypted payload).
/// Standard MTU minus IP/UDP overhead, rounded down.
pub const MAX_UDP_PACKET_SIZE: usize = 1500;

/// Packet types carried in the routing header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PacketType {
    Audio = 0,
    Keepalive = 1,
    Ping = 2,
    Pong = 3,
}

impl PacketType {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Self::Audio),
            1 => Some(Self::Keepalive),
            2 => Some(Self::Ping),
            3 => Some(Self::Pong),
            _ => None,
        }
    }
}

/// Parsed routing header — zero-copy view into the first 20 bytes of a packet.
#[derive(Debug, Clone, Copy)]
pub struct RoutingHeader {
    pub server_id: u32,
    pub channel_id: u32,
    pub client_id: u64,
    pub packet_type: u8,
    pub flags: u8,
    pub length: u16,
}

impl RoutingHeader {
    /// Parse a routing header from a byte slice.  Returns `None` if the slice
    /// is shorter than [`ROUTING_HEADER_SIZE`].
    #[inline(always)]
    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < ROUTING_HEADER_SIZE {
            return None;
        }
        Some(Self {
            server_id: u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]),
            channel_id: u32::from_be_bytes([buf[4], buf[5], buf[6], buf[7]]),
            client_id: u64::from_be_bytes([
                buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
            ]),
            packet_type: buf[16],
            flags: buf[17],
            length: u16::from_be_bytes([buf[18], buf[19]]),
        })
    }
}

// ---------------------------------------------------------------------------
// WebSocket JSON messages — client → server
// ---------------------------------------------------------------------------

#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ClientMessage {
    Auth {
        token: String,
    },
    JoinChannel {
        channel_id: String,
    },
    LeaveChannel {
        channel_id: String,
    },
    StartTransmit {
        channel_id: String,
    },
    StopTransmit {
        channel_id: String,
    },
    Ping {
        timestamp: u64,
    },
}

// ---------------------------------------------------------------------------
// WebSocket JSON messages — server → client
// ---------------------------------------------------------------------------

#[derive(Debug, Serialize, Clone)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ServerMessage {
    AuthOk {
        client_id: String,
        udp_port: u16,
    },
    ChannelJoined {
        channel_id: String,
        members: Vec<ChannelMemberInfo>,
    },
    ChannelLeft {
        channel_id: String,
    },
    UserJoined {
        channel_id: String,
        client_id: String,
        user_name: String,
    },
    UserLeft {
        channel_id: String,
        client_id: String,
    },
    UserSpeaking {
        channel_id: String,
        client_id: String,
    },
    UserStopped {
        channel_id: String,
        client_id: String,
    },
    Pong {
        timestamp: u64,
        server_time: u64,
    },
    Error {
        message: String,
    },
}

#[derive(Debug, Serialize, Clone)]
pub struct ChannelMemberInfo {
    pub client_id: String,
    pub user_name: String,
    pub speaking: bool,
}
