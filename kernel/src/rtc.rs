//! NEO-OS-3 MC146818 RTC driver.
//!
//! Reads date/time from the CMOS real-time clock via I/O ports 0x70/0x71.
//! QEMU emulates this device; real hardware uses the same interface.

use crate::cpu::{inb, outb};
use crate::kinfo;

/// CMOS RTC I/O ports.
const CMOS_ADDR: u16 = 0x70;
const CMOS_DATA: u16 = 0x71;

/// RTC register indices.
const REG_SECONDS: u8 = 0x00;
const REG_MINUTES: u8 = 0x02;
const REG_HOURS: u8 = 0x04;
const REG_DAY_OF_WEEK: u8 = 0x06;
const REG_DAY: u8 = 0x07;
const REG_MONTH: u8 = 0x08;
const REG_YEAR: u8 = 0x09;
const REG_STATUS_A: u8 = 0x0A;
const REG_STATUS_B: u8 = 0x0B;

/// Date/time structure.
#[derive(Clone, Copy)]
pub struct DateTime {
    pub year: u16,
    pub month: u8,
    pub day: u8,
    pub hour: u8,
    pub minute: u8,
    pub second: u8,
    pub day_of_week: u8, // 1=Sunday, 7=Saturday
}

impl DateTime {
    /// Format as "YYYY-MM-DD HH:MM:SS".
    pub fn format(&self, buf: &mut [u8; 20]) {
        // YYYY-MM-DD HH:MM:SS\0
        buf[0] = b'0' + (self.year / 1000) as u8;
        buf[1] = b'0' + ((self.year / 100) % 10) as u8;
        buf[2] = b'0' + ((self.year / 10) % 10) as u8;
        buf[3] = b'0' + (self.year % 10) as u8;
        buf[4] = b'-';
        buf[5] = b'0' + self.month / 10;
        buf[6] = b'0' + self.month % 10;
        buf[7] = b'-';
        buf[8] = b'0' + self.day / 10;
        buf[9] = b'0' + self.day % 10;
        buf[10] = b' ';
        buf[11] = b'0' + self.hour / 10;
        buf[12] = b'0' + self.hour % 10;
        buf[13] = b':';
        buf[14] = b'0' + self.minute / 10;
        buf[15] = b'0' + self.minute % 10;
        buf[16] = b':';
        buf[17] = b'0' + self.second / 10;
        buf[18] = b'0' + self.second % 10;
        buf[19] = 0;
    }

    /// Get day of week name.
    pub fn weekday_name(&self) -> &'static str {
        match self.day_of_week {
            1 => "Sun",
            2 => "Mon",
            3 => "Tue",
            4 => "Wed",
            5 => "Thu",
            6 => "Fri",
            7 => "Sat",
            _ => "???",
        }
    }

    /// Get month name.
    pub fn month_name(&self) -> &'static str {
        match self.month {
            1 => "Jan", 2 => "Feb", 3 => "Mar", 4 => "Apr",
            5 => "May", 6 => "Jun", 7 => "Jul", 8 => "Aug",
            9 => "Sep", 10 => "Oct", 11 => "Nov", 12 => "Dec",
            _ => "???",
        }
    }
}

/// Read a CMOS register.
fn cmos_read(reg: u8) -> u8 {
    unsafe {
        outb(CMOS_ADDR, reg);
        inb(CMOS_DATA)
    }
}

/// Convert BCD to binary.
fn bcd_to_bin(bcd: u8) -> u8 {
    (bcd >> 4) * 10 + (bcd & 0x0F)
}

/// Check if RTC is in BCD mode (vs binary).
fn is_bcd_mode() -> bool {
    let status_b = cmos_read(REG_STATUS_B);
    status_b & 0x04 == 0 // bit 2 = 0 means BCD mode
}

/// Wait until RTC update cycle is complete.
fn wait_update_complete() {
    // Status register A bit 7 = update in progress.
    loop {
        let status_a = cmos_read(REG_STATUS_A);
        if status_a & 0x80 == 0 { break; }
        core::hint::spin_loop();
    }
}

/// Read the current date/time from the RTC.
pub fn read_rtc() -> DateTime {
    wait_update_complete();

    let bcd = is_bcd_mode();

    let mut seconds = cmos_read(REG_SECONDS);
    let mut minutes = cmos_read(REG_MINUTES);
    let mut hours = cmos_read(REG_HOURS);
    let mut dow = cmos_read(REG_DAY_OF_WEEK);
    let mut day = cmos_read(REG_DAY);
    let mut month = cmos_read(REG_MONTH);
    let mut year = cmos_read(REG_YEAR);

    if bcd {
        seconds = bcd_to_bin(seconds);
        minutes = bcd_to_bin(minutes);
        hours = bcd_to_bin(hours & 0x7F) | (hours & 0x80); // preserve 12/24h flag
        dow = bcd_to_bin(dow);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
    }

    // Handle 12-hour mode.
    if hours & 0x80 != 0 {
        // 12-hour mode with PM flag.
        hours = (hours & 0x7F) + 12;
    }

    // Assume 21st century for 2-digit year.
    let full_year = 2000 + year as u16;

    DateTime {
        year: full_year,
        month,
        day,
        hour: hours,
        minute: minutes,
        second: seconds,
        day_of_week: dow,
    }
}

/// Initialize RTC driver (just logs detection).
pub fn init() {
    let dt = read_rtc();
    let mut buf = [0u8; 20];
    dt.format(&mut buf);
    let time_str = core::str::from_utf8(&buf[..19]).unwrap_or("????");
    kinfo!("rtc: {} (MC146818)", time_str);
}
