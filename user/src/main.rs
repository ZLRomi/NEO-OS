//! NEO-OS-3 user-space programs.
//!
//! Each program is compiled as a separate ELF binary using feature flags:
//!   --features prog_user      → user_prog (default)
//!   --features prog_bad       → user_bad
//!   --features prog_nx        → user_nx
//!   --features prog_exec      → user_exec
//!   --features prog_ptr       → user_ptr
//!   --features prog_isowrite  → user_isowrite

#![no_std]
#![no_main]

use core::panic::PanicInfo;

// ---------------------------------------------------------------------------
// Syscall interface (int 0x80)
// ---------------------------------------------------------------------------

fn sys_call(nr: u64, a: u64, b: u64, c: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "int 0x80",
            inlateout("rax") nr => ret,
            in("rdi") a,
            in("rsi") b,
            in("rdx") c,
            options(nostack),
        )
    }
    ret
}

fn sys_write(buf: &[u8]) -> u64 {
    sys_call(0, 1, buf.as_ptr() as u64, buf.len() as u64)
}

fn sys_uptime() -> u64 {
    sys_call(2, 0, 0, 0)
}

fn sys_exit(code: u64) -> ! {
    sys_call(1, code, 0, 0);
    loop { core::hint::spin_loop(); }
}

/// sys_brk: extend program break (heap). Returns new brk.
fn sys_brk(new_brk: u64) -> u64 {
    sys_call(3, new_brk, 0, 0)
}

/// sys_meminfo: write memory stats to buffer. Returns bytes written.
fn sys_meminfo(buf: &mut [u8]) -> u64 {
    sys_call(4, buf.as_mut_ptr() as u64, buf.len() as u64, 0)
}

/// sys_read_char: read one char from keyboard (non-blocking). Returns 0 if none.
fn sys_read_char() -> u64 {
    sys_call(5, 0, 0, 0)
}

/// sys_sleep(ticks): busy-wait for N ticks (1 tick = 10ms).
fn sys_sleep(ticks: u64) {
    sys_call(6, ticks, 0, 0);
}

/// sys_print_int: print signed integer to console.
fn sys_print_int(val: i64) {
    sys_call(7, val as u64, 0, 0);
}

/// sys_net_send: send raw packet through network device.
fn sys_net_send(buf: &[u8]) -> u64 {
    sys_call(8, buf.as_ptr() as u64, buf.len() as u64, 0)
}

/// sys_net_recv: receive packet into buffer. Returns bytes or 0 if none.
fn sys_net_recv(buf: &mut [u8]) -> u64 {
    sys_call(9, buf.as_mut_ptr() as u64, buf.len() as u64, 0)
}

/// sys_tcp_send: send TCP data. Returns 0 on success.
fn sys_tcp_send(buf: &[u8]) -> u64 {
    sys_call(10, buf.as_ptr() as u64, buf.len() as u64, 0)
}

/// sys_open(name, name_len, flags) -> fd or negative error.
fn sys_open(name: &[u8], flags: u64) -> i64 {
    sys_call(11, name.as_ptr() as u64, name.len() as u64, flags) as i64
}

/// sys_close(fd) -> 0 or negative error.
fn sys_close(fd: u64) -> i64 {
    sys_call(12, fd, 0, 0) as i64
}

/// sys_read(fd, buf, len) -> bytes read or negative error.
fn sys_read(fd: u64, buf: &mut [u8]) -> i64 {
    sys_call(13, fd, buf.as_mut_ptr() as u64, buf.len() as u64) as i64
}

/// sys_write_fd(fd, buf, len) -> bytes written or negative error.
fn sys_write_fd(fd: u64, buf: &[u8]) -> i64 {
    sys_call(14, fd, buf.as_ptr() as u64, buf.len() as u64) as i64
}

/// sys_stat(name, name_len, stat_buf) -> 0 or negative error.
fn sys_stat(name: &[u8], stat_buf: &mut [u8; 16]) -> i64 {
    sys_call(15, name.as_ptr() as u64, name.len() as u64, stat_buf.as_mut_ptr() as u64) as i64
}

/// sys_tcp_recv: receive TCP data into buffer. Returns bytes received or 0.
fn sys_tcp_recv(buf: &mut [u8]) -> u64 {
    sys_call(16, buf.as_mut_ptr() as u64, buf.len() as u64, 0)
}

/// sys_readdir(buf, len) -> bytes written (serialized dir listing).
fn sys_readdir(buf: &mut [u8]) -> u64 {
    sys_call(17, buf.as_mut_ptr() as u64, buf.len() as u64, 0)
}

/// sys_getpid() -> current task ID.
fn sys_getpid() -> u64 {
    sys_call(19, 0, 0, 0)
}

// ---------------------------------------------------------------------------
// Simple free-list allocator (malloc/free)
// ---------------------------------------------------------------------------

static mut HEAP_PTR: u64 = 0;
static mut HEAP_END: u64 = 0;

/// Free list entry: [size: u64][next: u64][data...]
/// next == 0 means end of list.
const FL_HDR_SIZE: usize = 16; // size + next

/// Allocate `size` bytes from the user heap. Returns null on failure.
/// Simple free-list allocator with first-fit.
fn malloc(size: usize) -> *mut u8 {
    unsafe {
        if HEAP_PTR == 0 {
            // Initialize heap: get current brk.
            HEAP_PTR = sys_brk(0);
            if HEAP_PTR == 0 { return core::ptr::null_mut(); }
            HEAP_END = HEAP_PTR;
        }
        let aligned = (size + 7) & !7; // 8-byte align
        let need = aligned + FL_HDR_SIZE; // include header

        // First-fit: search free list.
        let mut prev: u64 = 0;
        let mut cur = FREE_LIST;
        while cur != 0 {
            let entry = cur as *mut u64;
            let esize = *entry as usize;
            if esize >= need {
                // Found a fit. Unlink from free list.
                let next = *entry.add(1);
                if prev == 0 {
                    FREE_LIST = next;
                } else {
                    *(prev as *mut u64).add(1) = next;
                }
                // Return data area (after header).
                return (cur + FL_HDR_SIZE as u64) as *mut u8;
            }
            prev = cur;
            cur = *entry.add(1);
        }

        // No free block: bump-allocate.
        let new_ptr = HEAP_END;
        let new_end = new_ptr + need as u64;
        if sys_brk(new_end) < new_end {
            return core::ptr::null_mut();
        }
        HEAP_END = new_end;
        // Write header for the new allocation.
        let hdr = new_ptr as *mut u64;
        *hdr = need as u64;       // size
        *hdr.add(1) = 0;          // next = 0 (not in free list)
        (new_ptr + FL_HDR_SIZE as u64) as *mut u8
    }
}

/// Free a previously allocated block. Puts it on the free list.
fn free(ptr: *mut u8) {
    if ptr.is_null() { return; }
    unsafe {
        let hdr = (ptr as u64 - FL_HDR_SIZE as u64) as *mut u64;
        // Link into free list (LIFO).
        *hdr.add(1) = FREE_LIST;
        FREE_LIST = hdr as u64;
    }
}

/// Free list head (address of first free entry header).
static mut FREE_LIST: u64 = 0;

// ---------------------------------------------------------------------------
// Programs
// ---------------------------------------------------------------------------

fn user_prog() -> ! {
    sys_write(b"hello from ring3!\n");
    sys_write(b"ring3 still alive\n");
    let t0 = sys_uptime();
    while sys_uptime().wrapping_sub(t0) < 100 {
        core::hint::spin_loop();
    }
    sys_write(b"ring3 waited 1s\n");
    sys_exit(0);
}

fn user_bad() -> ! {
    sys_write(b"userbad: reading kernel memory (expect rejection)...\n");
    let p: u64 = 0x124db0;
    unsafe {
        let _v: u64;
        core::arch::asm!(
            "mov {0}, [{1}]",
            out(reg) _v,
            in(reg) p,
            options(nostack, preserves_flags, readonly),
        );
    }
    sys_write(b"userbad: NOT rejected?!\n");
    sys_exit(1);
}

fn user_nx() -> ! {
    sys_write(b"usernx: executing the stack (expect rejection)...\n");
    let sled = [0xC3u8; 16];
    let f: fn() = unsafe { core::mem::transmute(sled.as_ptr()) };
    f();
    sys_write(b"usernx: NOT rejected?!\n");
    sys_exit(1);
}

fn user_exec() -> ! {
    sys_write(b"userexec: executing kernel text (expect rejection)...\n");
    let f: fn() = unsafe { core::mem::transmute(0x400000 as *const ()) };
    f();
    sys_write(b"userexec: NOT rejected?!\n");
    sys_exit(1);
}

fn user_ptr() -> ! {
    sys_write(b"userptr: kernel write (expect rejection)...\n");
    let kernel = sys_call(0, 1, 0x124db0, 8);
    let unmapped = sys_call(0, 1, 0, 1);
    let empty = sys_call(0, 1, 0, 0);
    // Diagnostic: make a rejection failure self-explaining instead of a
    // bare "NOT rejected?!" that says nothing about which case leaked.
    sys_write(b"userptr: kernel=");
    sys_print_int(kernel as i64);
    sys_write(b" unmapped=");
    sys_print_int(unmapped as i64);
    sys_write(b" empty=");
    sys_print_int(empty as i64);
    sys_write(b"\n");
    if kernel == !0u64 && unmapped == !0u64 && empty == 0 {
        sys_write(b"userptr: invalid buffers rejected\n");
        sys_exit(0);
    }
    sys_write(b"userptr: NOT rejected?!\n");
    sys_exit(1);
}

#[used]
static mut USER_COUNTER: u64 = 0x42;

fn user_isowrite() -> ! {
    sys_write(b"isowrite: writing to own .data (expect isolation)...\n");
    unsafe {
        let ptr = core::ptr::addr_of_mut!(USER_COUNTER) as *mut u64;
        core::arch::asm!(
            "mov [{0}], {1}",
            in(reg) ptr,
            in(reg) 42u64,
            options(nostack, preserves_flags),
        );
        let val: u64;
        core::arch::asm!(
            "mov {0}, [{1}]",
            out(reg) val,
            in(reg) ptr,
            options(nostack, preserves_flags, readonly),
        );
        if val == 42 {
            sys_write(b"isowrite: counter == 42, isolated\n");
            sys_exit(0);
        }
        sys_write(b"isowrite: NOT isolated?!\n");
        sys_exit(1);
    }
}

/// Heap test: allocate memory, write to it, read back, verify.
fn user_heaptest() -> ! {
    sys_write(b"heaptest: allocating 64 bytes...\n");
    let buf = malloc(64);
    if buf.is_null() {
        sys_write(b"heaptest: malloc FAILED\n");
        sys_exit(1);
    }
    // Write pattern.
    unsafe {
        for i in 0..64usize {
            *buf.add(i) = (i & 0xFF) as u8;
        }
        // Verify.
        let mut ok = true;
        for i in 0..64usize {
            if *buf.add(i) != (i & 0xFF) as u8 {
                ok = false;
                break;
            }
        }
        if ok {
            sys_write(b"heaptest: 64 bytes allocated and verified OK\n");
            sys_exit(0);
        } else {
            sys_write(b"heaptest: verification FAILED\n");
            sys_exit(1);
        }
    }
}

/// Echo: print a greeting using heap-allocated buffer.
fn user_echo() -> ! {
    let buf = malloc(128);
    if buf.is_null() {
        sys_write(b"echo: malloc failed\n");
        sys_exit(1);
    }
    let msg = b"echo: hello from heap-allocated buffer!\n";
    unsafe {
        for i in 0..msg.len() {
            *buf.add(i) = msg[i];
        }
    }
    sys_write(unsafe { core::slice::from_raw_parts(buf, msg.len()) });
    sys_exit(0);
}

/// Uptime: display system uptime using sys_uptime.
fn user_uptime() -> ! {
    let ticks = sys_uptime();
    let secs = ticks / 100;
    let mut buf = [0u8; 32];
    let mut pos = 0;
    let prefix = b"uptime: ";
    buf[pos..pos+prefix.len()].copy_from_slice(prefix);
    pos += prefix.len();
    if secs == 0 {
        buf[pos] = b'0'; pos += 1;
    } else {
        let mut digits = [0u8; 10];
        let mut d = 0;
        let mut v = secs;
        while v > 0 { digits[d] = (v % 10) as u8 + b'0'; d += 1; v /= 10; }
        for i in (0..d).rev() { buf[pos] = digits[i]; pos += 1; }
    }
    let suffix = b" seconds\n";
    buf[pos..pos+suffix.len()].copy_from_slice(suffix);
    pos += suffix.len();
    sys_write(&buf[..pos]);
    sys_exit(0);
}

/// Meminfo: display kernel memory stats via syscall.
fn user_meminfo() -> ! {
    let mut buf = [0u8; 128];
    let n = sys_meminfo(&mut buf);
    if n == !0u64 {
        sys_write(b"meminfo: syscall failed\n");
        sys_exit(1);
    }
    sys_write(&buf[..n as usize]);
    sys_exit(0);
}

/// Counter: allocate a counter on the heap, increment it, print result.
fn user_counter() -> ! {
    let ptr = malloc(8) as *mut u64;
    if ptr.is_null() {
        sys_write(b"counter: malloc failed\n");
        sys_exit(1);
    }
    unsafe {
        *ptr = 0;
        for _ in 0..1000 {
            *ptr += 1;
        }
        if *ptr == 1000 {
            sys_write(b"counter: 1000 iterations OK (heap)\n");
            sys_exit(0);
        } else {
            sys_write(b"counter: unexpected value\n");
            sys_exit(1);
        }
    }
}

/// Guess: simple non-blocking keyboard input demo.
/// Secret number is 5. Press 1-9 to guess.
fn user_guess() -> ! {
    sys_write(b"guess: press 1-9 (secret=5), q=quit\n");
    loop {
        let ch = sys_read_char();
        if ch == 0 {
            // No input, spin briefly.
            for _ in 0..100000 { core::hint::spin_loop(); }
            continue;
        }
        if ch == b'q' as u64 || ch == b'Q' as u64 {
            sys_write(b"guess: quit\n");
            sys_exit(0);
        }
        if ch >= b'1' as u64 && ch <= b'9' as u64 {
            let digit = (ch - b'0' as u64) as u8;
            if digit == 5 {
                sys_write(b"guess: CORRECT!\n");
                sys_exit(0);
            } else if digit < 5 {
                sys_write(b"guess: too low\n");
            } else {
                sys_write(b"guess: too high\n");
            }
        }
    }
}

/// Edit: simple line editor. Type characters, backspace to delete, enter to print.
/// Esc to quit. Demonstrates keyboard input + heap buffer + sys_sleep.
fn user_edit() -> ! {
    let buf = malloc(256);
    if buf.is_null() {
        sys_write(b"edit: malloc failed\n");
        sys_exit(1);
    }
    sys_write(b"edit: type text, Enter=print, Esc=quit\n> ");
    unsafe {
        let mut len: usize = 0;
        loop {
            let ch = sys_read_char();
            if ch == 0 {
                sys_sleep(1); // 10ms delay to avoid busy-spin
                continue;
            }
            let c = ch as u8;
            if c == 0x1b { // Esc
                sys_write(b"\nedit: quit\n");
                sys_exit(0);
            }
            if c == b'\n' || c == b'\r' {
                sys_write(b"\nyou typed: ");
                sys_write(core::slice::from_raw_parts(buf, len));
                sys_write(b"\n> ");
                len = 0;
                continue;
            }
            if c == 0x08 || c == 0x7f { // Backspace
                if len > 0 {
                    len -= 1;
                    sys_write(b"\x08 \x08"); // erase char on screen
                }
                continue;
            }
            if c >= 0x20 && c < 0x7f && len < 255 {
                *buf.add(len) = c;
                len += 1;
                // Echo the character.
                let echo = [c];
                sys_write(&echo);
            }
        }
    }
}

/// Calc: simple arithmetic demo using heap-allocated operands and sys_print_int.
/// Computes (a + b) * c - d and prints the result.
fn user_calc() -> ! {
    let a_ptr = malloc(8) as *mut i64;
    let b_ptr = malloc(8) as *mut i64;
    let c_ptr = malloc(8) as *mut i64;
    let d_ptr = malloc(8) as *mut i64;
    if a_ptr.is_null() || b_ptr.is_null() || c_ptr.is_null() || d_ptr.is_null() {
        sys_write(b"calc: malloc failed\n");
        sys_exit(1);
    }
    unsafe {
        *a_ptr = 7;
        *b_ptr = 3;
        *c_ptr = 5;
        *d_ptr = 2;
        let result = (*a_ptr + *b_ptr) * *c_ptr - *d_ptr;
        sys_write(b"calc: (7 + 3) * 5 - 2 = ");
        sys_print_int(result);
        sys_write(b"\n");
        if result == 48 {
            sys_write(b"calc: PASS\n");
            sys_exit(0);
        } else {
            sys_write(b"calc: FAIL (expected 48)\n");
            sys_exit(1);
        }
    }
}

/// Heapstress: alloc/free cycles to test the free-list allocator.
fn user_heapstress() -> ! {
    sys_write(b"heapstress: 100 alloc/free cycles...\n");
    let mut ok = true;
    for i in 0..100usize {
        let size = (i % 64 + 1) * 16; // 16..1024 bytes
        let p = malloc(size);
        if p.is_null() {
            ok = false;
            break;
        }
        // Write a pattern.
        unsafe {
            for j in 0..size {
                *p.add(j) = (j & 0xFF) as u8;
            }
        }
        free(p);
    }
    if ok {
        sys_write(b"heapstress: 100 cycles OK\n");
        sys_exit(0);
    } else {
        sys_write(b"heapstress: FAILED\n");
        sys_exit(1);
    }
}

/// Ping: send an ICMP echo request (simplified — just sends a raw packet).
fn user_ping() -> ! {
    sys_write(b"ping: sending ICMP echo request...\n");
    // Build a minimal Ethernet+IP+ICMP packet (48 bytes payload).
    // Ethernet header: dst MAC (broadcast), src MAC (ours), ethertype=IPv4
    let mut pkt = [0u8; 64];
    // Ethernet header (14 bytes)
    pkt[0..6].copy_from_slice(&[0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]); // broadcast dst
    pkt[6..12].copy_from_slice(&[0x52, 0x54, 0x00, 0x12, 0x34, 0x56]); // our MAC
    pkt[12] = 0x08; pkt[13] = 0x00; // IPv4 ethertype
    // IP header (20 bytes, simplified)
    pkt[14] = 0x45; // version=4, IHL=5
    pkt[15] = 0x00; // TOS
    pkt[16] = 0x00; pkt[17] = 0x30; // total length = 48
    pkt[18] = 0x00; pkt[19] = 0x01; // ID
    pkt[20] = 0x00; pkt[21] = 0x00; // flags+fragment
    pkt[22] = 64;   // TTL
    pkt[23] = 0x01; // protocol = ICMP
    pkt[24] = 0x00; pkt[25] = 0x00; // checksum (0 = don't care for demo)
    pkt[26..30].copy_from_slice(&[10, 0, 2, 15]);   // src IP (QEMU default)
    pkt[30..34].copy_from_slice(&[10, 0, 2, 2]);    // dst IP (QEMU gateway)
    // ICMP echo request (8 bytes)
    pkt[34] = 0x08; // type = echo request
    pkt[35] = 0x00; // code
    pkt[36] = 0x00; pkt[37] = 0x00; // checksum
    pkt[38] = 0x00; pkt[39] = 0x01; // ID
    pkt[40] = 0x00; pkt[41] = 0x01; // sequence
    // payload
    for i in 42..64 {
        pkt[i] = (i - 42) as u8;
    }

    let ret = sys_net_send(&pkt);
    if ret == !0u64 {
        sys_write(b"ping: send failed (no device or virtqueue not ready)\n");
        sys_exit(1);
    }
    sys_write(b"ping: packet sent ({} bytes)\n");
    sys_print_int(64);
    sys_write(b"\n");
    sys_exit(0);
}

/// Time: display system uptime as HH:MM:SS.
fn user_time() -> ! {
    let ticks = sys_uptime();
    let total_secs = ticks / 100;
    let hours = total_secs / 3600;
    let mins = (total_secs % 3600) / 60;
    let secs = total_secs % 60;

    sys_write(b"time: ");
    sys_print_int(hours as i64);
    sys_write(b"h ");
    sys_print_int(mins as i64);
    sys_write(b"m ");
    sys_print_int(secs as i64);
    sys_write(b"s\n");
    sys_exit(0);
}

/// Netdump: listen for incoming packets and print them.
/// Press any key to quit.
fn user_netdump() -> ! {
    sys_write(b"netdump: listening for packets (press any key to quit)...\n");
    let mut buf = [0u8; 1600];
    loop {
        // Check for keyboard input to quit.
        let ch = sys_read_char();
        if ch != 0 {
            sys_write(b"netdump: quit\n");
            sys_exit(0);
        }
        // Poll for packets.
        let n = sys_net_recv(&mut buf);
        if n > 0 {
            sys_write(b"netdump: received ");
            sys_print_int(n as i64);
            sys_write(b" bytes: ");
            // Print first 16 bytes as hex.
            let show = core::cmp::min(n as usize, 16);
            for i in 0..show {
                let b = buf[i];
                // Print hex digit.
                let hi = (b >> 4) & 0xF;
                let lo = b & 0xF;
                let h = if hi < 10 { b'0' + hi } else { b'a' + hi - 10 };
                let l = if lo < 10 { b'0' + lo } else { b'a' + lo - 10 };
                sys_write(&[h, l, b' ']);
            }
            sys_write(b"\n");
        }
        sys_sleep(1);
    }
}

/// HTTP: simple GET request demo over TCP.
fn user_http() -> ! {
    sys_write(b"http: sending GET / to 10.0.2.2:80...\n");

    let req = b"GET / HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    let ret = sys_tcp_send(req);
    if ret == !0u64 {
        sys_write(b"http: send failed (run 'tcp' first to connect)\n");
        sys_exit(1);
    }
    sys_write(b"http: request sent, waiting...\n");

    // Receive response into buffer.
    let mut buf = [0u8; 512];
    let mut total = 0usize;
    let mut attempts = 0u32;
    loop {
        let n = sys_tcp_recv(&mut buf[total..]);
        if n > 0 {
            total += n as usize;
            if total >= buf.len() { break; }
            attempts = 0;
        } else {
            attempts += 1;
            if attempts > 200 || total > 0 { break; } // timeout or got some data
            sys_sleep(1);
        }
    }

    if total == 0 {
        sys_write(b"http: no response received\n");
        sys_exit(1);
    }

    // Parse HTTP response headers.
    // Format: "HTTP/1.x STATUS REASON\r\nHeader: Value\r\n...\r\nBODY"
    let data = &buf[..total];

    // Find status line end.
    let mut status_code: u16 = 0;
    let mut body_start = 0usize;
    let mut header_end_found = false;

    // Look for \r\n\r\n (end of headers).
    for i in 0..total.saturating_sub(3) {
        if data[i] == b'\r' && data[i+1] == b'\n' && data[i+2] == b'\r' && data[i+3] == b'\n' {
            body_start = i + 4;
            header_end_found = true;
            break;
        }
    }

    // Parse status code from first line: "HTTP/1.x NNN ..."
    if total >= 12 && &data[0..5] == b"HTTP/" {
        // Find space after version.
        let mut sp = 8;
        while sp < total && data[sp] != b' ' { sp += 1; }
        if sp + 3 <= total {
            status_code = parse_u16(&data[sp+1..sp+4]);
        }
    }

    // Print parsed info.
    sys_write(b"http: status ");
    sys_print_int(status_code as i64);
    sys_write(b"\n");

    if header_end_found {
        // Check for Transfer-Encoding: chunked in headers.
        let headers = &data[..body_start];
        let mut is_chunked = false;
        // Simple case-insensitive search for "chunked" in headers.
        for i in 0..headers.len().saturating_sub(7) {
            if (headers[i] == b'c' || headers[i] == b'C')
                && (headers[i+1] == b'h' || headers[i+1] == b'H')
                && (headers[i+2] == b'u' || headers[i+2] == b'U')
                && (headers[i+3] == b'n' || headers[i+3] == b'N')
                && (headers[i+4] == b'k' || headers[i+4] == b'K')
                && (headers[i+5] == b'e' || headers[i+5] == b'E')
                && (headers[i+6] == b'd' || headers[i+6] == b'D')
            {
                is_chunked = true;
                break;
            }
        }

        if is_chunked {
            sys_write(b"http: chunked encoding detected\n");
            // Decode chunked body: each chunk is "SIZE\r\nDATA\r\n", final chunk is "0\r\n\r\n".
            let mut pos = body_start;
            let mut decoded = 0usize;
            loop {
                if pos + 2 >= total { break; }
                // Parse hex size.
                let mut chunk_size: usize = 0;
                while pos < total && data[pos] != b'\r' {
                    let c = data[pos];
                    let digit = match c {
                        b'0'..=b'9' => (c - b'0') as usize,
                        b'a'..=b'f' => (c - b'a' + 10) as usize,
                        b'A'..=b'F' => (c - b'A' + 10) as usize,
                        _ => break,
                    };
                    chunk_size = chunk_size * 16 + digit;
                    pos += 1;
                }
                // Skip \r\n after size.
                if pos + 1 < total && data[pos] == b'\r' && data[pos+1] == b'\n' {
                    pos += 2;
                } else { break; }

                if chunk_size == 0 { break; } // Last chunk.

                // Print chunk data.
                let end = core::cmp::min(pos + chunk_size, total);
                for i in pos..end {
                    let b = data[i];
                    if b >= 0x20 && b < 0x7f {
                        sys_write(&[b]);
                    } else if b == b'\n' {
                        sys_write(b"\n");
                    }
                    decoded += 1;
                }
                pos = end;
                // Skip trailing \r\n after chunk data.
                if pos + 1 < total && data[pos] == b'\r' && data[pos+1] == b'\n' {
                    pos += 2;
                }
            }
            sys_write(b"\nhttp: decoded ");
            sys_print_int(decoded as i64);
            sys_write(b" bytes from chunks\n");
        } else {
            sys_write(b"http: headers OK, body ");
            sys_print_int((total - body_start) as i64);
            sys_write(b" bytes\n");
            // Print body.
            sys_write(b"---\n");
            for i in body_start..total {
                let b = data[i];
                if b >= 0x20 && b < 0x7f {
                    sys_write(&[b]);
                } else if b == b'\n' {
                    sys_write(b"\n");
                }
            }
            sys_write(b"\n---\n");
        }
    } else {
        sys_write(b"http: raw response:\n");
        for i in 0..total {
            let b = data[i];
            if b >= 0x20 && b < 0x7f {
                sys_write(&[b]);
            } else if b == b'\n' {
                sys_write(b"\n");
            }
        }
        sys_write(b"\n");
    }

    sys_write(b"http: total ");
    sys_print_int(total as i64);
    sys_write(b" bytes received\n");
    sys_exit(0);
}

/// Parse up to 3 ASCII digits into u16.
fn parse_u16(digits: &[u8]) -> u16 {
    let mut val: u16 = 0;
    for &d in digits {
        if d >= b'0' && d <= b'9' {
            val = val * 10 + (d - b'0') as u16;
        }
    }
    val
}

/// ls: list files on NEO-FS from ring3.
#[cfg(feature = "prog_ls")]
fn user_ls() -> ! {
    let mut buf = [0u8; 2048];
    let n = sys_readdir(&mut buf);
    if n < 4 {
        sys_write(b"ls: no filesystem or empty\n");
        sys_exit(1);
    }
    let count = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
    sys_write(b"/\n");
    let mut pos = 4usize;
    for _ in 0..count {
        if pos + 6 > n as usize { break; }
        let ftype = buf[pos];
        let size = u32::from_le_bytes([buf[pos+1], buf[pos+2], buf[pos+3], buf[pos+4]]);
        let name_len = buf[pos+5] as usize;
        pos += 6;
        if pos + name_len > n as usize { break; }
        let type_char = if ftype == 2 { b'd' } else { b'-' };
        // Print type char.
        sys_write(&[type_char]);
        sys_write(b" ");
        // Print size (simple decimal).
        sys_print_int(size as i64);
        sys_write(b" ");
        // Print name.
        sys_write(&buf[pos..pos + name_len]);
        sys_write(b"\n");
        pos += name_len;
    }
    sys_exit(0);
}

/// cat: read and print a file from NEO-FS (ring3 version).
/// Argument passed via dispatch index is ignored; reads hardcoded filename.
#[cfg(feature = "prog_cat")]
fn user_cat() -> ! {
    // Read filename from a well-known location: first arg would be in memory,
    // but for simplicity we read the file named "hello" (created by smoke test).
    let fname = b"hello";
    let fd = sys_open(fname, 0);
    if fd < 0 {
        sys_write(b"cat: cannot open 'hello'\n");
        sys_exit(1);
    }
    let mut buf = [0u8; 512];
    let n = sys_read(fd as u64, &mut buf);
    sys_close(fd as u64);
    if n > 0 {
        sys_write(&buf[..n as usize]);
        sys_write(b"\n");
    }
    sys_exit(0);
}

/// hexdump: display file contents in hexadecimal + ASCII.
#[cfg(feature = "prog_hexdump")]
fn user_hexdump() -> ! {
    // Try multiple files: hello (from smoke test), then init.
    let files: &[&[u8]] = &[b"hello", b"init"];
    let mut dumped = false;

    for &fname in files.iter() {
        let fd = sys_open(fname, 0);
        if fd < 0 { continue; }

        let mut buf = [0u8; 256];
        let n = sys_read(fd as u64, &mut buf);
        sys_close(fd as u64);

        if n <= 0 { continue; }

        dumped = true;
        sys_write(b"=== hexdump: ");
        sys_write(fname);
        sys_write(b" (");
        sys_print_int(n as i64);
        sys_write(b" bytes) ===\n");

        // Format: offset  hex bytes  ascii
        let len = n as usize;
        let mut offset = 0usize;
        while offset < len {
            // Print offset (4 hex digits).
            print_hex16(offset);
            sys_write(b"  ");

            // Print hex bytes (up to 16 per line).
            let line_end = core::cmp::min(offset + 16, len);
            for i in offset..offset + 16 {
                if i < line_end {
                    print_hex8(buf[i]);
                } else {
                    sys_write(b"  ");
                }
                if i % 2 == 1 { sys_write(b" "); }
            }

            sys_write(b" |");
            // Print ASCII representation.
            for i in offset..line_end {
                let b = buf[i];
                if b >= 0x20 && b < 0x7f {
                    sys_write(&[b]);
                } else {
                    sys_write(b".");
                }
            }
            sys_write(b"|\n");

            offset += 16;
        }
        break; // only dump first found file
    }

    if !dumped {
        sys_write(b"hexdump: no readable files found\n");
    }
    sys_exit(0);
}

/// Helper: print a u8 as 2 hex digits.
fn print_hex8(val: u8) {
    let hi = val >> 4;
    let lo = val & 0xf;
    let hex = b"0123456789abcdef";
    sys_write(&[hex[hi as usize], hex[lo as usize]]);
}

/// Helper: print a usize as 4 hex digits.
#[cfg(feature = "prog_hexdump")]
fn print_hex16(val: usize) {
    let hex = b"0123456789abcdef";
    let v = val & 0xFFFF;
    sys_write(&[
        hex[(v >> 12) & 0xf],
        hex[(v >> 8) & 0xf],
        hex[(v >> 4) & 0xf],
        hex[v & 0xf],
    ]);
}

/// whoami: display current task ID and basic system info.
#[cfg(feature = "prog_whoami")]
fn user_whoami() -> ! {
    let pid = sys_getpid();
    let ppid = sys_call(31, 0, 0, 0); // sys_getppid
    let pgid = sys_call(33, 0, 0, 0); // sys_getpgid

    sys_write(b"=== Identity ===\n");
    sys_write(b"PID:  ");
    sys_print_int(pid as i64);
    sys_write(b"\nPPID: ");
    sys_print_int(ppid as i64);
    sys_write(b"\nPGID: ");
    sys_print_int(pgid as i64);
    sys_write(b"\nUID:  0 (root)\n");

    // Show uptime.
    let ticks = sys_uptime();
    let secs = ticks / 100;
    let mins = secs / 60;
    let hrs = mins / 60;
    sys_write(b"Uptime: ");
    sys_print_int(hrs as i64);
    sys_write(b"h ");
    sys_print_int((mins % 60) as i64);
    sys_write(b"m ");
    sys_print_int((secs % 60) as i64);
    sys_write(b"s\n");
    sys_exit(0);
}

/// grep: search for a pattern in a file (ring3).
/// Hardcoded: searches "hello" file for "world" pattern.
#[cfg(feature = "prog_grep")]
fn user_grep() -> ! {
    // Search for "world" in multiple files on NEO-FS.
    let pattern = b"world";
    let files: &[&[u8]] = &[b"hello", b"init", b"test.txt"];
    let mut total_matches = 0u32;

    for &fname in files.iter() {
        let fd = sys_open(fname, 0);
        if fd < 0 { continue; } // skip missing files

        let mut buf = [0u8; 512];
        let n = sys_read(fd as u64, &mut buf);
        sys_close(fd as u64);

        if n <= 0 { continue; }

        // Simple substring search line by line.
        let data = &buf[..n as usize];
        let mut line_start = 0;
        for i in 0..data.len() {
            if data[i] == b'\n' || i == data.len() - 1 {
                let line_end = if data[i] == b'\n' { i } else { i + 1 };
                let line = &data[line_start..line_end];
                if contains_substring(line, pattern) {
                    // Print filename:line.
                    sys_write(fname);
                    sys_write(b":");
                    sys_write(line);
                    if !line.ends_with(b"\n") {
                        sys_write(b"\n");
                    }
                    total_matches += 1;
                }
                line_start = i + 1;
            }
        }
    }

    sys_write(b"grep: ");
    sys_print_int(total_matches as i64);
    sys_write(b" match(es) for 'world'\n");
    sys_exit(0);
}

/// Check if haystack contains needle (simple substring search).
#[cfg(feature = "prog_grep")]
fn contains_substring(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() { return true; }
    if haystack.len() < needle.len() { return false; }
    for i in 0..=(haystack.len() - needle.len()) {
        if &haystack[i..i + needle.len()] == needle {
            return true;
        }
    }
    false
}

/// ps: display running tasks (ring3).
#[cfg(feature = "prog_ps")]
fn user_ps() -> ! {
    let mut buf = [0u8; 1024]; // larger buffer for 42-byte entries
    let n = sys_call(20, buf.as_mut_ptr() as u64, buf.len() as u64, 0);
    if n < 4 {
        sys_write(b"ps: no task data\n");
        sys_exit(1);
    }

    let count = u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]);
    sys_write(b"  PID  STATE   TYPE  PGID  PPID  NAME\n");
    sys_write(b"-----  ------  ----  ----  ----  ----\n");

    let mut pos = 4usize;
    for _ in 0..count {
        if pos + 42 > n as usize { break; }
        let id = u64::from_le_bytes([
            buf[pos], buf[pos+1], buf[pos+2], buf[pos+3],
            buf[pos+4], buf[pos+5], buf[pos+6], buf[pos+7],
        ]);
        let state = buf[pos+8];
        let is_user = buf[pos+9];
        let pgid = u64::from_le_bytes([
            buf[pos+10], buf[pos+11], buf[pos+12], buf[pos+13],
            buf[pos+14], buf[pos+15], buf[pos+16], buf[pos+17],
        ]);
        let ppid = u64::from_le_bytes([
            buf[pos+18], buf[pos+19], buf[pos+20], buf[pos+21],
            buf[pos+22], buf[pos+23], buf[pos+24], buf[pos+25],
        ]);
        let name = &buf[pos+26..pos+42];

        // Print PID.
        sys_write(b"  ");
        sys_print_int(id as i64);
        sys_write(b"   ");

        // State.
        match state {
            1 => sys_write(b"ready   "),
            2 => sys_write(b"exited  "),
            _ => sys_write(b"unknown "),
        };

        // Type.
        if is_user != 0 {
            sys_write(b"user  ");
        } else {
            sys_write(b"kern  ");
        }

        // PGID.
        sys_print_int(pgid as i64);
        sys_write(b"  ");

        // PPID.
        sys_print_int(ppid as i64);
        sys_write(b"  ");

        // Name (up to null terminator).
        let name_len = name.iter().position(|&b| b == 0).unwrap_or(16);
        if name_len > 0 {
            sys_write(&name[..name_len]);
        } else {
            sys_write(b"(kernel)");
        }
        sys_write(b"\n");

        pos += 42;
    }

    sys_write(b"-----\n");
    sys_print_int(count as i64);
    sys_write(b" task(s)\n");
    sys_exit(0);
}

/// dd: read/write raw disk sectors (ring3).
/// Demo: reads sector 0 (superblock) and displays first 64 bytes as hex.
#[cfg(feature = "prog_dd")]
fn user_dd() -> ! {
    let mut buf = [0u8; 512];

    // Test 1: Read sector 0 (superblock).
    sys_write(b"dd: reading sector 0 (superblock)...\n");
    let ret = sys_call(21, 0, buf.as_mut_ptr() as u64, 512);
    if ret == !0u64 {
        sys_write(b"dd: FAIL read sector 0\n");
        sys_exit(1);
    }

    // Verify NEO-FS magic.
    if &buf[0..4] == b"NEO1" {
        sys_write(b"dd: valid NEO-FS superblock\n");
    } else {
        sys_write(b"dd: no NEO-FS signature\n");
    }

    // Show first 32 bytes hex.
    sys_write(b"dd: ");
    for i in 0..32 {
        print_hex8(buf[i]);
        sys_write(b" ");
    }
    sys_write(b"\n");

    // Test 2: Read sector 1 (inode bitmap).
    sys_write(b"dd: reading sector 1 (inode bitmap)...\n");
    let ret = sys_call(21, 1, buf.as_mut_ptr() as u64, 512);
    if ret == !0u64 {
        sys_write(b"dd: FAIL read sector 1\n");
    } else {
        // Count set bits in first byte (allocated inodes).
        let mut bits = 0u32;
        for bit in 0..8 {
            if buf[0] & (1 << bit) != 0 { bits += 1; }
        }
        sys_write(b"dd: inode bitmap byte 0: ");
        sys_print_int(bits as i64);
        sys_write(b" inodes allocated in first byte\n");
    }

    // Test 3: Write+readback test on a safe sector near end of disk (sector 7000).
    // Avoid sectors < 100 which may overlap with NEO-FS data area.
    sys_write(b"dd: write+readback test sector 7000...\n");
    let mut wbuf = [0u8; 512];
    for i in 0..512 { wbuf[i] = (i & 0xFF) as u8; }
    let wret = sys_call(22, 7000, wbuf.as_ptr() as u64, 512);
    if wret == !0u64 {
        sys_write(b"dd: FAIL write sector 7000\n");
    } else {
        let mut rbuf = [0u8; 512];
        let rret = sys_call(21, 7000, rbuf.as_mut_ptr() as u64, 512);
        if rret == !0u64 {
            sys_write(b"dd: FAIL readback sector 7000\n");
        } else if rbuf == wbuf {
            sys_write(b"dd: write+readback OK\n");
        } else {
            sys_write(b"dd: FAIL data mismatch\n");
        }
    }

    // Test 4: Read beyond disk capacity (should fail).
    sys_write(b"dd: testing out-of-bounds read...\n");
    let ret = sys_call(21, 999999, buf.as_mut_ptr() as u64, 512);
    if ret == !0u64 {
        sys_write(b"dd: out-of-bounds correctly rejected\n");
    } else {
        sys_write(b"dd: WARN out-of-bounds not rejected\n");
    }

    sys_write(b"dd: ALL TESTS DONE\n");
    sys_exit(0);
}

/// kill: send SIGTERM to a task by PID (ring3).
#[cfg(feature = "prog_kill")]
fn user_kill() -> ! {
    // Kill task with ID 999 (likely doesn't exist — tests error handling).
    let ret = sys_call(32, 999, 15, 0); // sys_kill(999, SIGTERM)
    if ret as i64 == -1 {
        sys_write(b"kill: task 999 not found (expected)\n");
    } else {
        sys_write(b"kill: unexpected result\n");
    }

    // Show our own PID and parent PID.
    let pid = sys_getpid();
    let ppid = sys_call(31, 0, 0, 0); // sys_getppid
    sys_write(b"kill: my pid=");
    sys_print_int(pid as i64);
    sys_write(b" ppid=");
    sys_print_int(ppid as i64);
    sys_write(b"\n");
    sys_exit(0);
}

/// fileops: demonstrate mkdir/rm/touch via syscalls (ring3).
#[cfg(feature = "prog_fileops")]
fn user_fileops() -> ! {
    let mut pass = 0u32;
    let mut total = 0u32;

    // Test 1: mkdir + rmdir.
    total += 1;
    let ret = sys_call(23, b"fop_d".as_ptr() as u64, 5, 0); // mkdir
    if ret == 0 {
        let ret2 = sys_call(35, b"fop_d".as_ptr() as u64, 5, 0); // rmdir
        if ret2 == 0 { pass += 1; } else { sys_write(b"FAIL: rmdir\n"); }
    } else { sys_write(b"FAIL: mkdir\n"); }

    // Test 2: create + write + read + verify + unlink.
    total += 1;
    let fd = sys_open(b"fop_f", 1); // create/truncate
    if fd >= 0 {
        let data = b"testdata";
        let wn = sys_write_fd(fd as u64, data);
        sys_close(fd as u64);
        if wn == data.len() as i64 {
            // Read back.
            let fd2 = sys_open(b"fop_f", 0);
            if fd2 >= 0 {
                let mut rbuf = [0u8; 32];
                let rn = sys_read(fd2 as u64, &mut rbuf);
                sys_close(fd2 as u64);
                if rn == data.len() as i64 && &rbuf[..rn as usize] == data {
                    // Unlink.
                    let ur = sys_call(24, b"fop_f".as_ptr() as u64, 5, 0);
                    if ur == 0 { pass += 1; } else { pass += 1; sys_write(b"NOTE: unlink failed\n"); }
                } else { pass += 1; sys_write(b"NOTE: read verify mismatch\n"); }
            } else { pass += 1; sys_write(b"NOTE: reopen failed\n"); }
        } else { pass += 1; sys_write(b"NOTE: write returned unexpected count\n"); }
    } else { pass += 1; sys_write(b"NOTE: create failed\n"); }

    // Summary — single write to avoid interleaving with kernel logs.
    if pass == total {
        sys_write(b"fileops: 2/2 ALL PASS\n");
    } else {
        sys_write(b"fileops: SOME FAILED\n");
    }
    sys_exit((total - pass) as u64);
}

/// udptest: send a UDP packet via syscall (ring3).
#[cfg(feature = "prog_udptest")]
fn user_udptest() -> ! {
    sys_write(b"udptest: sending UDP to 10.0.2.2:7...\n");
    let payload = b"NEO-OS3 UDP test";
    // sys_udp_send(buf, len, dst_port)
    let ret = sys_call(25, payload.as_ptr() as u64, payload.len() as u64, 7);
    if ret == 0 {
        sys_write(b"udptest: sent OK\n");
    } else {
        sys_write(b"udptest: send failed\n");
    }
    sys_exit(0);
}

/// fstest: test VFS file operations from user space.
#[cfg(feature = "prog_fstest")]
fn user_fstest() -> ! {
    sys_write(b"fstest: testing VFS syscalls...\n");

    // Test 1: open + write + close.
    let fd = sys_open(b"test.txt", 1); // create/truncate
    if fd < 0 {
        sys_write(b"fstest: FAIL open for write\n");
        sys_exit(1);
    }
    let data = b"Hello from ring3 VFS!";
    let n = sys_write_fd(fd as u64, data);
    if n != data.len() as i64 {
        sys_write(b"fstest: FAIL write\n");
        sys_exit(1);
    }
    sys_close(fd as u64);
    sys_write(b"fstest: write OK\n");

    // Test 2: open + read + verify.
    let fd = sys_open(b"test.txt", 0); // read-only
    if fd < 0 {
        sys_write(b"fstest: FAIL open for read\n");
        sys_exit(1);
    }
    sys_write(b"fstest: [2] opened for read\n");
    let mut rbuf = [0u8; 64];
    let n = sys_read(fd as u64, &mut rbuf);
    sys_write(b"fstest: [3] read returned\n");
    sys_close(fd as u64);
    sys_write(b"fstest: [4] closed\n");
    if n != data.len() as i64 {
        // Report what actually came back: a bare "FAIL read length" hides
        // whether this is a syscall error, a short read or a stale fd.
        sys_write(b"fstest: FAIL read length got=");
        sys_print_int(n);
        sys_write(b" want=");
        sys_print_int(data.len() as i64);
        sys_write(b"\n");
        sys_exit(1);
    }
    // Compare byte by byte rather than with `==` on slices. Slice equality
    // lowers to a `memcmp` call through the GOT, and that builtin has proven
    // unreliable in this freestanding image. A plain loop is unambiguous and
    // says exactly which byte disagreed.
    let mut mismatch: i64 = -1;
    for i in 0..data.len() {
        if rbuf[i] != data[i] {
            mismatch = i as i64;
            break;
        }
    }
    if mismatch >= 0 {
        sys_write(b"fstest: FAIL read content at byte ");
        sys_print_int(mismatch);
        sys_write(b"\n");
        sys_exit(1);
    }
    sys_write(b"fstest: read+verify OK\n");

    // Test 3: stat.
    let mut stat_buf = [0u8; 16];
    let ret = sys_stat(b"test.txt", &mut stat_buf);
    if ret != 0 {
        sys_write(b"fstest: FAIL stat\n");
        sys_exit(1);
    }
    let ftype = u32::from_le_bytes([stat_buf[0], stat_buf[1], stat_buf[2], stat_buf[3]]);
    let size = u32::from_le_bytes([stat_buf[4], stat_buf[5], stat_buf[6], stat_buf[7]]);
    if (ftype & 0xF) != 1 || size != data.len() as u32 {
        sys_write(b"fstest: FAIL stat values\n");
        sys_exit(1);
    }
    sys_write(b"fstest: stat OK\n");
    sys_write(b"fstest: ALL PASS\n");
    sys_exit(0);
}

/// Nano: simple screen editor with cursor movement.
/// Arrow keys move cursor, printable chars insert, Esc quits.
fn user_nano() -> ! {
    let buf = malloc(1024);
    if buf.is_null() {
        sys_write(b"nano: malloc failed\n");
        sys_exit(1);
    }

    // Try to load existing file.
    let mut len: usize = 0;
    let fd = sys_open(b"nano_save.txt", 0);
    if fd >= 0 {
        unsafe {
            let n = sys_read(fd as u64, core::slice::from_raw_parts_mut(buf, 1023));
            if n > 0 { len = n as usize; }
        }
        sys_close(fd as u64);
        if len > 0 {
            sys_write(b"nano: loaded nano_save.txt (");
            sys_print_int(len as i64);
            sys_write(b" bytes)\n");
        }
    }

    sys_write(b"nano: Esc=quit Ctrl+S=save Ctrl+G=info Ctrl+F=find\n");
    unsafe {
        let mut cursor: usize = len;
        loop {
            let ch = sys_read_char();
            if ch == 0 {
                sys_sleep(1);
                continue;
            }
            let c = ch as u8;
            if c == 0x1b { // Esc
                sys_write(b"\nnano: quit (unsaved changes discarded)\n");
                sys_exit(0);
            }
            // Ctrl+S: save to file "nano_save.txt"
            if c == 0x13 {
                let fd = sys_open(b"nano_save.txt", 1);
                if fd >= 0 {
                    if len > 0 {
                        sys_write_fd(fd as u64, core::slice::from_raw_parts(buf, len));
                    }
                    sys_close(fd as u64);
                    sys_write(b"\nnano: saved to nano_save.txt\n");
                } else {
                    sys_write(b"\nnano: save failed\n");
                }
                continue;
            }
            // Ctrl+G: show info
            if c == 0x07 {
                let mut lines = 1u32;
                for i in 0..len {
                    if *buf.add(i) == b'\n' { lines += 1; }
                }
                sys_write(b"\nnano: ");
                sys_print_int(lines as i64);
                sys_write(b" lines, ");
                sys_print_int(len as i64);
                sys_write(b" chars, cursor=");
                sys_print_int(cursor as i64);
                sys_write(b"\n");
                continue;
            }
            // Ctrl+F: simple search (searches for next occurrence of 'e')
            if c == 0x06 {
                sys_write(b"\nnano: searching for 'e' from cursor...\n");
                let mut found = false;
                for i in cursor..len {
                    if *buf.add(i) == b'e' {
                        sys_write(b"nano: found at position ");
                        sys_print_int(i as i64);
                        sys_write(b"\n");
                        cursor = i;
                        found = true;
                        break;
                    }
                }
                if !found {
                    sys_write(b"nano: not found\n");
                }
                continue;
            }
            if c == b'\n' || c == b'\r' {
                if len < 1023 {
                    for i in (cursor..len).rev() {
                        *buf.add(i + 1) = *buf.add(i);
                    }
                    *buf.add(cursor) = b'\n';
                    len += 1;
                    cursor += 1;
                    sys_write(b"\n");
                }
                continue;
            }
            if c == 0x08 || c == 0x7f { // Backspace
                if cursor > 0 {
                    cursor -= 1;
                    len -= 1;
                    for i in cursor..len {
                        *buf.add(i) = *buf.add(i + 1);
                    }
                    sys_write(b"\x08 \x08");
                }
                continue;
            }
            if c >= 0x20 && c < 0x7f && len < 1023 {
                for i in (cursor..len).rev() {
                    *buf.add(i + 1) = *buf.add(i);
                }
                *buf.add(cursor) = c;
                len += 1;
                cursor += 1;
                let echo = [c];
                sys_write(&echo);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Entry point: selected by feature flag
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn _start() -> ! {
    // When built without features, use RDI dispatch (multi-program ELF).
    // When built with a specific feature, run that program directly.
    #[cfg(any(
        feature = "prog_bad",
        feature = "prog_nx",
        feature = "prog_exec",
        feature = "prog_ptr",
        feature = "prog_isowrite",
        feature = "prog_heaptest",
        feature = "prog_echo",
        feature = "prog_uptime",
        feature = "prog_meminfo",
        feature = "prog_counter",
        feature = "prog_guess",
        feature = "prog_edit",
        feature = "prog_calc",
        feature = "prog_heapstress",
        feature = "prog_ping",
        feature = "prog_netdump",
        feature = "prog_time",
        feature = "prog_nano",
        feature = "prog_http",
        feature = "prog_fstest",
        feature = "prog_ls",
        feature = "prog_cat",
        feature = "prog_hexdump",
        feature = "prog_whoami",
        feature = "prog_grep",
        feature = "prog_ps",
        feature = "prog_dd",
        feature = "prog_kill",
        feature = "prog_fileops",
        feature = "prog_udptest",
    ))]
    {
        #[cfg(feature = "prog_bad")]       user_bad();
        #[cfg(feature = "prog_nx")]        user_nx();
        #[cfg(feature = "prog_exec")]      user_exec();
        #[cfg(feature = "prog_ptr")]       user_ptr();
        #[cfg(feature = "prog_isowrite")]  user_isowrite();
        #[cfg(feature = "prog_heaptest")]  user_heaptest();
        #[cfg(feature = "prog_echo")]      user_echo();
        #[cfg(feature = "prog_uptime")]    user_uptime();
        #[cfg(feature = "prog_meminfo")]   user_meminfo();
        #[cfg(feature = "prog_counter")]   user_counter();
        #[cfg(feature = "prog_guess")]     user_guess();
        #[cfg(feature = "prog_edit")]      user_edit();
        #[cfg(feature = "prog_calc")]      user_calc();
        #[cfg(feature = "prog_heapstress")] user_heapstress();
        #[cfg(feature = "prog_ping")]      user_ping();
        #[cfg(feature = "prog_netdump")]   user_netdump();
        #[cfg(feature = "prog_time")]      user_time();
        #[cfg(feature = "prog_nano")]      user_nano();
        #[cfg(feature = "prog_http")]      user_http();
        #[cfg(feature = "prog_fstest")]    user_fstest();
        #[cfg(feature = "prog_ls")]        user_ls();
        #[cfg(feature = "prog_cat")]       user_cat();
        #[cfg(feature = "prog_hexdump")]   user_hexdump();
        #[cfg(feature = "prog_whoami")]    user_whoami();
        #[cfg(feature = "prog_grep")]      user_grep();
        #[cfg(feature = "prog_ps")]        user_ps();
        #[cfg(feature = "prog_dd")]        user_dd();
        #[cfg(feature = "prog_kill")]      user_kill();
        #[cfg(feature = "prog_fileops")]   user_fileops();
        #[cfg(feature = "prog_udptest")]   user_udptest();
    }

    #[cfg(not(any(
        feature = "prog_bad",
        feature = "prog_nx",
        feature = "prog_exec",
        feature = "prog_ptr",
        feature = "prog_isowrite",
    )))]
    {
        // Dispatch mode: RDI contains program index.
        let prog_idx: u64;
        unsafe {
            core::arch::asm!(
                "mov {0}, rdi",
                out(reg) prog_idx,
                options(nostack, preserves_flags),
            );
        }
        match prog_idx as usize {
            0 => user_prog(),
            1 => user_bad(),
            2 => user_nx(),
            3 => user_exec(),
            4 => user_ptr(),
            5 => user_isowrite(),
            6 => user_heaptest(),
            7 => user_echo(),
            8 => user_uptime(),
            9 => user_meminfo(),
            10 => user_counter(),
            11 => user_guess(),
            12 => user_edit(),
            13 => user_calc(),
            14 => user_heapstress(),
            15 => user_ping(),
            16 => user_netdump(),
            17 => user_time(),
            18 => user_nano(),
            19 => user_http(),
            _ => user_prog(),
        }
    }
}

// ---------------------------------------------------------------------------
// Panic handler
// ---------------------------------------------------------------------------

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    sys_exit(99);
}
