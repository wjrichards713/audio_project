//! Audio SFU server — proof of concept.
//!
//! Starts three listeners:
//! - **UDP 10000** — audio packet receive and forward
//! - **WebSocket 8080** — signaling (JSON)
//! - **HTTP 9090** — health check

mod channel;
mod client;
mod protocol;
mod udp_handler;
mod ws_handler;

use std::sync::Arc;

use tokio::io::AsyncWriteExt;
use tokio::net::{TcpListener, UdpSocket};
use tracing::{error, info};

const UDP_PORT: u16 = 10000;
const WS_PORT: u16 = 8080;
const HEALTH_PORT: u16 = 9090;

#[tokio::main]
async fn main() {
    // Initialise structured logging.  Respect RUST_LOG env var, defaulting to
    // `info` for our crate and `warn` for everything else.
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| {
                "audio_sfu=info,warn".into()
            }),
        )
        .init();

    info!("starting audio SFU server (proof of concept)");

    // Shared state.
    let state = channel::ServerState::new(UDP_PORT);

    // Bind UDP socket.
    let udp_socket = UdpSocket::bind(("0.0.0.0", UDP_PORT))
        .await
        .expect("failed to bind UDP socket");
    let udp_socket = Arc::new(udp_socket);
    info!(port = UDP_PORT, "UDP listener bound");

    // Bind WebSocket TCP listener.
    let ws_listener = TcpListener::bind(("0.0.0.0", WS_PORT))
        .await
        .expect("failed to bind WebSocket listener");
    info!(port = WS_PORT, "WebSocket listener bound");

    // Bind health check TCP listener.
    let health_listener = TcpListener::bind(("0.0.0.0", HEALTH_PORT))
        .await
        .expect("failed to bind health check listener");
    info!(port = HEALTH_PORT, "health check listener bound");

    // Spawn all three loops.
    let udp_state = Arc::clone(&state);
    let udp_handle = tokio::spawn(async move {
        udp_handler::run_udp_loop(udp_socket, udp_state).await;
    });

    let ws_state = Arc::clone(&state);
    let ws_handle = tokio::spawn(async move {
        ws_handler::run_ws_server(ws_listener, ws_state).await;
    });

    let health_state = Arc::clone(&state);
    let health_handle = tokio::spawn(async move {
        run_health_server(health_listener, health_state).await;
    });

    info!("all listeners started — server is ready");

    // Wait for any task to finish (they shouldn't under normal operation).
    tokio::select! {
        r = udp_handle => { error!("UDP loop exited: {:?}", r); }
        r = ws_handle => { error!("WS loop exited: {:?}", r); }
        r = health_handle => { error!("Health loop exited: {:?}", r); }
    }
}

// ---------------------------------------------------------------------------
// Minimal HTTP health check
// ---------------------------------------------------------------------------

async fn run_health_server(listener: TcpListener, state: Arc<channel::ServerState>) {
    loop {
        let (mut stream, _) = match listener.accept().await {
            Ok(v) => v,
            Err(e) => {
                error!(error = %e, "health accept error");
                continue;
            }
        };

        let state = Arc::clone(&state);
        tokio::spawn(async move {
            // Read the request (we don't actually parse it — any request gets
            // the health response).
            let mut req_buf = [0u8; 1024];
            let _ = tokio::io::AsyncReadExt::read(&mut stream, &mut req_buf).await;

            let connected_clients = state.clients.len();
            let total_channel_members: usize = state
                .channels
                .iter()
                .map(|c| c.members.len())
                .sum();

            let body = serde_json::json!({
                "status": "ok",
                "connected_clients": connected_clients,
                "total_channel_members": total_channel_members,
                "channels": state.channels.iter().map(|c| {
                    serde_json::json!({
                        "name": c.name,
                        "members": c.members.len(),
                    })
                }).collect::<Vec<_>>(),
            });

            let body_str = serde_json::to_string(&body).unwrap();
            let response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body_str.len(),
                body_str
            );

            let _ = stream.write_all(response.as_bytes()).await;
            let _ = stream.shutdown().await;
        });
    }
}
