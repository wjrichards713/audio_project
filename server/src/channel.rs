//! Channel state management.
//!
//! Channels are the top-level grouping for real-time audio.  Each channel has a
//! set of members (client IDs) and tracks who is currently transmitting.

use std::collections::HashSet;
use std::net::SocketAddr;
use std::sync::Arc;

use dashmap::DashMap;
use tracing::{info, warn};

use crate::client::Client;
use crate::protocol::{ChannelMemberInfo, ServerMessage};

/// A single audio channel.
pub struct Channel {
    /// Human-readable channel name (e.g. "channel-1").
    pub name: String,

    /// Set of client IDs that are members of this channel.
    pub members: HashSet<String>,
}

impl Channel {
    pub fn new(name: String) -> Self {
        Self {
            name,
            members: HashSet::new(),
        }
    }
}

/// Shared server state accessible from all tasks.
pub struct ServerState {
    /// All known channels, keyed by channel name.
    pub channels: DashMap<String, Channel>,

    /// All connected clients, keyed by client ID (UUID string).
    pub clients: DashMap<String, Client>,

    /// Reverse lookup: UDP `SocketAddr` → client ID.
    /// Populated when we receive the first UDP packet from a client.
    pub udp_addr_to_client: DashMap<SocketAddr, String>,

    /// Reverse lookup: numeric UDP client_id (u64 from routing header) → client UUID string.
    pub udp_id_to_client: DashMap<u64, String>,

    /// The UDP port the server is listening on.
    pub udp_port: u16,
}

impl ServerState {
    pub fn new(udp_port: u16) -> Arc<Self> {
        let state = Arc::new(Self {
            channels: DashMap::new(),
            clients: DashMap::new(),
            udp_addr_to_client: DashMap::new(),
            udp_id_to_client: DashMap::new(),
            udp_port,
        });

        // Pre-configure the three test channels.
        for name in &["channel-1", "channel-2", "channel-3"] {
            state
                .channels
                .insert(name.to_string(), Channel::new(name.to_string()));
        }

        info!("initialized server state with 3 pre-configured channels");
        state
    }

    // -----------------------------------------------------------------------
    // Channel operations
    // -----------------------------------------------------------------------

    /// Add a client to a channel.  Returns the list of current members (for the
    /// `channel_joined` response) and broadcasts `user_joined` to existing members.
    pub async fn join_channel(
        self: &Arc<Self>,
        client_id: &str,
        channel_id: &str,
    ) -> Result<Vec<ChannelMemberInfo>, String> {
        // Validate channel exists.
        let mut channel = self
            .channels
            .get_mut(channel_id)
            .ok_or_else(|| format!("channel '{}' does not exist", channel_id))?;

        // Validate client exists and is not already in a channel.
        {
            let client = self
                .clients
                .get(client_id)
                .ok_or_else(|| "client not found".to_string())?;
            if let Some(ref current) = client.channel_id {
                return Err(format!(
                    "already in channel '{}'; leave first",
                    current
                ));
            }
        }

        // Collect member info *before* inserting, so we can broadcast.
        let existing_members: Vec<ChannelMemberInfo> = channel
            .members
            .iter()
            .filter_map(|mid| {
                self.clients.get(mid).map(|c| ChannelMemberInfo {
                    client_id: c.id.clone(),
                    user_name: c.user_name.clone(),
                    speaking: c.speaking,
                })
            })
            .collect();

        // Insert into channel.
        channel.members.insert(client_id.to_string());

        // Update client record.
        let (user_name, client_id_str) = {
            let mut client = self.clients.get_mut(client_id).unwrap();
            client.channel_id = Some(channel_id.to_string());
            (client.user_name.clone(), client.id.clone())
        };

        // We must drop the mutable borrow on channel before broadcasting.
        let member_ids: Vec<String> = channel.members.iter().cloned().collect();
        drop(channel);

        // Broadcast user_joined to all *other* members.
        let join_msg = ServerMessage::UserJoined {
            channel_id: channel_id.to_string(),
            client_id: client_id_str,
            user_name,
        };
        self.broadcast_to_members(&member_ids, client_id, &join_msg)
            .await;

        // Return member list including the new member.
        let mut members = existing_members;
        if let Some(c) = self.clients.get(client_id) {
            members.push(ChannelMemberInfo {
                client_id: c.id.clone(),
                user_name: c.user_name.clone(),
                speaking: c.speaking,
            });
        }

        info!(client = %client_id, channel = %channel_id, "client joined channel");
        Ok(members)
    }

    /// Remove a client from a channel.  Broadcasts `user_left` to remaining
    /// members.  Also stops transmitting if the client was speaking.
    pub async fn leave_channel(
        self: &Arc<Self>,
        client_id: &str,
        channel_id: &str,
    ) -> Result<(), String> {
        // Stop speaking first (no-op if not speaking).
        self.stop_transmit(client_id, channel_id).await.ok();

        let member_ids: Vec<String>;
        {
            let mut channel = self
                .channels
                .get_mut(channel_id)
                .ok_or_else(|| format!("channel '{}' does not exist", channel_id))?;

            if !channel.members.remove(client_id) {
                return Err("not in that channel".to_string());
            }
            member_ids = channel.members.iter().cloned().collect();
        }

        // Update client record.
        if let Some(mut client) = self.clients.get_mut(client_id) {
            client.channel_id = None;
        }

        // Broadcast.
        let left_msg = ServerMessage::UserLeft {
            channel_id: channel_id.to_string(),
            client_id: client_id.to_string(),
        };
        self.broadcast_to_members(&member_ids, client_id, &left_msg)
            .await;

        info!(client = %client_id, channel = %channel_id, "client left channel");
        Ok(())
    }

    /// Mark a client as transmitting in a channel.
    pub async fn start_transmit(
        self: &Arc<Self>,
        client_id: &str,
        channel_id: &str,
    ) -> Result<(), String> {
        let member_ids: Vec<String>;
        {
            let channel = self
                .channels
                .get(channel_id)
                .ok_or_else(|| format!("channel '{}' does not exist", channel_id))?;

            if !channel.members.contains(client_id) {
                return Err("not in that channel".to_string());
            }
            member_ids = channel.members.iter().cloned().collect();
        }

        if let Some(mut client) = self.clients.get_mut(client_id) {
            client.speaking = true;
        }

        let msg = ServerMessage::UserSpeaking {
            channel_id: channel_id.to_string(),
            client_id: client_id.to_string(),
        };
        self.broadcast_to_members(&member_ids, client_id, &msg)
            .await;

        info!(client = %client_id, channel = %channel_id, "client started transmitting");
        Ok(())
    }

    /// Mark a client as no longer transmitting.
    pub async fn stop_transmit(
        self: &Arc<Self>,
        client_id: &str,
        channel_id: &str,
    ) -> Result<(), String> {
        let member_ids: Vec<String>;
        {
            let channel = self
                .channels
                .get(channel_id)
                .ok_or_else(|| format!("channel '{}' does not exist", channel_id))?;

            if !channel.members.contains(client_id) {
                return Err("not in that channel".to_string());
            }
            member_ids = channel.members.iter().cloned().collect();
        }

        let was_speaking;
        if let Some(mut client) = self.clients.get_mut(client_id) {
            was_speaking = client.speaking;
            client.speaking = false;
        } else {
            return Err("client not found".to_string());
        }

        if was_speaking {
            let msg = ServerMessage::UserStopped {
                channel_id: channel_id.to_string(),
                client_id: client_id.to_string(),
            };
            self.broadcast_to_members(&member_ids, client_id, &msg)
                .await;
            info!(client = %client_id, channel = %channel_id, "client stopped transmitting");
        }

        Ok(())
    }

    /// Remove a client entirely (disconnect cleanup).  Leaves any channel the
    /// client was in and removes all bookkeeping.
    pub async fn remove_client(self: &Arc<Self>, client_id: &str) {
        // Leave channel if in one.
        let channel_id = self
            .clients
            .get(client_id)
            .and_then(|c| c.channel_id.clone());

        if let Some(ch) = channel_id {
            if let Err(e) = self.leave_channel(client_id, &ch).await {
                warn!(client = %client_id, error = %e, "error during disconnect leave");
            }
        }

        // Remove reverse lookups.
        if let Some((_, client)) = self.clients.remove(client_id) {
            if let Some(addr) = client.udp_addr {
                self.udp_addr_to_client.remove(&addr);
            }
            self.udp_id_to_client.remove(&client.udp_client_id);
        }

        info!(client = %client_id, "client removed");
    }

    /// Collect UDP [`SocketAddr`]s of all channel members except `exclude_id`.
    /// Returns into a caller-provided `Vec` to avoid allocations on the hot path
    /// when reused.
    #[inline]
    pub fn collect_forward_targets(
        &self,
        channel_id: u32,
        exclude_client_udp_id: u64,
        targets: &mut Vec<SocketAddr>,
    ) {
        targets.clear();

        // Channel IDs in UDP headers are numeric; map to string name.
        let channel_name = format!("channel-{}", channel_id);

        let channel = match self.channels.get(&channel_name) {
            Some(c) => c,
            None => return,
        };

        for member_id in channel.members.iter() {
            if let Some(client) = self.clients.get(member_id) {
                if client.udp_client_id != exclude_client_udp_id {
                    if let Some(addr) = client.udp_addr {
                        targets.push(addr);
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Send a message to every client in `member_ids` except `exclude_id`.
    async fn broadcast_to_members(
        &self,
        member_ids: &[String],
        exclude_id: &str,
        msg: &ServerMessage,
    ) {
        for mid in member_ids {
            if mid == exclude_id {
                continue;
            }
            if let Some(client) = self.clients.get(mid) {
                if let Err(e) = client.send(msg).await {
                    warn!(client = %mid, error = %e, "failed to send ws broadcast");
                }
            }
        }
    }
}
