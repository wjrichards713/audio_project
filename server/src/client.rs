//! Per-connection client state.

use std::net::SocketAddr;
use std::sync::Arc;

use futures_util::SinkExt;
use tokio::sync::Mutex;
use tungstenite::Message;

use crate::protocol::ServerMessage;

/// Alias for the write-half of a tokio-tungstenite WebSocket stream.
pub type WsSender = futures_util::stream::SplitSink<
    tokio_tungstenite::WebSocketStream<tokio::net::TcpStream>,
    Message,
>;

/// Represents a single connected client.
pub struct Client {
    /// Unique identifier (UUID v4, hex string).
    pub id: String,

    /// Human-readable display name.
    pub user_name: String,

    /// The channel this client is currently in, if any.
    /// A client may only be in one channel at a time in this PoC.
    pub channel_id: Option<String>,

    /// Whether the client is currently transmitting audio.
    pub speaking: bool,

    /// The client's UDP endpoint, learned from the first UDP packet we receive.
    pub udp_addr: Option<SocketAddr>,

    /// Numeric client ID used in the UDP routing header (derived from the UUID).
    pub udp_client_id: u64,

    /// WebSocket write half, behind a mutex so we can send from any task.
    ws_sender: Arc<Mutex<WsSender>>,
}

impl Client {
    pub fn new(id: String, udp_client_id: u64, user_name: String, ws_sender: WsSender) -> Self {
        Self {
            id,
            user_name,
            channel_id: None,
            speaking: false,
            udp_addr: None,
            udp_client_id,
            ws_sender: Arc::new(Mutex::new(ws_sender)),
        }
    }

    /// Send a [`ServerMessage`] over the WebSocket connection.
    pub async fn send(&self, msg: &ServerMessage) -> Result<(), tungstenite::Error> {
        let text = serde_json::to_string(msg).expect("ServerMessage serialization cannot fail");
        let mut sender = self.ws_sender.lock().await;
        sender.send(Message::Text(text.into())).await
    }

    /// Get a cloneable handle to the sender for use in broadcast helpers.
    pub fn sender_handle(&self) -> Arc<Mutex<WsSender>> {
        Arc::clone(&self.ws_sender)
    }
}
