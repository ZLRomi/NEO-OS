#include "cmd.h"
#include "terminal.h"
#include "serial.h"
#include "printk.h"
#include "io.h"
#include "memory.h"
#include "heap.h"
#include "idt.h"
#include "gui.h"
#include "net.h"
#include "disk.h"
#include "disk_sched.h"
#include "fat32.h"
#include "process.h"
#include "klog.h"
#include "gfx_settings.h"
#include "demo_3d.h"
#include "download.h"
#include "player.h"
#include "gui_internal.h"
#include "power.h"
#include "spool.h"
#include "rtc.h"
#include "rtc.h"
#include "userdb.h"
#include "pipe.h"
#include "tty.h"
#include "procfs.h"
#include <stdint.h>
#include <stddef.h>

const char* cmd_arg = NULL;

static int g_redirect_fd = -1;
static char g_pipe_buf[8192];
static int g_pipe_len = 0;

static void __attribute__((unused)) redirect_sink(const char* s, int len) {
    if (g_redirect_fd < 0) return;
    if (g_redirect_fd >= PIPE_FD_BASE && g_redirect_fd < PIPE_FD_END) {
        pipe_write_fd(g_redirect_fd, s, len);
    } else if (g_redirect_fd >= 0 && g_redirect_fd < MAX_FD) {
        fat32_write(g_redirect_fd, s, len);
    }
}

static void __attribute__((unused)) pipe_collect_sink(const char* s, int len) {
    int i;
    for (i = 0; i < len && g_pipe_len < (int)sizeof(g_pipe_buf) - 1; i++) {
        g_pipe_buf[g_pipe_len++] = s[i];
    }
}

static uint32_t parse_hex(const char* s);
static int strcmp(const char* a, const char* b);

// ========== 命令实现 ==========
static void cmd_help(const char* arg) {
    (void)arg;
    terminal_write("Available commands:\n");
    terminal_write("  help     - show this help\n");
    terminal_write("  clear    - clear GSHELL output\n");
    terminal_write("  reboot   - reboot the system\n");
    terminal_write("  shutdown - power off the system\n");
    terminal_write("  meminfo  - show memory info\n");
    terminal_write("  alloc    - allocate a page\n");
    terminal_write("  free     - free a page\n");
    terminal_write("  kmalloc  - allocate from heap\n");
    terminal_write("  kfree    - free heap memory\n");
    terminal_write("  demo     - bring demo window to front\n");
    terminal_write("  menu     - toggle start menu\n");
    terminal_write("  ls [path] - list directory\n");
    terminal_write("  cat <file> - show file content\n");
    terminal_write("  cd <path>  - change directory\n");
    terminal_write("  mkdir <name> - create directory\n");
    terminal_write("  rm <name>   - remove file\n");
    terminal_write("  pwd      - show current directory\n");
    terminal_write("  ps       - list processes\n");
    terminal_write("  top      - show processes\n");
    terminal_write("  klog     - dump kernel log\n");
    terminal_write("  taskmgr  - open Task Manager\n");
    terminal_write("  display  - open Display Settings\n");
    terminal_write("  demo3d   - toggle 3D rotating cube demo\n");
    terminal_write("  download <url> - download a file\n");
    terminal_write("  downloads - open Downloads window\n");
    terminal_write("  play <path> - play MP3/MP4 file\n");
    terminal_write("  player - open Media Player\n");
    terminal_write("  print <text> - submit a print job\n");
    terminal_write("  lpq - list print queue\n");
    terminal_write("  lprm <id> - cancel a print job\n");
    terminal_write("  date    - show current date and time\n");
    terminal_write("  disksched [fcfs|sstf|scan|cscan] - show/set disk scheduler policy\n");
    terminal_write("  whoami  - show current user\n");
    terminal_write("  useradd <name> <pwd> [uid] [gid] - add a user\n");
    terminal_write("  userdel <name> - delete a user\n");
    terminal_write("  su <name> [password] - switch user\n");
    terminal_write("  kill <pid> [sig] - send signal to process\n");
}

static void cmd_demo(const char* arg) {
    (void)arg;
    window_t* win = window_find_by_type(WINDOW_TYPE_DEMO);
    if (win) {
        win->visible = 1;
        window_bring_to_front(win);
    } else {
        terminal_write("No demo window\n");
    }
}

static void cmd_menu(const char* arg) {
    (void)arg;
    start_menu_toggle();
}

static void cmd_ping(const char* arg) {
    if (!arg || *arg == '\0') {
        gshell_puts("Usage: ping <ip>\n");
        return;
    }
    uint32_t ip = net_parse_ip(arg);
    if (!ip) {
        gshell_puts("Invalid IP\n");
        return;
    }
    gshell_puts("Pinging ");
    gshell_puts(arg);
    gshell_puts(" ...\n");
    if (ping(ip, 150)) {
        gshell_puts("Reply received\n");
    } else {
        gshell_puts("Timeout\n");
    }
}

static void cmd_dns(const char* arg) {
    if (!arg || *arg == '\0') {
        gshell_puts("Usage: dns <domain>\n");
        return;
    }
    uint32_t ip = net_dns_resolve(arg, 150);
    if (ip) {
        char buf[16];
        net_ip_to_str(ip, buf);
        gshell_puts(buf);
        gshell_puts("\n");
    } else {
        gshell_puts("DNS timeout\n");
    }
}

static void cmd_clear(const char* arg) {
    (void)arg;
    terminal_clear();
}

static void cmd_reboot(const char* arg) {
    (void)arg;
    terminal_write("Rebooting...\n");
    power_reboot();
    while (1) __asm__("hlt");
}

static void cmd_shutdown(const char* arg) {
    (void)arg;
    terminal_write("Shutting down...\n");
    power_off();
    while (1) __asm__("hlt");
}

static void cmd_meminfo(const char* arg) {
    (void)arg;
    print_memory_info();
    uint32_t total = pmm_get_total_pages();
    uint32_t freep = pmm_get_free_pages();
    uint32_t used = (total > freep) ? total - freep : 0;
    printk("PMM: total=%d pages, free=%d pages, used=%d pages (%d KB free)\n",
           (int)total, (int)freep, (int)used, (int)(freep * 4));
}

static void cmd_alloc(const char* arg) {
    (void)arg;
    uint32_t addr = pmm_alloc_page();
    if (addr) {
        printk("Allocated page at 0x%x\n", addr);
    } else {
        printk("No free pages!\n");
    }
}

static void cmd_kmalloc(const char* arg) {
    if (!arg || *arg == '\0') {
        printk("Usage: kmalloc <size>\n");
        return;
    }
    size_t size = parse_hex(arg);
    void* ptr = kmalloc(size);
    if (ptr) {
        printk("kmalloc(%d) = 0x%x\n", size, (uint32_t)ptr);
    } else {
        printk("kmalloc failed\n");
    }
}

static void cmd_kfree(const char* arg) {
    if (!arg || *arg == '\0') {
        printk("Usage: kfree <address>\n");
        return;
    }
    uint32_t addr = parse_hex(arg);
    kfree((void*)addr);
    printk("kfree(0x%x)\n", addr);
}

static uint32_t parse_hex(const char* s) {
    uint32_t val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s;
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else break;
        val = val * 16 + digit;
        s++;
    }
    return val;
}

static void cmd_free(const char* arg) {
    if (!arg || *arg == '\0') {
        printk("Usage: free <address>\n");
        return;
    }
    uint32_t addr = parse_hex(arg);
    if (addr == 0) {
        printk("Invalid address\n");
        return;
    }
    pmm_free_page(addr);
    printk("Freed page at 0x%x\n", addr);
}

static void cmd_ls(const char* arg) {
    if (!fat32_is_mounted()) {
        terminal_write("Filesystem not mounted\n");
        return;
    }
    const char* path = (arg && *arg) ? arg : ".";
    int dh = fat32_opendir(path);
    if (dh < 0) {
        terminal_write("Cannot open directory\n");
        return;
    }
    char name[16];
    int isdir;
    while (fat32_readdir(dh, name, &isdir, 0)) {
        if (isdir) {
            terminal_write("[DIR]  ");
        } else {
            terminal_write("       ");
        }
        terminal_write(name);
        terminal_write("\n");
    }
    fat32_close(dh);
}

static void cmd_cat(const char* arg) {
    if (!arg || *arg == '\0') {
        terminal_write("Usage: cat <file>\n");
        return;
    }
    if (arg[0] == '/' && arg[1] == 'p' && arg[2] == 'r' && arg[3] == 'o' && arg[4] == 'c' && (arg[5] == '/' || arg[5] == '\0')) {
        static char pbuf[512];
        int n = procfs_read(arg, pbuf, sizeof(pbuf) - 1);
        if (n < 0) {
            terminal_write("procfs: cannot read\n");
            return;
        }
        pbuf[n] = '\0';
        terminal_write(pbuf);
        terminal_write("\n");
        return;
    }
    if (!fat32_is_mounted()) {
        terminal_write("Filesystem not mounted\n");
        return;
    }
    int fd = fat32_open(arg, 0);
    if (fd < 0) {
        terminal_write("Cannot open file\n");
        return;
    }
    char buf[513];
    int n;
    int total = 0;
    while ((n = fat32_read(fd, buf, 512)) > 0) {
        buf[n] = 0;
        terminal_write(buf);
        total += n;
        if (total > 65536) break;
    }
    fat32_close(fd);
    terminal_write("\n");
}

static void cmd_cd(const char* arg) {
    if (!fat32_is_mounted()) {
        terminal_write("Filesystem not mounted\n");
        return;
    }
    if (!arg || *arg == '\0') {
        fat32_setcwd("/");
        return;
    }
    fat32_setcwd(arg);
}

static void cmd_mkdir(const char* arg) {
    if (!fat32_is_mounted()) {
        terminal_write("Filesystem not mounted\n");
        return;
    }
    if (!arg || *arg == '\0') {
        terminal_write("Usage: mkdir <name>\n");
        return;
    }
    if (fat32_mkdir(arg) < 0) {
        terminal_write("mkdir failed\n");
    } else {
        terminal_write("Directory created\n");
    }
}

static void cmd_rm(const char* arg) {
    if (!fat32_is_mounted()) {
        terminal_write("Filesystem not mounted\n");
        return;
    }
    if (!arg || *arg == '\0') {
        terminal_write("Usage: rm <name>\n");
        return;
    }
    if (fat32_remove(arg) < 0) {
        terminal_write("rm failed\n");
    } else {
        terminal_write("Removed\n");
    }
}

static void cmd_pwd(const char* arg) {
    (void)arg;
    if (!fat32_is_mounted()) {
        terminal_write("Filesystem not mounted\n");
        return;
    }
    terminal_write(fat32_getcwd());
    terminal_write("\n");
}

static const char* state_name(int s) {
    switch (s) {
        case 0: return "READY";
        case 1: return "RUN";
        case 2: return "BLOCK";
        case 3: return "TERM";
        default: return "?";
    }
}

static int field_int(const char* text, const char* key) {
    const char* p = text;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n') eol++;
        const char* c = p;
        while (c < eol && *c != ':') c++;
        if (c < eol) {
            int klen = c - p;
            int i = 0, match = 1;
            while (i < klen && key[i]) {
                if (key[i] != p[i]) { match = 0; break; }
                i++;
            }
            if (match && key[i] == '\0' && i == klen) {
                const char* v = c + 1;
                int val = 0;
                while (v < eol && *v >= '0' && *v <= '9') {
                    val = val * 10 + (*v - '0');
                    v++;
                }
                return val;
            }
        }
        if (*eol == 0) break;
        p = eol + 1;
    }
    return -1;
}

static int field_str(const char* text, const char* key, char* out, int outmax) {
    const char* p = text;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n') eol++;
        const char* c = p;
        while (c < eol && *c != ':') c++;
        if (c < eol) {
            int klen = c - p;
            int i = 0, match = 1;
            while (i < klen && key[i]) {
                if (key[i] != p[i]) { match = 0; break; }
                i++;
            }
            if (match && key[i] == '\0' && i == klen) {
                const char* v = c + 1;
                int vlen = eol - v;
                int copy = vlen < outmax - 1 ? vlen : outmax - 1;
                int j;
                for (j = 0; j < copy; j++) out[j] = v[j];
                out[copy] = '\0';
                return copy;
            }
        }
        if (*eol == 0) break;
        p = eol + 1;
    }
    out[0] = '\0';
    return 0;
}

static void cmd_ps(const char* arg) {
    (void)arg;
    static char listbuf[1024];
    int n = procfs_readdir("/proc", listbuf, sizeof(listbuf));
    if (n <= 0) {
        terminal_write("proc: no processes\n");
        return;
    }
    printk("PID  NAME            STATE  PRI\n");
    int count = 0;
    int i = 0;
    while (i < n) {
        const char* entry = listbuf + i;
        int elen = 0;
        while (entry[elen]) elen++;
        i += elen + 1;
        if (!elen || !(entry[0] >= '0' && entry[0] <= '9')) continue;
        char path[40];
        int p = 0;
        path[p++] = '/'; path[p++] = 'p'; path[p++] = 'r'; path[p++] = 'o'; path[p++] = 'c'; path[p++] = '/';
        int e = 0;
        while (entry[e] && p < 34) path[p++] = entry[e++];
        path[p++] = '/'; path[p++] = 's'; path[p++] = 't'; path[p++] = 'a'; path[p++] = 't'; path[p++] = 'u'; path[p++] = 's';
        path[p] = '\0';
        static char sbuf[256];
        int sn = procfs_read(path, sbuf, sizeof(sbuf) - 1);
        if (sn < 0) continue;
        sbuf[sn] = '\0';
        int pid = field_int(sbuf, "pid");
        int state = field_int(sbuf, "state");
        int priority = field_int(sbuf, "priority");
        char name[16];
        field_str(sbuf, "name", name, sizeof(name));
        if (pid < 0) continue;
        char line[64];
        int j = 0;
        char pb[12];
        int pi = 0;
        int pv = pid;
        if (pv == 0) pb[pi++] = '0';
        while (pv > 0) { pb[pi++] = '0' + (pv % 10); pv /= 10; }
        while (pi > 0 && j < 4) line[j++] = pb[--pi];
        while (j < 4) line[j++] = ' ';
        line[j++] = ' ';
        int k = 0;
        while (name[k] && j < 4 + 1 + 15) line[j++] = name[k++];
        while (j < 4 + 1 + 15) line[j++] = ' ';
        line[j++] = ' ';
        const char* st = state_name(state);
        while (*st && j < 4 + 1 + 15 + 1 + 5) line[j++] = *st++;
        while (j < 4 + 1 + 15 + 1 + 5) line[j++] = ' ';
        line[j++] = ' ';
        char pr[12];
        int pri = 0;
        int pvv = priority;
        if (pvv < 0) { line[j++] = '-'; pvv = -pvv; }
        if (pvv == 0) pr[pri++] = '0';
        while (pvv > 0) { pr[pri++] = '0' + (pvv % 10); pvv /= 10; }
        while (pri > 0 && j < 63) line[j++] = pr[--pri];
        line[j] = '\0';
        printk("%s\n", line);
        count++;
    }
    printk("Total: %d processes\n", count);
}

static void cmd_top(const char* arg) {
    cmd_ps(arg);
    cmd_meminfo(NULL);
}

static void cmd_klog(const char* arg) {
    (void)arg;
    static char buf[2048];
    int n = klog_read(buf, sizeof(buf));
    if (n <= 0) {
        terminal_write("klog: empty\n");
        return;
    }
    terminal_write("===== Kernel Log (");
    char num[12];
    int j = 0;
    int v = n;
    if (v == 0) num[j++] = '0';
    while (v > 0) { num[j++] = '0' + (v % 10); v /= 10; }
    num[j] = '\0';
    char rev[12];
    int k = 0;
    while (j > 0) rev[k++] = num[--j];
    rev[k] = '\0';
    terminal_write(rev);
    terminal_write(" bytes) =====\n");
    terminal_write(buf);
    terminal_write("\n===== End Log =====\n");
}

static void cmd_taskmgr(const char* arg) {
    (void)arg;
    window_t* win = window_find_by_type(WINDOW_TYPE_TASKMGR);
    if (!win) {
        terminal_write("Task Manager unavailable\n");
        return;
    }
    win->visible = 1;
    win->minimized = 0;
    window_bring_to_front(win);
    terminal_write("Task Manager opened\n");
}

static void cmd_display(const char* arg) {
    (void)arg;
    gfx_settings_open();
    terminal_write("Display Settings opened\n");
}

static void cmd_demo3d(const char* arg) {
    (void)arg;
    demo_3d_toggle();
    if (demo_3d_is_active()) {
        terminal_write("3D demo: ON (rotating cube)\n");
    } else {
        terminal_write("3D demo: OFF\n");
    }
}

static void cmd_download(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: download <url> [save_dir]\n");
        return;
    }
    const char* save_dir = "/downloads";
    int i = 0;
    int url_end = 0;
    while (arg[i]) {
        if (arg[i] == ' ') {
            url_end = i;
            while (arg[i] == ' ') i++;
            if (arg[i]) save_dir = &arg[i];
            break;
        }
        i++;
    }
    char url_buf[512];
    int ulen = url_end > 0 ? url_end : i;
    if (ulen >= 512) ulen = 511;
    for (int k = 0; k < ulen; k++) url_buf[k] = arg[k];
    url_buf[ulen] = '\0';
    int r = download_start(url_buf, save_dir);
    if (r > 0) {
        terminal_write("Downloaded: ");
        char buf[16];
        int_to_str(r, buf);
        terminal_write(buf);
        terminal_write(" bytes\n");
    } else {
        terminal_write("Download failed\n");
    }
}

static void cmd_downloads(const char* arg) {
    (void)arg;
    window_t* win = window_find_by_type(WINDOW_TYPE_DOWNLOADS);
    if (!win) {
        terminal_write("Downloads window unavailable\n");
        return;
    }
    win->visible = 1;
    win->minimized = 0;
    window_bring_to_front(win);
    terminal_write("Downloads opened\n");
}

static void cmd_play(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: play <file_path>\n");
        return;
    }
    if (!player_is_supported(arg)) {
        terminal_write("Unsupported format (use .mp3/.mp4)\n");
        return;
    }
    int r = player_open(arg);
    if (r == 0) {
        window_t* win = window_find_by_type(WINDOW_TYPE_PLAYER);
        if (win) {
            win->visible = 1;
            win->minimized = 0;
            window_bring_to_front(win);
        }
        player_play();
        terminal_write("Playing: ");
        terminal_write(arg);
        terminal_write("\n");
    } else {
        terminal_write("Failed to open file\n");
    }
}

static void cmd_player(const char* arg) {
    (void)arg;
    window_t* win = window_find_by_type(WINDOW_TYPE_PLAYER);
    if (!win) {
        terminal_write("Media Player unavailable\n");
        return;
    }
    win->visible = 1;
    win->minimized = 0;
    window_bring_to_front(win);
    terminal_write("Media Player opened\n");
}

static void cmd_disksched(const char* arg) {
    if (arg && *arg) {
        disk_sched_policy_t p;
        if (strcmp(arg, "fcfs") == 0) {
            p = DISK_SCHED_FCFS;
        } else if (strcmp(arg, "sstf") == 0) {
            p = DISK_SCHED_SSTF;
        } else if (strcmp(arg, "scan") == 0) {
            p = DISK_SCHED_SCAN;
        } else if (strcmp(arg, "cscan") == 0) {
            p = DISK_SCHED_CSCAN;
        } else {
            terminal_write("Usage: disksched [fcfs|sstf|scan|cscan]\n");
            return;
        }
        disk_sched_set_policy(p);
    }
    const char* name = "unknown";
    switch (disk_sched_get_policy()) {
        case DISK_SCHED_FCFS:  name = "FCFS";   break;
        case DISK_SCHED_SSTF:  name = "SSTF";   break;
        case DISK_SCHED_SCAN:  name = "SCAN";   break;
        case DISK_SCHED_CSCAN: name = "C-SCAN"; break;
    }
    terminal_write("Disk scheduler policy: ");
    terminal_write(name);
    terminal_write("\n");

    int pending = disk_sched_pending();
    char num[12];
    int j = 0;
    int v = pending;
    if (v == 0) num[j++] = '0';
    while (v > 0) { num[j++] = '0' + (v % 10); v /= 10; }
    num[j] = '\0';
    char rev[12];
    int k = 0;
    while (j > 0) rev[k++] = num[--j];
    rev[k] = '\0';
    terminal_write("Pending requests: ");
    terminal_write(rev);
    terminal_write("\n");
}

static void cmd_date(const char* arg) {
    (void)arg;
    rtc_time_t t;
    if (rtc_get_time(&t) < 0) {
        terminal_write("rtc: read failed\n");
        return;
    }
    char buf[32];
    int p = 0;
    int y = t.year;
    if (y < 0) y = 0;
    char yd[4];
    for (int k = 3; k >= 0; k--) { yd[k] = '0' + (y % 10); y /= 10; }
    for (int k = 0; k < 4; k++) buf[p++] = yd[k];
    buf[p++] = '-';
    buf[p++] = '0' + (t.month / 10);
    buf[p++] = '0' + (t.month % 10);
    buf[p++] = '-';
    buf[p++] = '0' + (t.day / 10);
    buf[p++] = '0' + (t.day % 10);
    buf[p++] = ' ';
    buf[p++] = '0' + (t.hour / 10);
    buf[p++] = '0' + (t.hour % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (t.minute / 10);
    buf[p++] = '0' + (t.minute % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (t.second / 10);
    buf[p++] = '0' + (t.second % 10);
    buf[p++] = '\n';
    buf[p] = '\0';
    terminal_write(buf);
}

// ========== 命令表 ==========
typedef struct {
    const char* name;
    const char* desc;
    void (*handler)(const char* arg);
} cmd_entry_t;

static void cmd_print(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: print <text>\n");
        return;
    }
    int len = 0;
    while (arg[len]) len++;
    int id = spool_submit("cmdline", arg, (uint32_t)len);
    if (id < 0) {
        terminal_write("Print submit failed\n");
    } else {
        printk("Submitted job #%d (%d bytes)\n", id, len);
    }
}

static void cmd_lpq(const char* arg) {
    (void)arg;
    print_job_t list[MAX_SPOOL_JOBS];
    int n = spool_list(list, MAX_SPOOL_JOBS);
    if (n == 0) {
        terminal_write("Print queue empty\n");
        return;
    }
    terminal_write("ID   STATUS     SIZE    OWNER           NAME\n");
    for (int i = 0; i < n; i++) {
        const char* st;
        switch (list[i].status) {
            case 0: st = "pending  "; break;
            case 1: st = "printing "; break;
            case 2: st = "done     "; break;
            case 3: st = "cancelled"; break;
            default: st = "unknown  "; break;
        }
        printk("%d | %s | %d | %s | %s\n",
               list[i].job_id, st, list[i].size, list[i].owner, list[i].name);
    }
    printk("Pending jobs: %d\n", spool_count());
}

static void cmd_lprm(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: lprm <job_id>\n");
        return;
    }
    int id = 0;
    while (*arg >= '0' && *arg <= '9') {
        id = id * 10 + (*arg - '0');
        arg++;
    }
    if (id <= 0) {
        terminal_write("Invalid job id\n");
        return;
    }
    if (spool_cancel(id) < 0) {
        terminal_write("Cancel failed (not pending or not found)\n");
    } else {
        printk("Job #%d cancelled\n", id);
    }
}

static void cmd_whoami(const char* arg) {
    (void)arg;
    const char* name = userdb_get_username();
    char num[12];
    int j = 0;
    uint32_t v = userdb_get_uid();
    if (v == 0) num[j++] = '0';
    while (v > 0) { num[j++] = '0' + (v % 10); v /= 10; }
    num[j] = '\0';
    char rev[12];
    int k = 0;
    while (j > 0) rev[k++] = num[--j];
    rev[k] = '\0';
    terminal_write(name);
    terminal_write(" (uid=");
    terminal_write(rev);
    terminal_write(")\n");
}

static void cmd_useradd(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: useradd <name> <password> [uid] [gid]\n");
        return;
    }
    char name[32];
    int i = 0;
    const char* p = arg;
    while (*p && *p != ' ' && i < 31) name[i++] = *p++;
    name[i] = 0;
    while (*p == ' ') p++;
    if (!*p) {
        terminal_write("Usage: useradd <name> <password> [uid] [gid]\n");
        return;
    }
    char pwd[64];
    int j = 0;
    while (*p && *p != ' ' && j < 63) pwd[j++] = *p++;
    pwd[j] = 0;
    while (*p == ' ') p++;

    uint32_t uid = 0;
    int has_uid = 0;
    if (*p >= '0' && *p <= '9') {
        has_uid = 1;
        while (*p >= '0' && *p <= '9') { uid = uid * 10 + (*p - '0'); p++; }
    }
    uint32_t gid = uid;
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        gid = 0;
        while (*p >= '0' && *p <= '9') { gid = gid * 10 + (*p - '0'); p++; }
    }
    if (!has_uid) uid = userdb_alloc_uid();

    if (userdb_add_user(name, pwd, uid, gid) < 0) {
        terminal_write("useradd failed\n");
        return;
    }
    printk("User '%s' added (uid=%d gid=%d)\n", name, uid, gid);
}

static void cmd_userdel(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: userdel <name>\n");
        return;
    }
    if (userdb_del_user(arg) < 0) {
        terminal_write("userdel failed\n");
    } else {
        terminal_write("User deleted\n");
    }
}

static void cmd_su(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: su <name> [password]\n");
        return;
    }
    char name[32];
    int i = 0;
    const char* p = arg;
    while (*p && *p != ' ' && i < 31) name[i++] = *p++;
    name[i] = 0;
    while (*p == ' ') p++;
    const char* pwd = p;
    if (userdb_su(name, pwd) < 0) {
        terminal_write("su failed (bad credentials)\n");
    } else {
        const char* uname = userdb_get_username();
        terminal_write("Switched to ");
        terminal_write(uname);
        terminal_write("\n");
    }
}

static void cmd_kill(const char* arg) {
    if (!arg || !*arg) {
        terminal_write("Usage: kill <pid> [sig]\n");
        return;
    }
    int pid = 0;
    while (*arg >= '0' && *arg <= '9') {
        pid = pid * 10 + (*arg - '0');
        arg++;
    }
    if (pid <= 0) {
        terminal_write("Invalid pid\n");
        return;
    }
    while (*arg == ' ') arg++;
    int sig = SIGTERM;
    if (*arg >= '0' && *arg <= '9') {
        sig = 0;
        while (*arg >= '0' && *arg <= '9') {
            sig = sig * 10 + (*arg - '0');
            arg++;
        }
    }
    int r = signal_send(pid, sig);
    if (r < 0) {
        printk("kill: failed (pid=%d sig=%d err=%d)\n", pid, sig, r);
    } else {
        printk("kill: sent sig=%d to pid=%d\n", sig, pid);
    }
}


static const cmd_entry_t cmd_table[] = {
    {"help",    "显示此帮助",      cmd_help},
    {"clear",   "清空终端输出", cmd_clear},
    {"reboot",  "重启系统",       cmd_reboot},
    {"shutdown","关闭系统",    cmd_shutdown},
    {"meminfo", "显示内存信息",    cmd_meminfo},
    {"alloc",   "分配一页内存",     cmd_alloc},
    {"free",    "释放一页内存",         cmd_free},
    {"kmalloc", "从堆分配内存",  cmd_kmalloc},
    {"kfree",   "释放堆内存",    cmd_kfree},
    {"demo",    "将演示窗口置于前台", cmd_demo},
    {"menu",    "切换开始菜单",   cmd_menu},
    {"ping",    "Ping IP 地址",  cmd_ping},
    {"dns",     "解析域名", cmd_dns},
    {"ls",      "列出目录",      cmd_ls},
    {"cat",     "显示文件内容",   cmd_cat},
    {"cd",      "切换目录",    cmd_cd},
    {"mkdir",   "创建目录",    cmd_mkdir},
    {"rm",      "删除文件",         cmd_rm},
    {"pwd",     "显示当前目录",    cmd_pwd},
    {"ps",      "列出进程",      cmd_ps},
    {"top",     "显示进程",      cmd_top},
    {"klog",    "转储内核日志",     cmd_klog},
    {"taskmgr", "打开任务管理器",   cmd_taskmgr},
    {"display", "打开显示设置", cmd_display},
    {"demo3d", "切换 3D 立方体演示", cmd_demo3d},
    {"download", "下载文件", cmd_download},
    {"downloads", "打开下载窗口", cmd_downloads},
    {"play", "播放媒体文件", cmd_play},
    {"player", "打开媒体播放器", cmd_player},
    {"print", "提交打印任务", cmd_print},
    {"lpq", "列出打印队列", cmd_lpq},
    {"lprm", "取消打印任务", cmd_lprm},
    {"disksched", "显示/设置磁盘调度策略", cmd_disksched},
    {"date", "显示当前日期和时间", cmd_date},
    {"whoami", "显示当前用户", cmd_whoami},
    {"useradd", "添加用户（用户名 密码 [uid] [gid]）", cmd_useradd},
    {"userdel", "删除用户", cmd_userdel},
    {"su", "切换用户（用户名 [密码]）", cmd_su},
    {"kill", "向进程发送信号（pid [sig]）", cmd_kill},
};

#define CMD_COUNT (sizeof(cmd_table) / sizeof(cmd_entry_t))

static int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

// ========== 解析入口 ==========
int cmd_getline(char* buf, int max) {
    if (!buf || max <= 0) return 0;
    int total = 0;
    while (total < max - 1) {
        char c = 0;
        int n = tty_read(0, &c, 1);
        if (n <= 0) break;
        buf[total++] = c;
        if (c == '\n') break;
    }
    if (total < max) buf[total] = '\0';
    return total;
}

static void execute_single(const char* line, const char* override_arg) {
    while (*line == ' ') line++;
    if (*line == '\0') return;
    char cmd_buf[32];
    int i = 0;
    while (*line && *line != ' ' && i < 31) {
        cmd_buf[i++] = *line++;
    }
    cmd_buf[i] = '\0';
    if (override_arg) {
        cmd_arg = override_arg;
    } else {
        while (*line == ' ') line++;
        cmd_arg = line;
    }
    for (unsigned int j = 0; j < CMD_COUNT; j++) {
        if (strcmp(cmd_buf, cmd_table[j].name) == 0) {
            cmd_table[j].handler(cmd_arg);
            return;
        }
    }
    terminal_write("Unknown command: ");
    terminal_write(cmd_buf);
    terminal_write("\n");
}

void cmd_parse(const char* input) {
    while (*input == ' ') input++;
    if (*input == '\0') {
        terminal_write("\n");
        return;
    }

    static char line[512];
    int i;
    for (i = 0; input[i] && i < 511; i++) line[i] = input[i];
    line[i] = 0;

    int pipe_pos = -1;
    for (i = 0; line[i]; i++) {
        if (line[i] == '|') { pipe_pos = i; break; }
    }
    if (pipe_pos >= 0) {
        line[pipe_pos] = 0;
        char* cmd2 = line + pipe_pos + 1;
        while (*cmd2 == ' ') cmd2++;

        g_pipe_len = 0;
        terminal_set_redirect(pipe_collect_sink);
        execute_single(line, NULL);
        terminal_set_redirect(NULL);

        int rfd, wfd;
        if (pipe_create(&rfd, &wfd) == 0) {
            pipe_write_fd(wfd, g_pipe_buf, g_pipe_len);
            pipe_close(wfd);
            g_pipe_len = pipe_read_fd(rfd, g_pipe_buf, sizeof(g_pipe_buf) - 1);
            g_pipe_buf[g_pipe_len] = 0;
            pipe_close(rfd);
        } else {
            g_pipe_buf[g_pipe_len] = 0;
        }
        execute_single(cmd2, g_pipe_buf);
        return;
    }

    char redir_file[MAX_PATH];
    int mode = 0;
    int redir_pos = -1;
    for (i = 0; line[i]; i++) {
        if (line[i] == '>') {
            redir_pos = i;
            if (line[i+1] == '>') { mode = 2; i += 2; }
            else { mode = 1; i++; }
            while (line[i] == ' ') i++;
            int j = 0;
            while (line[i] && line[i] != ' ' && j < MAX_PATH - 1) redir_file[j++] = line[i++];
            redir_file[j] = 0;
            break;
        } else if (line[i] == '<') {
            redir_pos = i;
            mode = 3;
            i++;
            while (line[i] == ' ') i++;
            int j = 0;
            while (line[i] && line[i] != ' ' && j < MAX_PATH - 1) redir_file[j++] = line[i++];
            redir_file[j] = 0;
            break;
        }
    }
    if (redir_pos >= 0) {
        int end = redir_pos;
        while (end > 0 && line[end-1] == ' ') end--;
        line[end] = 0;
    }

    if (mode == 1 || mode == 2) {
        int fd = fat32_open(redir_file, 1);
        if (fd < 0) {
            terminal_write("Cannot open file\n");
            return;
        }
        if (mode == 2) {
            char tmp[512];
            while (fat32_read(fd, tmp, sizeof(tmp)) > 0) {}
        }
        g_redirect_fd = fd;
        terminal_set_redirect(redirect_sink);
        execute_single(line, NULL);
        terminal_set_redirect(NULL);
        fat32_close(fd);
        g_redirect_fd = -1;
        return;
    }

    if (mode == 3) {
        int fd = fat32_open(redir_file, 0);
        if (fd < 0) {
            terminal_write("Cannot open file\n");
            return;
        }
        static char in_buf[2048];
        int total = 0, n;
        while (total < (int)sizeof(in_buf) - 1 &&
               (n = fat32_read(fd, in_buf + total, (int)sizeof(in_buf) - 1 - total)) > 0) {
            total += n;
        }
        in_buf[total] = 0;
        fat32_close(fd);
        execute_single(line, in_buf);
        return;
    }

    execute_single(line, NULL);
}
