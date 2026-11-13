//! NEO-OS-3 minimal IPv4 layer.
//!
//! Builds IP headers and calculates checksums for outgoing packets.

/// Calculate the IPv4 header checksum.
/// data: the IP header bytes (checksum field must be zeroed).
/// Returns the 16-bit checksum.
pub fn ipv4_checksum(data: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    for i in (0..data.len()).step_by(2) {
        let w = if i + 1 < data.len() {
            ((data[i] as u16) << 8) | (data[i + 1] as u16)
        } else {
            (data[i] as u16) << 8
        };
        sum += w as u32;
    }
    // Fold carries.
    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

/// Build an IPv4 header into the buffer.
/// Returns the header length (20 bytes for no options).
pub fn build_ipv4_header(
    buf: &mut [u8],
    total_len: u16,
    protocol: u8,
    src_ip: [u8; 4],
    dst_ip: [u8; 4],
) -> usize {
    if buf.len() < 20 {
        return 0;
    }
    buf[0] = 0x45; // version=4, IHL=5 (20 bytes)
    buf[1] = 0x00; // TOS
    buf[2] = (total_len >> 8) as u8;
    buf[3] = total_len as u8;
    buf[4] = 0x00; buf[5] = 0x01; // ID
    buf[6] = 0x40; buf[7] = 0x00; // DF flag
    buf[8] = 64;   // TTL
    buf[9] = protocol;
    buf[10] = 0x00; buf[11] = 0x00; // checksum (filled below)
    buf[12..16].copy_from_slice(&src_ip);
    buf[16..20].copy_from_slice(&dst_ip);

    // Calculate and fill checksum.
    let csum = ipv4_checksum(&buf[..20]);
    buf[10] = (csum >> 8) as u8;
    buf[11] = csum as u8;

    20
}

/// Build an Ethernet + IPv4 + UDP packet into the buffer.
/// Returns total packet length or 0 on failure.
pub fn build_udp_packet(
    buf: &mut [u8],
    dst_mac: [u8; 6],
    src_mac: [u8; 6],
    src_ip: [u8; 4],
    dst_ip: [u8; 4],
    src_port: u16,
    dst_port: u16,
    payload: &[u8],
) -> usize {
    let udp_len = 8 + payload.len();
    let ip_len = 20 + udp_len;
    let total_len = 14 + ip_len;

    if buf.len() < total_len {
        return 0;
    }

    // Ethernet header.
    buf[0..6].copy_from_slice(&dst_mac);
    buf[6..12].copy_from_slice(&src_mac);
    buf[12] = 0x08; buf[13] = 0x00; // IPv4

    // IP header.
    build_ipv4_header(
        &mut buf[14..],
        ip_len as u16,
        17, // UDP
        src_ip,
        dst_ip,
    );

    // UDP header.
    let udp_start = 14 + 20;
    buf[udp_start] = (src_port >> 8) as u8;
    buf[udp_start + 1] = src_port as u8;
    buf[udp_start + 2] = (dst_port >> 8) as u8;
    buf[udp_start + 3] = dst_port as u8;
    buf[udp_start + 4] = (udp_len >> 8) as u8;
    buf[udp_start + 5] = udp_len as u8;
    buf[udp_start + 6] = 0x00; buf[udp_start + 7] = 0x00; // checksum (0 = not used)

    // Payload.
    let payload_start = udp_start + 8;
    buf[payload_start..payload_start + payload.len()].copy_from_slice(payload);

    total_len
}

/// Calculate TCP checksum (pseudo-header + TCP header + data).
pub fn tcp_checksum(src_ip: [u8; 4], dst_ip: [u8; 4], tcp_data: &[u8]) -> u16 {
    let mut sum: u32 = 0;

    // Pseudo-header: src IP + dst IP + zero + protocol + TCP length.
    for i in (0..4).step_by(2) {
        sum += ((src_ip[i] as u32) << 8) | src_ip[i + 1] as u32;
        sum += ((dst_ip[i] as u32) << 8) | dst_ip[i + 1] as u32;
    }
    sum += 6; // protocol = TCP
    sum += tcp_data.len() as u32;

    // TCP header + data.
    for i in (0..tcp_data.len()).step_by(2) {
        let w = if i + 1 < tcp_data.len() {
            ((tcp_data[i] as u32) << 8) | (tcp_data[i + 1] as u32)
        } else {
            (tcp_data[i] as u32) << 8
        };
        sum += w;
    }

    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

/// Build an Ethernet + IPv4 + TCP packet into the buffer.
/// Returns total packet length or 0 on failure.
pub fn build_tcp_packet(
    buf: &mut [u8],
    dst_mac: [u8; 6],
    src_mac: [u8; 6],
    src_ip: [u8; 4],
    dst_ip: [u8; 4],
    src_port: u16,
    dst_port: u16,
    seq: u32,
    ack: u32,
    flags: u8,
    window: u16,
    payload: &[u8],
) -> usize {
    let tcp_len = 20 + payload.len();
    let ip_len = 20 + tcp_len;
    let total_len = 14 + ip_len;

    if buf.len() < total_len {
        return 0;
    }

    // Ethernet header.
    buf[0..6].copy_from_slice(&dst_mac);
    buf[6..12].copy_from_slice(&src_mac);
    buf[12] = 0x08; buf[13] = 0x00;

    // IP header.
    build_ipv4_header(
        &mut buf[14..],
        ip_len as u16,
        6, // TCP
        src_ip,
        dst_ip,
    );

    // TCP header.
    let tcp_start = 14 + 20;
    buf[tcp_start] = (src_port >> 8) as u8;
    buf[tcp_start + 1] = src_port as u8;
    buf[tcp_start + 2] = (dst_port >> 8) as u8;
    buf[tcp_start + 3] = dst_port as u8;
    buf[tcp_start + 4] = (seq >> 24) as u8;
    buf[tcp_start + 5] = (seq >> 16) as u8;
    buf[tcp_start + 6] = (seq >> 8) as u8;
    buf[tcp_start + 7] = seq as u8;
    buf[tcp_start + 8] = (ack >> 24) as u8;
    buf[tcp_start + 9] = (ack >> 16) as u8;
    buf[tcp_start + 10] = (ack >> 8) as u8;
    buf[tcp_start + 11] = ack as u8;
    buf[tcp_start + 12] = 0x50; // data offset = 5 (20 bytes)
    buf[tcp_start + 13] = flags;
    buf[tcp_start + 14] = (window >> 8) as u8;
    buf[tcp_start + 15] = window as u8;
    buf[tcp_start + 16] = 0x00; buf[tcp_start + 17] = 0x00; // checksum
    buf[tcp_start + 18] = 0x00; buf[tcp_start + 19] = 0x00; // urgent

    // Payload.
    let payload_start = tcp_start + 20;
    buf[payload_start..payload_start + payload.len()].copy_from_slice(payload);

    // TCP checksum.
    let csum = tcp_checksum(src_ip, dst_ip, &buf[tcp_start..payload_start + payload.len()]);
    buf[tcp_start + 16] = (csum >> 8) as u8;
    buf[tcp_start + 17] = csum as u8;

    total_len
}
