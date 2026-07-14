#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>

#define MAX_USERS 16
#define USERNAME_LEN 32
#define PASSWORD_HASH_LEN 32  // SHA256 = 32 字节

typedef struct {
    char username[USERNAME_LEN];
    uint8_t password_hash[PASSWORD_HASH_LEN];
    int uid;
    int is_admin;  // 1=root/admin
    int in_use;
} user_t;

int auth_init(void);
int auth_add_user(const char* username, const char* password, int is_admin);
int auth_login(const char* username, const char* password);
int auth_get_current_uid(void);
void auth_set_current_uid(int uid);
const char* auth_get_current_username(void);
user_t* auth_get_users(int* count);

#endif
