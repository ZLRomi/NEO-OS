//! NEO-OS-3 minimal DNS resolver.
//!
//! Sends a DNS A record query via UDP and parses the response.
//! Uses QEMU's built-in DNS (10.0.2.3) or configurable server.

use crate::kinfo;

/// Default DNS server (QEMU user-mode networking).
const DNS_SERVER: [u8; 4] = [10, 0, 2, 3];
const DNS_PORT: u16 = 53;
const OUR_IP: [u8; 4] = [10, 0, 2, 15];
const OUR_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
const SRC_PORT: u16 = 53535;

/// Build a DNS A record query packet.
/// Returns total packet length (Eth + IP + UDP + DNS).
fn build_dns_query(buf: &mut [u8], name: &[u8]) -> usize {
    // Encode domain name in DNS format: length-prefixed labels.
    let mut dns_name = [0u8; 64];
    let mut dns_name_len = 0usize;
    let mut label_start = 0;
    for i in 0..=name.len() {
        if i == name.len() || name[i] == b'.' {
            let label_len = i - label_start;
            if label_len > 0 && label_len <= 63 {
                dns_name[dns_name_len] = label_len as u8;
                dns_name_len += 1;
                for j in label_start..i {
                    dns_name[dns_name_len] = name[j];
                    dns_name_len += 1;
                }
            }
            label_start = i + 1;
        }
    }
    dns_name[dns_name_len] = 0; // root label
    dns_name_len += 1;

    // DNS header (12 bytes) + question section.
    let dns_len = 12 + dns_name_len + 4; // header + name + QTYPE(2) + QCLASS(2)
    let udp_len = 8 + dns_len;
    let ip_len = 20 + udp_len;
    let total_len = 14 + ip_len;

    if buf.len() < total_len { return 0; }

    // Ethernet header.
    buf[0..6].copy_from_slice(&[0xFF; 6]); // broadcast
    buf[6..12].copy_from_slice(&OUR_MAC);
    buf[12] = 0x08; buf[13] = 0x00;

    // IP header.
    crate::ip::build_ipv4_header(&mut buf[14..], ip_len as u16, 17, OUR_IP, DNS_SERVER);

    // UDP header.
    let udp_start = 14 + 20;
    buf[udp_start] = (SRC_PORT >> 8) as u8;
    buf[udp_start + 1] = SRC_PORT as u8;
    buf[udp_start + 2] = (DNS_PORT >> 8) as u8;
    buf[udp_start + 3] = DNS_PORT as u8;
    buf[udp_start + 4] = (udp_len >> 8) as u8;
    buf[udp_start + 5] = udp_len as u8;
    buf[udp_start + 6] = 0; buf[udp_start + 7] = 0; // checksum (optional for UDP)

    // DNS header.
    let dns_start = udp_start + 8;
    buf[dns_start] = 0x12; buf[dns_start + 1] = 0x34; // Transaction ID
    buf[dns_start + 2] = 0x01; buf[dns_start + 3] = 0x00; // Flags: standard query, RD=1
    buf[dns_start + 4] = 0x00; buf[dns_start + 5] = 0x01; // QDCOUNT = 1
    buf[dns_start + 6] = 0x00; buf[dns_start + 7] = 0x00; // ANCOUNT = 0
    buf[dns_start + 8] = 0x00; buf[dns_start + 9] = 0x00; // NSCOUNT = 0
    buf[dns_start + 10] = 0x00; buf[dns_start + 11] = 0x00; // ARCOUNT = 0

    // Question section.
    let q_start = dns_start + 12;
    buf[q_start..q_start + dns_name_len].copy_from_slice(&dns_name[..dns_name_len]);
    let after_name = q_start + dns_name_len;
    buf[after_name] = 0x00; buf[after_name + 1] = 0x01; // QTYPE = A
    buf[after_name + 2] = 0x00; buf[after_name + 3] = 0x01; // QCLASS = IN

    total_len
}

/// Parse a DNS response to extract the first A record IP address.
/// Returns Some([a,b,c,d]) on success, None on failure.
pub fn parse_dns_response(pkt: &[u8]) -> Option<[u8; 4]> {
    // Minimum: Eth(14) + IP(20) + UDP(8) + DNS header(12) = 54
    if pkt.len() < 54 { return None; }

    // Check it's a UDP packet from DNS server.
    if pkt[23] != 17 { return None; } // Not UDP

    let ip_hdr_len = (pkt[14] & 0x0F) as usize * 4;
    let udp_start = 14 + ip_hdr_len;
    let dns_start = udp_start + 8;

    if pkt.len() < dns_start + 12 { return None; }

    // Check transaction ID matches.
    if pkt[dns_start] != 0x12 || pkt[dns_start + 1] != 0x34 { return None; }

    // Check QR bit (response) and RCODE (no error).
    let flags = ((pkt[dns_start + 2] as u16) << 8) | pkt[dns_start + 3] as u16;
    if flags & 0x8000 == 0 { return None; } // Not a response
    if flags & 0x000F != 0 { return None; } // Error code

    let ancount = ((pkt[dns_start + 6] as u16) << 8) | pkt[dns_start + 7] as u16;
    if ancount == 0 { return None; }

    // Skip question section to reach answers.
    let mut pos = dns_start + 12;
    // Skip QNAME.
    while pos < pkt.len() {
        let label_len = pkt[pos] as usize;
        if label_len == 0 { pos += 1; break; }
        if label_len & 0xC0 == 0xC0 { pos += 2; break; } // compressed
        pos += 1 + label_len;
    }
    pos += 4; // QTYPE + QCLASS

    // Parse first answer.
    if pos + 12 > pkt.len() { return None; }

    // Skip NAME (might be compressed pointer).
    if pkt[pos] & 0xC0 == 0xC0 {
        pos += 2; // compressed pointer
    } else {
        while pos < pkt.len() {
            let label_len = pkt[pos] as usize;
            if label_len == 0 { pos += 1; break; }
            pos += 1 + label_len;
        }
    }

    if pos + 10 > pkt.len() { return None; }

    let rtype = ((pkt[pos] as u16) << 8) | pkt[pos + 1] as u16;
    let rdlength = ((pkt[pos + 8] as u16) << 8) | pkt[pos + 9] as u16;
    pos += 10;

    if rtype != 1 || rdlength != 4 { return None; } // Not an A record
    if pos + 4 > pkt.len() { return None; }

    Some([pkt[pos], pkt[pos + 1], pkt[pos + 2], pkt[pos + 3]])
}

/// Resolve a hostname to an IP address.
/// Sends a DNS query and polls for the response.
pub fn resolve(name: &[u8]) -> Option<[u8; 4]> {
    let mut pkt = [0u8; 256];
    let len = build_dns_query(&mut pkt, name);
    if len == 0 { return None; }

    kinfo!("dns: querying {} via {}.{}.{}.{}", 
           core::str::from_utf8(name).unwrap_or("?"),
           DNS_SERVER[0], DNS_SERVER[1], DNS_SERVER[2], DNS_SERVER[3]);

    if crate::virtio_net::send(&pkt[..len]).is_err() {
        return None;
    }

    // Poll for response (up to 2 seconds).
    for _ in 0..200 {
        if let Some((rx_pkt, _)) = crate::virtio_net::poll() {
            if let Some(ip) = parse_dns_response(rx_pkt) {
                kinfo!("dns: resolved to {}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3]);
                return Some(ip);
            }
        }
        crate::interrupts::sleep_ms(10);
    }

    kinfo!("dns: resolution timed out");
    None
}
