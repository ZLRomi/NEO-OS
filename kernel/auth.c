#include "auth.h"
#include "sha256.h"
#include "printk.h"
#include <stdint.h>

static user_t users[MAX_USERS];
static int current_uid = 0;
static int next_uid_counter = 0;

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static int memeq(const uint8_t* a, const uint8_t* b, int n) {
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int auth_init(void) {
    int i;
    for (i = 0; i < MAX_USERS; i++) users[i].in_use = 0;
    current_uid = 0;
    next_uid_counter = 0;
    int uid = auth_add_user("root", "root", 1);
    printk("auth: initialized, root user created (uid=%d)\n", uid);
    return 0;
}

int auth_add_user(const char* username, const char* password, int is_admin) {
    int slot = -1;
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (!users[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        printk("auth: user table full\n");
        return -1;
    }
    str_copy(users[slot].username, username, USERNAME_LEN);
    sha256((const uint8_t*)password, (size_t)str_len(password), users[slot].password_hash);
    users[slot].uid = next_uid_counter++;
    users[slot].is_admin = is_admin;
    users[slot].in_use = 1;
    return users[slot].uid;
}

int auth_login(const char* username, const char* password) {
    int i;
    uint8_t hash[PASSWORD_HASH_LEN];
    for (i = 0; i < MAX_USERS; i++) {
        if (users[i].in_use && str_eq(users[i].username, username)) {
            sha256((const uint8_t*)password, (size_t)str_len(password), hash);
            if (memeq(hash, users[i].password_hash, PASSWORD_HASH_LEN)) {
                current_uid = users[i].uid;
                return users[i].uid;
            }
            return -1;
        }
    }
    return -1;
}

int auth_get_current_uid(void) {
    return current_uid;
}

void auth_set_current_uid(int uid) {
    current_uid = uid;
}

const char* auth_get_current_username(void) {
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (users[i].in_use && users[i].uid == current_uid) return users[i].username;
    }
    return "unknown";
}

user_t* auth_get_users(int* count) {
    int n = 0;
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (users[i].in_use) n++;
    }
    if (count) *count = n;
    return users;
}
