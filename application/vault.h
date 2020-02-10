#include <stdint.h>

#define VE_SUCCESS 0
#define VE_MEMERR 1
#define VE_PARAMERR 2
#define VE_IOERR 3
#define VE_CRYPTOERR 4
#define VE_VOPEN 5
#define VE_VCLOSE 6
#define VE_SYSCALL 7
#define VE_EXIST 8
#define VE_ACCESS 9
#define VE_KEYEXIST 10
#define VE_FILE 11
#define VE_NOSPACE 12

struct vault_info;

struct vault_info* init_vault();

int release_vault(struct vault_info* info);

int create_vault(char* directory, char* username, char* password, struct vault_info* info);

int open_vault(char* dreictory, char* username, char* password, struct vault_info* info);

int close_vault(struct vault_info* info);

int add_key(struct vault_info* info, uint8_t type, const char* key, const char* vaule);

char** get_vault_keys(struct vault_info* info);

uint32_t num_vault_keys(struct vault_info* info);

uint64_t last_modified_time(struct vault_info* info, const char* key);

int open_key(struct vault_info* info, const char* key);

int delete_key(struct vault_info* info, const char* key);

int update_key(struct vault_info* info, uint8_t type, const char* key, const char* vaule);
