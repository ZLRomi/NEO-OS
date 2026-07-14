#ifndef USERDB_H
#define USERDB_H

#include <stdint.h>

#define USERDB_MAX_USERS    16
#define USERDB_USERNAME_LEN 32
#define USERDB_HASH_LEN     65
#define USERDB_SALT_LEN     17
#define USERDB_HOME_LEN     64
#define USERDB_SHELL_LEN     32
#define USERDB_MAX_SESSIONS 4

#define USERDB_PASSWD_PATH "/etc/passwd"
#define USERDB_SHADOW_PATH "/etc/shadow"

typedef struct {
    char username[USERDB_USERNAME_LEN];
    uint32_t uid;
    uint32_t gid;
    char home_dir[USERDB_HOME_LEN];
    char shell[USERDB_SHELL_LEN];
    char password_hash[USERDB_HASH_LEN];
    char salt[USERDB_SALT_LEN];
    int in_use;
} user_record_t;

typedef struct {
    uint32_t session_id;
    uint32_t uid;
    char username[USERDB_USERNAME_LEN];
    int active;
} user_session_t;

void userdb_init(void);
int userdb_load(void);
int userdb_save(void);
int userdb_add_user(const char* name, const char* password, uint32_t uid, uint32_t gid);
int userdb_del_user(const char* name);
int userdb_find(const char* name, user_record_t* out);
int userdb_verify(const char* name, const char* password);
int userdb_su(const char* name, const char* password);
uint32_t userdb_get_uid(void);
int userdb_set_uid(uint32_t uid);
uint32_t userdb_alloc_uid(void);
const char* userdb_get_username(void);

int userdb_session_create(uint32_t uid, const char* username);
int userdb_session_destroy(int sid);
user_session_t* userdb_session_get(int sid);

#endif
