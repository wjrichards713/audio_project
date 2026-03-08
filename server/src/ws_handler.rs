//! WebSocket signaling handler.
//!
//! Each accepted TCP connection is upgraded to a WebSocket, then we run a
//! per-connection message loop that dispatches [`ClientMessage`]s to the
//! shared [`ServerState`].

use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use futures_util::StreamExt;
use tokio::net::{TcpListener, TcpStream};
use tracing::{error, info, warn};

use crate::channel::ServerState;
use crate::client::Client;
use crate::protocol::{ClientMessage, ServerMessage};

/// Accept WebSocket connections in a loop.  Never returns.
pub async fn run_ws_server(listener: TcpListener, state: Arc<ServerState>) {
    loop {
        let (stream, peer) = match listener.accept().await {
            Ok(v) => v,
            Err(e) => {
                error!(error = %e, "WS accept error");
                continue;
            }
        };

        let state = Arc::clone(&state);
        tokio::spawn(async move {
            if let Err(e) = handle_connection(stream, state).await {
                warn!(peer = %peer, error = %e, "WS connection error");
            }
        });
    }
}

async fn handle_connection(
    stream: TcpStream,
    state: Arc<ServerState>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let ws_stream = tokio_tungstenite::accept_async(stream).await?;
    let (sender, mut receiver) = ws_stream.split();

    // Wait for the first message, which must be an `auth` message.
    let client_id = loop {
        let msg = match receiver.next().await {
            Some(Ok(m)) => m,
            Some(Err(e)) => return Err(e.into()),
            None => return Ok(()), // connection closed before auth
        };

        if let tungstenite::Message::Text(text) = msg {
            match serde_json::from_str::<ClientMessage>(&text) {
                Ok(ClientMessage::Auth { token }) => {
                    let id = uuid::Uuid::new_v4().to_string();
                    // Derive a stable numeric ID for UDP routing from the first
                    // 8 bytes of the UUID.
                    let udp_client_id = derive_udp_id(&id);

                    // Use the token as a rudimentary username for the PoC.
                    let user_name = if token.is_empty() {
                        format!("user-{}", &id[..8])
                    } else {
                        token.clone()
                    };

                    let client = Client::new(id.clone(), udp_client_id, user_name, sender);

                    // Send auth_ok *before* inserting into state so the sender
                    // is still owned by `client`.
                    let auth_ok = ServerMessage::AuthOk {
                        client_id: id.clone(),
                        udp_port: state.udp_port,
                    };
                    client.send(&auth_ok).await?;

                    state.clients.insert(id.clone(), client);
                    state.udp_id_to_client.insert(udp_client_id, id.clone());

                    info!(client = %id, "client authenticated");
                    break id;
                }
                Ok(_) => {
                    // Non-auth message before auth — send error but keep trying.
                    // We can't send via Client yet, so just ignore.
                    warn!("received non-auth message before authentication");
                    continue;
                }
                Err(e) => {
                    warn!(error = %e, "failed to parse auth message");
                    continue;
                }
            }
        }
    };

    // Main message loop.
    while let Some(result) = receiver.next().await {
        let msg = match result {
            Ok(tungstenite::Message::Text(text)) => text,
            Ok(tungstenite::Message::Close(_)) => break,
            Ok(tungstenite::Message::Ping(_)) | Ok(tungstenite::Message::Pong(_)) => continue,
            Ok(_) => continue,
            Err(e) => {
                warn!(client = %client_id, error = %e, "WS read error");
                break;
            }
        };

        let parsed = match serde_json::from_str::<ClientMessage>(&msg) {
            Ok(m) => m,
            Err(e) => {
                send_error(&state, &client_id, &format!("invalid message: {}", e)).await;
                continue;
            }
        };

        match parsed {
            ClientMessage::Auth { .. } => {
                send_error(&state, &client_id, "already authenticated").await;
            }

            ClientMessage::JoinChannel { channel_id } => {
                match state.join_channel(&client_id, &channel_id).await {
                    Ok(members) => {
                        let reply = ServerMessage::ChannelJoined {
                            channel_id,
                            members,
                        };
                        send_to_client(&state, &client_id, &reply).await;
                    }
                    Err(e) => {
                        send_error(&state, &client_id, &e).await;
                    }
                }
            }

            ClientMessage::LeaveChannel { channel_id } => {
                match state.leave_channel(&client_id, &channel_id).await {
                    Ok(()) => {
                        let reply = ServerMessage::ChannelLeft { channel_id };
                        send_to_client(&state, &client_id, &reply).await;
                    }
                    Err(e) => {
                        send_error(&state, &client_id, &e).await;
                    }
                }
            }

            ClientMessage::StartTransmit { channel_id } => {
                if let Err(e) = state.start_transmit(&client_id, &channel_id).await {
                    send_error(&state, &client_id, &e).await;
                }
            }

            ClientMessage::StopTransmit { channel_id } => {
                if let Err(e) = state.stop_transmit(&client_id, &channel_id).await {
                    send_error(&state, &client_id, &e).await;
                }
            }

            ClientMessage::Ping { timestamp } => {
                let server_time = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis() as u64;
                let reply = ServerMessage::Pong {
                    timestamp,
                    server_time,
                };
                send_to_client(&state, &client_id, &reply).await;
            }
        }
    }

    // Cleanup on disconnect.
    state.remove_client(&client_id).await;
    Ok(())
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Derive a deterministic `u64` from a UUID string for use as the UDP client
/// ID in routing headers.
fn derive_udp_id(uuid_str: &str) -> u64 {
    let uuid = uuid::Uuid::parse_str(uuid_str).expect("invalid uuid");
    let bytes = uuid.as_bytes();
    u64::from_be_bytes([
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
    ])
}

async fn send_to_client(state: &ServerState, client_id: &str, msg: &ServerMessage) {
    if let Some(client) = state.clients.get(client_id) {
        if let Err(e) = client.send(msg).await {
            warn!(client = %client_id, error = %e, "failed to send to client");
        }
    }
}

async fn send_error(state: &ServerState, client_id: &str, message: &str) {
    let msg = ServerMessage::Error {
        message: message.to_string(),
    };
    send_to_client(state, client_id, &msg).await;
}
