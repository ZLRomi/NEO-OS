//! NEO-OS-3 ARP (Address Resolution Protocol) implementation.
//!
//! Resolves IPv4 addresses to MAC addresses on the local network.
//! Maintains a small cache of recent resolutions.
//!
//! ARP packet format (28 bytes for IPv4/Ethernet):
//!   Hardware type (2) = 1 (Ethernet)
//!   Protocol type (2) = 0x0800 (IPv4)
//!   HW addr len (1) = 6
//!   Proto addr len (1) = 4
//!   Operation (2) = 1 (request) / 2 (reply)
//!   Sender MAC (6)
//!   Sender IP (4)
//!   Target MAC (6)
//!   Target IP (4)

use crate::kinfo;

/// ARP cache entry.
struct ArpEntry {
    ip: [u8; 4],
    mac: [u8; 6],
    valid: bool,
}

const ARP_CACHE_SIZE: usize = 8;
static mut ARP_CACHE: [ArpEntry; ARP_CACHE_SIZE] = {
    const EMPTY: ArpEntry = ArpEntry {
        ip: [0; 4],
        mac: [0; 6],
        valid: false,
    };
    [EMPTY; ARP_CACHE_SIZE]
};

/// Our IP and MAC addresses.
const OUR_IP: [u8; 4] = [10, 0, 2, 15]; // QEMU default
const OUR_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];

/// Look up a MAC address in the ARP cache.
pub fn lookup(ip: [u8; 4]) -> Option<[u8; 6]> {
    unsafe {
        for entry in &ARP_CACHE {
            if entry.valid && entry.ip == ip {
                return Some(entry.mac);
            }
        }
    }
    None
}

/// Add or update an entry in the ARP cache.
fn cache_update(ip: [u8; 4], mac: [u8; 6]) {
    unsafe {
        // Try to update existing entry.
        for entry in ARP_CACHE.iter_mut() {
            if entry.valid && entry.ip == ip {
                entry.mac = mac;
                return;
            }
        }
        // Find empty slot.
        for entry in ARP_CACHE.iter_mut() {
            if !entry.valid {
                entry.ip = ip;
                entry.mac = mac;
                entry.valid = true;
                return;
            }
        }
        // Cache full: evict oldest (first slot).
        ARP_CACHE[0].ip = ip;
        ARP_CACHE[0].mac = mac;
        ARP_CACHE[0].valid = true;
    }
}

/// Build an ARP request packet (42 bytes: Eth(14) + ARP(28)).
fn build_arp_request(buf: &mut [u8], target_ip: [u8; 4]) -> usize {
    if buf.len() < 42 { return 0; }

    // Ethernet header.
    buf[0..6].copy_from_slice(&[0xFF; 6]); // broadcast dst
    buf[6..12].copy_from_slice(&OUR_MAC);
    buf[12] = 0x08; buf[13] = 0x06; // EtherType = ARP

    // ARP payload.
    buf[14] = 0x00; buf[15] = 0x01; // Hardware type = Ethernet
    buf[16] = 0x08; buf[17] = 0x00; // Protocol type = IPv4
    buf[18] = 6;                     // HW addr len
    buf[19] = 4;                     // Proto addr len
    buf[20] = 0x00; buf[21] = 0x01; // Operation = request
    buf[22..28].copy_from_slice(&OUR_MAC);
    buf[28..32].copy_from_slice(&OUR_IP);
    buf[32..38].copy_from_slice(&[0; 6]); // target MAC (unknown)
    buf[38..42].copy_from_slice(&target_ip);

    42
}

/// Send an ARP request for the given IP address.
pub fn send_request(target_ip: [u8; 4]) {
    let mut pkt = [0u8; 64];
    let len = build_arp_request(&mut pkt, target_ip);
    if len > 0 {
        let _ = crate::virtio_net::send(&pkt[..len]);
        kinfo!("arp: request sent for {}.{}.{}.{}", 
               target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
    }
}

/// Process a received ARP packet. Called from the network RX path.
/// `pkt` is the raw Ethernet frame.
pub fn handle_packet(pkt: &[u8]) {
    if pkt.len() < 42 { return; }
    // Check EtherType = ARP (0x0806).
    if pkt[12] != 0x08 || pkt[13] != 0x06 { return; }

    let arp = &pkt[14..];
    // Verify hardware=Ethernet(1), protocol=IPv4(0x0800), hw_len=6, proto_len=4.
    if arp[0] != 0x00 || arp[1] != 0x01 { return; }
    if arp[2] != 0x08 || arp[3] != 0x00 { return; }
    if arp[4] != 6 || arp[5] != 4 { return; }

    let op = u16::from_be_bytes([arp[6], arp[7]]);
    let sender_mac: [u8; 6] = [arp[8], arp[9], arp[10], arp[11], arp[12], arp[13]];
    let sender_ip: [u8; 4] = [arp[14], arp[15], arp[16], arp[17]];
    let target_ip: [u8; 4] = [arp[24], arp[25], arp[26], arp[27]];

    // Always cache the sender's mapping.
    cache_update(sender_ip, sender_mac);

    if op == 1 && target_ip == OUR_IP {
        // ARP request for us — send reply.
        kinfo!("arp: request from {}.{}.{}.{}, sending reply",
               sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3]);
        send_reply(sender_ip, sender_mac);
    } else if op == 2 {
        // ARP reply — already cached above.
        kinfo!("arp: reply from {}.{}.{}.{} -> {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
               sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3],
               sender_mac[0], sender_mac[1], sender_mac[2],
               sender_mac[3], sender_mac[4], sender_mac[5]);
    }
}

/// Send an ARP reply to the given host.
fn send_reply(target_ip: [u8; 4], target_mac: [u8; 6]) {
    let mut pkt = [0u8; 64];
    if pkt.len() < 42 { return; }

    // Ethernet header.
    pkt[0..6].copy_from_slice(&target_mac);
    pkt[6..12].copy_from_slice(&OUR_MAC);
    pkt[12] = 0x08; pkt[13] = 0x06;

    // ARP reply.
    pkt[14] = 0x00; pkt[15] = 0x01;
    pkt[16] = 0x08; pkt[17] = 0x00;
    pkt[18] = 6; pkt[19] = 4;
    pkt[20] = 0x00; pkt[21] = 0x02; // reply
    pkt[22..28].copy_from_slice(&OUR_MAC);
    pkt[28..32].copy_from_slice(&OUR_IP);
    pkt[32..38].copy_from_slice(&target_mac);
    pkt[38..42].copy_from_slice(&target_ip);

    let _ = crate::virtio_net::send(&pkt[..42]);
}

/// Dump the ARP cache.
pub fn dump() {
    kinfo!("arp: cache:");
    unsafe {
        let mut count = 0;
        for entry in &ARP_CACHE {
            if entry.valid {
                kinfo!("  {}.{}.{}.{} -> {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                       entry.ip[0], entry.ip[1], entry.ip[2], entry.ip[3],
                       entry.mac[0], entry.mac[1], entry.mac[2],
                       entry.mac[3], entry.mac[4], entry.mac[5]);
                count += 1;
            }
        }
        if count == 0 {
            kinfo!("  (empty)");
        }
    }
}
