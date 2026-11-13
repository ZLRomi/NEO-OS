//! NEO-OS-3 TCP stack.
//!
//! Single outgoing TCP connection with full send/receive support:
//! - SYN → SYN-ACK → ACK handshake
//! - Data send (PSH+ACK) with sequence tracking
//! - Data receive with ACK reply and ring buffer
//! - FIN handling for graceful close

use crate::kinfo;

/// TCP connection states.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum TcpState {
    Closed,
    SynSent,
    Established,
    FinWait,
    TimeWait,
}

/// Receive ring buffer size.
const RECV_BUF_SIZE: usize = 4096;

/// Minimal TCP connection context.
pub struct TcpConn {
    pub state: TcpState,
    pub local_port: u16,
    pub remote_port: u16,
    pub seq: u32,
    pub ack: u32,
    pub remote_ip: [u8; 4],
    pub local_ip: [u8; 4],
    /// Receive ring buffer.
    recv_buf: [u8; RECV_BUF_SIZE],
    recv_head: usize,
    recv_tail: usize,
    recv_count: usize,
    /// Send window tracking.
    send_window: u32,     // peer's advertised window size
    last_acked: u32,      // last sequence number acknowledged by peer
    /// Retransmission tracking.
    unacked_seq: u32,     // seq of first unacknowledged byte
    unacked_data: [u8; 512], // copy of unacknowledged data (max one segment)
    unacked_len: usize,   // length of unacknowledged data
    retransmit_tick: u64, // tick when data was sent (for timeout)
    retransmit_active: bool,
}

impl TcpConn {
    pub const fn new() -> Self {
        Self {
            state: TcpState::Closed,
            local_port: 0,
            remote_port: 0,
            seq: 0,
            ack: 0,
            remote_ip: [0; 4],
            local_ip: [0; 4],
            recv_buf: [0; RECV_BUF_SIZE],
            recv_head: 0,
            recv_tail: 0,
            recv_count: 0,
            send_window: 65535, // default window until peer advertises
            last_acked: 0,
            unacked_seq: 0,
            unacked_data: [0; 512],
            unacked_len: 0,
            retransmit_tick: 0,
            retransmit_active: false,
        }
    }

    /// Initiate a connection (send SYN).
    pub fn connect(&mut self, remote_ip: [u8; 4], remote_port: u16, local_port: u16) {
        self.remote_ip = remote_ip;
        self.remote_port = remote_port;
        self.local_port = local_port;
        self.local_ip = [10, 0, 2, 15]; // QEMU default
        self.seq = 0x12345678;
        self.ack = 0;
        self.state = TcpState::SynSent;
        self.recv_head = 0;
        self.recv_tail = 0;
        self.recv_count = 0;
        self.send_window = 65535;
        self.last_acked = self.seq;
        self.unacked_seq = 0;
        self.unacked_len = 0;
        self.retransmit_active = false;
        kinfo!("tcp: SYN sent to {}.{}.{}.{}:{} from port {}",
            remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3],
            remote_port, local_port);
    }

    /// Handle a received TCP segment. Returns true if state changed.
    pub fn handle_segment(&mut self, _src_ip: [u8; 4], src_port: u16,
                          dst_port: u16, flags: u8, seq: u32, ack: u32,
                          window: u16, payload: &[u8]) -> bool {
        // Only process segments for our connection.
        if src_port != self.remote_port || dst_port != self.local_port {
            return false;
        }

        match self.state {
            TcpState::SynSent => {
                // Expecting SYN+ACK.
                if flags & 0x12 == 0x12 && ack == self.seq.wrapping_add(1) {
                    self.ack = seq.wrapping_add(1);
                    self.seq = self.seq.wrapping_add(1);
                    self.state = TcpState::Established;
                    send_ack(self);
                    kinfo!("tcp: connection established");
                    return true;
                }
                false
            }
            TcpState::Established => {
                // Update send window from peer's advertisement.
                self.send_window = window as u32;

                // Handle RST — immediate connection reset.
                if flags & 0x04 != 0 {
                    self.state = TcpState::Closed;
                    kinfo!("tcp: received RST, connection closed");
                    return true;
                }

                // Handle incoming data (PSH+ACK or just ACK with data).
                if !payload.is_empty() && seq == self.ack {
                    let mut copied = 0;
                    for &b in payload {
                        if self.recv_count >= RECV_BUF_SIZE { break; }
                        self.recv_buf[self.recv_head] = b;
                        self.recv_head = (self.recv_head + 1) % RECV_BUF_SIZE;
                        self.recv_count += 1;
                        copied += 1;
                    }
                    self.ack = seq.wrapping_add(copied as u32);
                    send_ack(self);
                    if copied > 0 {
                        kinfo!("tcp: received {} bytes", copied);
                    }
                    return copied > 0;
                }

                // Handle FIN from peer (passive close).
                if flags & 0x01 != 0 {
                    self.ack = seq.wrapping_add(1);
                    send_ack(self);
                    // Send our own FIN to complete the close.
                    send_fin(self);
                    self.state = TcpState::FinWait;
                    kinfo!("tcp: received FIN, sent FIN+ACK");
                    return true;
                }

                // Pure ACK — update last_acked and clear retransmit timer.
                if flags & 0x10 != 0 && payload.is_empty() {
                    if ack > self.last_acked {
                        self.last_acked = ack;
                        // If all sent data is acknowledged, stop retransmit timer.
                        if ack >= self.unacked_seq.wrapping_add(self.unacked_len as u32) {
                            self.retransmit_active = false;
                            self.unacked_len = 0;
                        }
                    }
                }
                false
            }
            TcpState::FinWait => {
                // Waiting for final ACK of our FIN.
                if flags & 0x04 != 0 {
                    // RST during close.
                    self.state = TcpState::Closed;
                    kinfo!("tcp: RST during FIN_WAIT");
                    return true;
                }
                if flags & 0x10 != 0 && ack == self.seq {
                    // Our FIN was acknowledged.
                    self.state = TcpState::Closed;
                    kinfo!("tcp: connection fully closed");
                    return true;
                }
                // Also handle late data or duplicate FIN.
                if flags & 0x01 != 0 {
                    self.ack = seq.wrapping_add(1);
                    send_ack(self);
                }
                false
            }
            TcpState::Closed | TcpState::TimeWait => false,
        }
    }

    /// Read received data into buf. Returns bytes read.
    pub fn recv(&mut self, buf: &mut [u8]) -> usize {
        let mut n = 0;
        while n < buf.len() && self.recv_count > 0 {
            buf[n] = self.recv_buf[self.recv_tail];
            self.recv_tail = (self.recv_tail + 1) % RECV_BUF_SIZE;
            self.recv_count -= 1;
            n += 1;
        }
        n
    }

    /// Check if there is data available to read.
    pub fn has_data(&self) -> bool {
        self.recv_count > 0
    }
}

/// Maximum concurrent TCP connections.
const MAX_CONNECTIONS: usize = 4;

/// Global TCP connection table.
static mut TCP_CONNS: [TcpConn; MAX_CONNECTIONS] = {
    const EMPTY: TcpConn = TcpConn::new();
    [EMPTY; MAX_CONNECTIONS]
};

/// Find a connection by 4-tuple, or return None.
fn find_conn(remote_ip: [u8; 4], remote_port: u16, local_port: u16) -> Option<usize> {
    unsafe {
        for i in 0..MAX_CONNECTIONS {
            let c = &TCP_CONNS[i];
            if c.state != TcpState::Closed
                && c.remote_ip == remote_ip
                && c.remote_port == remote_port
                && c.local_port == local_port
            {
                return Some(i);
            }
        }
    }
    None
}

/// Find an empty slot in the connection table.
fn find_free_slot() -> Option<usize> {
    unsafe {
        for i in 0..MAX_CONNECTIONS {
            if TCP_CONNS[i].state == TcpState::Closed {
                return Some(i);
            }
        }
    }
    None
}

/// Handle a received TCP segment. Routes to the correct connection.
pub fn handle_segment(src_ip: [u8; 4], src_port: u16, dst_port: u16,
                      flags: u8, seq: u32, ack: u32, window: u16, payload: &[u8]) -> bool {
    unsafe {
        // Try to find matching connection (src_ip is remote, dst_port is local).
        if let Some(idx) = find_conn(src_ip, src_port, dst_port) {
            return TCP_CONNS[idx].handle_segment(src_ip, src_port, dst_port, flags, seq, ack, window, payload);
        }
        // No matching connection — ignore (could send RST in future).
    }
    false
}

/// Get index of first active connection, or None.
fn first_active() -> Option<usize> {
    unsafe {
        for i in 0..MAX_CONNECTIONS {
            if TCP_CONNS[i].state != TcpState::Closed {
                return Some(i);
            }
        }
    }
    None
}

/// Receive data from the first active TCP connection.
pub fn recv(buf: &mut [u8]) -> usize {
    unsafe {
        match first_active() {
            Some(i) => TCP_CONNS[i].recv(buf),
            None => 0,
        }
    }
}

/// Check if there is received data available.
pub fn has_data() -> bool {
    unsafe {
        match first_active() {
            Some(i) => TCP_CONNS[i].has_data(),
            None => false,
        }
    }
}

/// Send an ACK packet for the given connection.
fn send_ack(conn: &TcpConn) {
    let mut pkt = [0u8; 128];
    let src_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
    let dst_mac = [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
    let len = crate::ip::build_tcp_packet(
        &mut pkt,
        dst_mac,
        src_mac,
        conn.local_ip,
        conn.remote_ip,
        conn.local_port,
        conn.remote_port,
        conn.seq,
        conn.ack,
        0x10, // ACK
        64240,
        &[],
    );
    if len == 0 {
        return;
    }
    let _ = crate::virtio_net::send(&pkt[..len]);
}

/// Send a FIN+ACK packet to initiate/complete connection close.
fn send_fin(conn: &mut TcpConn) {
    let mut pkt = [0u8; 128];
    let src_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
    let dst_mac = [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
    let len = crate::ip::build_tcp_packet(
        &mut pkt,
        dst_mac,
        src_mac,
        conn.local_ip,
        conn.remote_ip,
        conn.local_port,
        conn.remote_port,
        conn.seq,
        conn.ack,
        0x11, // FIN + ACK
        64240,
        &[],
    );
    if len > 0 {
        let _ = crate::virtio_net::send(&pkt[..len]);
        conn.seq = conn.seq.wrapping_add(1); // FIN consumes a sequence number
    }
}

/// Send data over the first active TCP connection.
pub fn send_data(data: &[u8]) -> Result<(), &'static str> {
    unsafe {
        let idx = match first_active() {
            Some(i) if TCP_CONNS[i].state == TcpState::Established => i,
            _ => return Err("not connected"),
        };
        let conn = &mut TCP_CONNS[idx];

        // Flow control: check send window.
        let unacked = conn.seq.wrapping_sub(conn.last_acked);
        if data.len() as u32 > conn.send_window.saturating_sub(unacked) {
            return Err("send window full");
        }

        let mut pkt = [0u8; 1600];
        let src_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let dst_mac = [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
        let len = crate::ip::build_tcp_packet(
            &mut pkt,
            dst_mac,
            src_mac,
            conn.local_ip,
            conn.remote_ip,
            conn.local_port,
            conn.remote_port,
            conn.seq,
            conn.ack,
            0x18, // PSH + ACK
            64240,
            data,
        );
        if len == 0 {
            return Err("packet build failed");
        }
        match crate::virtio_net::send(&pkt[..len]) {
            Ok(()) => {
                // Save unacked data for retransmission (up to 512 bytes).
                let save_len = core::cmp::min(data.len(), 512);
                conn.unacked_data[..save_len].copy_from_slice(&data[..save_len]);
                conn.unacked_len = save_len;
                conn.unacked_seq = conn.seq;
                conn.retransmit_tick = crate::interrupts::ticks();
                conn.retransmit_active = true;

                conn.seq = conn.seq.wrapping_add(data.len() as u32);
                kinfo!("tcp: sent {} bytes", data.len());
                Ok(())
            }
            Err(e) => Err(e),
        }
    }
}

/// Initiate a TCP connection. Returns true if SYN was sent.
pub fn connect(remote_ip: [u8; 4], remote_port: u16, local_port: u16) -> bool {
    unsafe {
        let idx = match find_free_slot() {
            Some(i) => i,
            None => return false, // all slots full
        };
        let conn = &mut TCP_CONNS[idx];
        conn.connect(remote_ip, remote_port, local_port);

        let mut pkt = [0u8; 128];
        let src_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let dst_mac = [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
        let len = crate::ip::build_tcp_packet(
            &mut pkt,
            dst_mac,
            src_mac,
            conn.local_ip,
            remote_ip,
            local_port,
            remote_port,
            conn.seq,
            0,
            0x02, // SYN
            64240,
            &[],
        );
        if len == 0 {
            kinfo!("tcp: packet build failed");
            return false;
        }
        match crate::virtio_net::send(&pkt[..len]) {
            Ok(()) => {
                kinfo!("tcp: SYN sent ({} bytes)", len);
                true
            }
            Err(_) => {
                kinfo!("tcp: SYN send failed");
                false
            }
        }
    }
}

/// Check if retransmission is needed and resend unacked data.
/// Should be called periodically from the network poll loop.
/// RTO = 1 second (100 ticks at 10ms/tick).
pub fn check_retransmit() {
    unsafe {
        for idx in 0..MAX_CONNECTIONS {
            let conn = &mut TCP_CONNS[idx];
            if !conn.retransmit_active || conn.state != TcpState::Established {
                continue;
            }
        let now = crate::interrupts::ticks();
        let elapsed = now.wrapping_sub(conn.retransmit_tick);
        if elapsed >= 100 { // 1 second RTO
            if conn.unacked_len > 0 {
                kinfo!("tcp: retransmitting {} bytes (seq={})", conn.unacked_len, conn.unacked_seq);
                // Resend the unacked data.
                let mut pkt = [0u8; 1600];
                let src_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
                let dst_mac = [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];
                let len = crate::ip::build_tcp_packet(
                    &mut pkt, dst_mac, src_mac,
                    conn.local_ip, conn.remote_ip,
                    conn.local_port, conn.remote_port,
                    conn.unacked_seq, conn.ack,
                    0x18, 64240,
                    &conn.unacked_data[..conn.unacked_len],
                );
                if len > 0 {
                    let _ = crate::virtio_net::send(&pkt[..len]);
                }
                // Reset timer (exponential backoff could be added here).
                conn.retransmit_tick = now;
            }
        }
        } // end for
    }
}

/// Actively close the first active TCP connection (send FIN).
pub fn close() {
    unsafe {
        if let Some(idx) = first_active() {
            if TCP_CONNS[idx].state == TcpState::Established {
                send_fin(&mut TCP_CONNS[idx]);
                TCP_CONNS[idx].state = TcpState::FinWait;
                kinfo!("tcp: active close initiated (FIN sent)");
            }
        }
    }
}

/// Reset all connections to Closed state.
pub fn reset() {
    unsafe {
        for i in 0..MAX_CONNECTIONS {
            TCP_CONNS[i].state = TcpState::Closed;
            TCP_CONNS[i].recv_head = 0;
            TCP_CONNS[i].recv_tail = 0;
            TCP_CONNS[i].recv_count = 0;
            TCP_CONNS[i].retransmit_active = false;
        }
    }
}

/// Check if any TCP connection is established.
pub fn is_established() -> bool {
    unsafe {
        for i in 0..MAX_CONNECTIONS {
            if TCP_CONNS[i].state == TcpState::Established {
                return true;
            }
        }
        false
    }
}

/// Get state name of the first active connection.
pub fn state_name() -> &'static str {
    unsafe {
        match first_active() {
            Some(i) => match TCP_CONNS[i].state {
                TcpState::Closed => "closed",
                TcpState::SynSent => "syn-sent",
                TcpState::Established => "established",
                TcpState::FinWait => "fin-wait",
                TcpState::TimeWait => "time-wait",
            },
            None => "closed",
        }
    }
}

/// Dump all TCP connections to kprint output.
pub fn dump_connections() {
    unsafe {
        let mut active = 0usize;
        for i in 0..MAX_CONNECTIONS {
            let c = &TCP_CONNS[i];
            if c.state == TcpState::Closed { continue; }
            active += 1;
            let state_str = match c.state {
                TcpState::Closed => "CLOSED",
                TcpState::SynSent => "SYN_SENT",
                TcpState::Established => "ESTABLISHED",
                TcpState::FinWait => "FIN_WAIT",
                TcpState::TimeWait => "TIME_WAIT",
            };
            kinfo!("tcp[{}]: local:{} -> {}.{}.{}.{}:{} {} seq={} ack={} win={}",
                   i,
                   c.local_port,
                   c.remote_ip[0], c.remote_ip[1], c.remote_ip[2], c.remote_ip[3],
                   c.remote_port,
                   state_str,
                   c.seq, c.ack, c.send_window);
        }
        if active == 0 {
            kinfo!("tcp: no active connections");
        } else {
            kinfo!("tcp: {} active connection(s)", active);
        }
    }
}
