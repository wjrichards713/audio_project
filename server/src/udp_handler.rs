//! UDP packet receive-and-forward loop.
//!
//! This is the performance-critical hot path.  Design goals:
//! - Zero heap allocations per packet (reuse buffers, pre-allocated target vec).
//! - Minimal lock contention (DashMap sharded reads).
//! - No decryption or payload inspection.

use std::net::SocketAddr;
use std::sync::Arc;

use tokio::net::UdpSocket;
use tracing::{debug, trace, warn};

use crate::channel::ServerState;
use crate::protocol::{PacketType, RoutingHeader, MAX_UDP_PACKET_SIZE, ROUTING_HEADER_SIZE};

/// Run the UDP receive loop.  This function never returns under normal
/// operation.
pub async fn run_udp_loop(socket: Arc<UdpSocket>, state: Arc<ServerState>) {
    // Pre-allocate a receive buffer and a target list that we reuse every
    // iteration — no per-packet allocations.
    let mut buf = vec![0u8; MAX_UDP_PACKET_SIZE];
    let mut targets: Vec<SocketAddr> = Vec::with_capacity(64);

    loop {
        let (len, src_addr) = match socket.recv_from(&mut buf).await {
            Ok(r) => r,
            Err(e) => {
                warn!(error = %e, "UDP recv_from error");
                continue;
            }
        };

        // Discard packets that are too small for a routing header.
        if len < ROUTING_HEADER_SIZE {
            trace!(len, "dropping runt UDP packet");
            continue;
        }

        let header = match RoutingHeader::parse(&buf[..len]) {
            Some(h) => h,
            None => continue,
        };

        let packet_type = match PacketType::from_u8(header.packet_type) {
            Some(pt) => pt,
            None => {
                trace!(packet_type = header.packet_type, "unknown packet type");
                continue;
            }
        };

        // Register (or update) the client's UDP address so we know where to
        // forward audio to them.
        register_udp_addr(&state, header.client_id, src_addr);

        match packet_type {
            PacketType::Audio => {
                // Forward the *entire* packet (header + encrypted payload) to
                // every other member in the channel.
                state.collect_forward_targets(header.channel_id, header.client_id, &mut targets);

                for &target in &targets {
                    if let Err(e) = socket.send_to(&buf[..len], target).await {
                        debug!(target = %target, error = %e, "UDP send_to failed");
                    }
                }
            }
            PacketType::Keepalive => {
                // Keepalives just refresh the UDP address mapping (done above).
                trace!(client_id = header.client_id, "keepalive received");
            }
            PacketType::Ping => {
                // Respond with a pong: flip the packet type byte and echo back.
                let mut reply = Vec::new();
                reply.extend_from_slice(&buf[..len]);
                reply[16] = PacketType::Pong as u8;
                if let Err(e) = socket.send_to(&reply, src_addr).await {
                    debug!(error = %e, "failed to send UDP pong");
                }
            }
            PacketType::Pong => {
                // We don't expect pongs from clients, ignore.
                trace!("unexpected pong from client");
            }
        }
    }
}

/// Associate a UDP `SocketAddr` with a client, using the numeric client ID
/// from the routing header as the key.
#[inline]
fn register_udp_addr(state: &ServerState, udp_client_id: u64, addr: SocketAddr) {
    // Look up the client UUID from the numeric UDP client ID.
    if let Some(client_uuid) = state.udp_id_to_client.get(&udp_client_id) {
        let client_uuid = client_uuid.clone();
        if let Some(mut client) = state.clients.get_mut(&client_uuid) {
            let old = client.udp_addr.replace(addr);
            if old != Some(addr) {
                // Address changed (NAT rebind, etc.) — update reverse map.
                if let Some(old_addr) = old {
                    state.udp_addr_to_client.remove(&old_addr);
                }
                state
                    .udp_addr_to_client
                    .insert(addr, client_uuid.clone());
                debug!(client = %client_uuid, addr = %addr, "registered UDP address");
            }
        }
    }
}
