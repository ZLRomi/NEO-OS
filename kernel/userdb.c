#include "userdb.h"
#include "sha256.h"
#include "fat32.h"
#include "heap.h"
#include "printk.h"
#include "serial.h"
#include "idt.h"
#include "auth.h"
#include <stdint.h>
#include <stddef.h>

static user_record_t g_users[USERDB_MAX_USERS];
static user_session_t g_sessions[USERDB_MAX_SESSIONS];
static uint32_t g_current_uid = 0;
static uint32_t g_next_session_id = 1;
static int g_loaded = 0;

static int kstrlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int kstrcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void kstrcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void kmemset(void* dst, int v, int n) {
    uint8_t* p = (uint8_t*)dst;
    for (int i = 0; i < n; i++) p[i] = (uint8_t)v;
}

static void kmemcpy(void* dst, const void* src, int n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static int parse_uint(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static void uint_to_str(uint32_t v, char* out) {
    char buf[12];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0 && i < 11) { buf[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = buf[--i];
    out[j] = 0;
}

static const char hex_chars[] = "0123456789abcdef";

static void bytes_to_hex(const uint8_t* in, int len, char* out) {
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hex_chars[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex_chars[in[i] & 0xF];
    }
    out[len * 2] = 0;
}

static void gen_salt(char* out) {
    static uint32_t seed = 0xDEECE66Du;
    uint8_t raw[8];
    for (int i = 0; i < 8; i++) {
        seed = seed * 1103515245U + 12345U + timer_ticks;
        uint32_t v = seed ^ (seed >> 11);
        raw[i] = (uint8_t)(v ^ (v >> 8));
    }
    bytes_to_hex(raw, 8, out);
    out[16] = 0;
}

static void compute_hash(const char* salt, const char* password, char* out_hex) {
    uint8_t digest[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t*)salt, (size_t)kstrlen(salt));
    sha256_update(&ctx, (const uint8_t*)password, (size_t)kstrlen(password));
    sha256_final(&ctx, digest);
    bytes_to_hex(digest, 32, out_hex);
}

static void ensure_etc_dir(void) {
    int dh = fat32_opendir("/etc");
    if (dh >= 0) {
        fat32_close(dh);
    } else {
        fat32_mkdir("/etc");
    }
}

static int find_slot(void) {
    for (int i = 0; i < USERDB_MAX_USERS; i++) {
        if (!g_users[i].in_use) return i;
    }
    return -1;
}

static int find_by_name(const char* name) {
    for (int i = 0; i < USERDB_MAX_USERS; i++) {
        if (g_users[i].in_use && kstrcmp(g_users[i].username, name) == 0) return i;
    }
    return -1;
}

static int find_by_uid(uint32_t uid) {
    for (int i = 0; i < USERDB_MAX_USERS; i++) {
        if (g_users[i].in_use && g_users[i].uid == uid) return i;
    }
    return -1;
}

static int read_file(const char* path, char* buf, int bufsize) {
    int fd = fat32_open(path, 0);
    if (fd < 0) return -1;
    int total = 0, n;
    while (total < bufsize - 1 &&
           (n = fat32_read(fd, buf + total, bufsize - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = 0;
    fat32_close(fd);
    return total;
}

static char* field_next(char** p) {
    char* start = *p;
    if (*start == 0) return start;
    char* c = start;
    while (*c && *c != ':' && *c != '\n') c++;
    if (*c == ':') {
        *c = 0;
        *p = c + 1;
    } else {
        *p = c;
    }
    return start;
}

static int parse_passwd_line(char* line) {
    char* p = line;
    char* username = field_next(&p);
    if (*username == 0) return -1;
    char* placeholder = field_next(&p);
    char* uid_s = field_next(&p);
    char* gid_s = field_next(&p);
    char* home = field_next(&p);
    char* shell = p;

    int slot = find_slot();
    if (slot < 0) return -1;
    (void)placeholder;

    user_record_t* u = &g_users[slot];
    kmemset(u, 0, sizeof(*u));
    kstrcpy(u->username, username, USERDB_USERNAME_LEN);
    u->uid = (uint32_t)parse_uint(uid_s);
    u->gid = (uint32_t)parse_uint(gid_s);
    kstrcpy(u->home_dir, home, USERDB_HOME_LEN);
    kstrcpy(u->shell, shell, USERDB_SHELL_LEN);
    u->in_use = 1;
    return 0;
}

static int parse_shadow_line(char* line) {
    char* p = line;
    char* username = field_next(&p);
    if (*username == 0) return -1;
    char* hash = field_next(&p);
    char* salt = p;

    int idx = find_by_name(username);
    if (idx < 0) return -1;
    kstrcpy(g_users[idx].password_hash, hash, USERDB_HASH_LEN);
    kstrcpy(g_users[idx].salt, salt, USERDB_SALT_LEN);
    return 0;
}

static int parse_lines(char* buf, int (*cb)(char*)) {
    int count = 0;
    char* cur = buf;
    while (*cur) {
        char* line = cur;
        char* e = cur;
        while (*e && *e != '\n') e++;
        char* end = e;
        if (*end == '\n') {
            *end = 0;
            cur = end + 1;
        } else {
            cur = end;
        }
        if (end > line && end[-1] == '\r') end[-1] = 0;
        char* trim = line;
        while (*trim == ' ' || *trim == '\t' || *trim == '\r') trim++;
        if (*trim == 0 || *trim == '#') {
            if (*cur == 0) break;
            continue;
        }
        if (cb(trim) == 0) count++;
        if (*cur == 0) break;
    }
    return count;
}

void userdb_init(void) {
    for (int i = 0; i < USERDB_MAX_USERS; i++) g_users[i].in_use = 0;
    for (int i = 0; i < USERDB_MAX_SESSIONS; i++) g_sessions[i].active = 0;
    g_current_uid = 0;
    g_next_session_id = 1;
    g_loaded = 0;

    if (!fat32_is_mounted()) {
        serial_write("userdb: fs not mounted, in-memory only\n");
        userdb_add_user("root", "root", 0, 0);
        return;
    }

    if (userdb_load() > 0) {
        g_loaded = 1;
        g_current_uid = 0;
        auth_set_current_uid(0);
        serial_write("userdb: loaded from disk\n");
        return;
    }

    serial_write("userdb: no database, creating default root\n");
    userdb_add_user("root", "root", 0, 0);
    if (userdb_save() == 0) g_loaded = 1;
}

int userdb_load(void) {
    if (!fat32_is_mounted()) return -1;

    char* pbuf = (char*)kmalloc(4096);
    char* sbuf = (char*)kmalloc(4096);
    if (!pbuf || !sbuf) {
        if (pbuf) kfree(pbuf);
        if (sbuf) kfree(sbuf);
        return -1;
    }

    int loaded = 0;
    int pn = read_file(USERDB_PASSWD_PATH, pbuf, 4096);
    if (pn > 0) {
        loaded += parse_lines(pbuf, parse_passwd_line);
    }
    int sn = read_file(USERDB_SHADOW_PATH, sbuf, 4096);
    if (sn > 0) {
        parse_lines(sbuf, parse_shadow_line);
    }

    kfree(pbuf);
    kfree(sbuf);
    return loaded;
}

static int write_file(const char* path, const char* buf, int len) {
    ensure_etc_dir();
    fat32_remove(path);
    int fd = fat32_open(path, 1);
    if (fd < 0) return -1;
    int w = fat32_write(fd, buf, len);
    fat32_close(fd);
    return (w == len) ? 0 : -1;
}

int userdb_save(void) {
    if (!fat32_is_mounted()) return -1;

    char* pbuf = (char*)kmalloc(8192);
    char* sbuf = (char*)kmalloc(8192);
    if (!pbuf || !sbuf) {
        if (pbuf) kfree(pbuf);
        if (sbuf) kfree(sbuf);
        return -1;
    }

    int pp = 0, sp = 0;
    for (int i = 0; i < USERDB_MAX_USERS; i++) {
        if (!g_users[i].in_use) continue;

        char uid_s[12], gid_s[12];
        uint_to_str(g_users[i].uid, uid_s);
        uint_to_str(g_users[i].gid, gid_s);

        const char* f[] = {
            g_users[i].username, "x", uid_s, gid_s,
            g_users[i].home_dir, g_users[i].shell
        };
        for (int k = 0; k < 6; k++) {
            const char* s = f[k];
            while (*s && pp < 8190) pbuf[pp++] = *s++;
            if (k < 5 && pp < 8191) pbuf[pp++] = ':';
        }
        if (pp < 8191) pbuf[pp++] = '\n';

        const char* sf[] = {
            g_users[i].username, g_users[i].password_hash, g_users[i].salt
        };
        for (int k = 0; k < 3; k++) {
            const char* s = sf[k];
            while (*s && sp < 8190) sbuf[sp++] = *s++;
            if (k < 2 && sp < 8191) sbuf[sp++] = ':';
        }
        if (sp < 8191) sbuf[sp++] = '\n';
    }

    int rp = write_file(USERDB_PASSWD_PATH, pbuf, pp);
    int rs = write_file(USERDB_SHADOW_PATH, sbuf, sp);

    kfree(pbuf);
    kfree(sbuf);

    if (rp < 0 || rs < 0) {
        serial_write("userdb: save failed\n");
        return -1;
    }
    serial_write("userdb: saved\n");
    return 0;
}

int userdb_add_user(const char* name, const char* password, uint32_t uid, uint32_t gid) {
    if (!name || !*name) return -1;
    if (find_by_name(name) >= 0) {
        printk("userdb: user '%s' already exists\n", name);
        return -1;
    }
    if (uid != 0 && find_by_uid(uid) >= 0) {
        printk("userdb: uid %d already in use\n", uid);
        return -1;
    }
    int slot = find_slot();
    if (slot < 0) {
        printk("userdb: user table full\n");
        return -1;
    }

    user_record_t* u = &g_users[slot];
    kmemset(u, 0, sizeof(*u));
    kstrcpy(u->username, name, USERDB_USERNAME_LEN);
    u->uid = uid;
    u->gid = gid;

    char home[USERDB_HOME_LEN];
    int hp = 0;
    const char* pre = "/home/";
    while (*pre && hp < USERDB_HOME_LEN - 1) home[hp++] = *pre++;
    const char* np = name;
    while (*np && hp < USERDB_HOME_LEN - 1) home[hp++] = *np++;
    home[hp] = 0;
    kstrcpy(u->home_dir, home, USERDB_HOME_LEN);

    kstrcpy(u->shell, "/bin/shell", USERDB_SHELL_LEN);

    gen_salt(u->salt);
    u->salt[USERDB_SALT_LEN - 1] = 0;
    compute_hash(u->salt, password ? password : "", u->password_hash);
    u->password_hash[USERDB_HASH_LEN - 1] = 0;
    u->in_use = 1;

    printk("userdb: added user '%s' uid=%d gid=%d\n", name, uid, gid);
    if (g_loaded) userdb_save();
    return 0;
}

int userdb_del_user(const char* name) {
    if (!name) return -1;
    int idx = find_by_name(name);
    if (idx < 0) {
        printk("userdb: user '%s' not found\n", name);
        return -1;
    }
    if (g_users[idx].uid == 0) {
        printk("userdb: cannot delete root\n");
        return -1;
    }
    g_users[idx].in_use = 0;
    kmemset(g_users[idx].username, 0, USERDB_USERNAME_LEN);
    printk("userdb: deleted user '%s'\n", name);
    if (g_loaded) userdb_save();
    return 0;
}

int userdb_find(const char* name, user_record_t* out) {
    if (!name || !out) return -1;
    int idx = find_by_name(name);
    if (idx < 0) return -1;
    kmemcpy(out, &g_users[idx], sizeof(user_record_t));
    return 0;
}

int userdb_verify(const char* name, const char* password) {
    if (!name || !password) return -1;
    int idx = find_by_name(name);
    if (idx < 0) return -1;
    char hash[USERDB_HASH_LEN];
    compute_hash(g_users[idx].salt, password, hash);
    return (kstrcmp(hash, g_users[idx].password_hash) == 0) ? 0 : -1;
}

int userdb_su(const char* name, const char* password) {
    if (!name) return -1;
    int idx = find_by_name(name);
    if (idx < 0) {
        printk("userdb: su: user '%s' not found\n", name);
        return -1;
    }
    if (g_current_uid != 0) {
        if (userdb_verify(name, password) != 0) {
            printk("userdb: su: authentication failed\n");
            return -1;
        }
    }
    g_current_uid = g_users[idx].uid;
    auth_set_current_uid((int)g_users[idx].uid);
    printk("userdb: su to '%s' (uid=%d)\n", name, g_users[idx].uid);
    return 0;
}

uint32_t userdb_get_uid(void) {
    return g_current_uid;
}

int userdb_set_uid(uint32_t uid) {
    if (g_current_uid != 0 && uid != g_current_uid) {
        if (find_by_uid(uid) < 0) {
            printk("userdb: set_uid denied, uid %d not found\n", uid);
            return -1;
        }
    }
    g_current_uid = uid;
    auth_set_current_uid((int)uid);
    return 0;
}

uint32_t userdb_alloc_uid(void) {
    for (uint32_t candidate = 1; candidate < 0xFFFF; candidate++) {
        if (find_by_uid(candidate) < 0) return candidate;
    }
    return 0;
}

const char* userdb_get_username(void) {
    int idx = find_by_uid(g_current_uid);
    if (idx >= 0) return g_users[idx].username;
    return "unknown";
}

int userdb_session_create(uint32_t uid, const char* username) {
    for (int i = 0; i < USERDB_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) {
            g_sessions[i].session_id = g_next_session_id++;
            g_sessions[i].uid = uid;
            kstrcpy(g_sessions[i].username, username ? username : "unknown",
                    USERDB_USERNAME_LEN);
            g_sessions[i].active = 1;
            return i;
        }
    }
    return -1;
}

int userdb_session_destroy(int sid) {
    if (sid < 0 || sid >= USERDB_MAX_SESSIONS) return -1;
    if (!g_sessions[sid].active) return -1;
    g_sessions[sid].active = 0;
    return 0;
}

user_session_t* userdb_session_get(int sid) {
    if (sid < 0 || sid >= USERDB_MAX_SESSIONS) return 0;
    if (!g_sessions[sid].active) return 0;
    return &g_sessions[sid];
}
