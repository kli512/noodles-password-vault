#include "vault.h"
#include "vault_map.h"

// C libraries
#include <errno.h>
#include <fcntl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

/**
   vault.c - Main implementation of the vault

   The vault is able to load in a file for a particular vault and process it.
   The vault file should have the following format:

   VERSION | SALT | ENCRYPTED_MASTER | SERVER_TIME | LOC_DAT | PAIRS | HASH
      8       16        32+24+16            8                           32

   The version is the version number of the vault file, in case of changes.
   The password salt is used for deriving the encryption key for the master.
   The encrypted master key is loaded and decrypted using the password.

   Afterwards, the location data field exists to specify where in the file.
   The location data field contains the length of the data, followed by
   4 4-byte chunks that specify the start of key-value pair, the key length,
   and the value length, and a state section for the state of the loc data.
   The state field is to allow for quick deletions, which will wipe the
   encrypted values but not move around the rest of the data in the file. The
   number of deleted entries can then be banked up until there is a sufficient
   amount to condense many at once. In addition, appending entries is quick,
   either addition or updating, keeping the amount of internal fragmentation on
   the smaller side.

   LENGTH | STATE1 | LOC1 | KEY_LEN1 | VAL_LEN1 | STATE2 | LOC2 | KEY_LEN2 | ...
      4       4       4        4          4         4       4        4

   Finally in the file comes the key value pairs, the actual data being stored.
   If the entry is deleted, the E_VAL field should be wiped with zeros.

   MTIME | TYPE | KEY | E_VAL | VAL_MAC | VAL_NONCE | HASH
     8      1     KLEN   VLEN     16         24        32

   Finally at the end of the file is a hash of the entire file, keyed with the
   master key to prevent tampering.
 */

/**
   vault_box - struct to hold an entry within the vault
 */
struct vault_box {
  char key[BOX_KEY_SIZE];
  char type;
  uint32_t val_len;
  char value[DATA_SIZE];
};

/**
   vault_info - struct to hold info of the currently open vault

   The current box is the box which is currently opened and contains
   an unencrypted password/information.

   Contains the derived key to generate the server password, the decrypted
   master for creating and checking hashes as well as decrypting and
   encrypting values, and the hash state.

   Finally also contains a hash map of the keys to their loc data in the file,
   the current file descriptor, and a status for if the vault is open.
 */
struct vault_info {
  int is_open;
  int user_fd;
  uint8_t derived_key[MASTER_KEY_SIZE];
  uint8_t decrypted_master[MASTER_KEY_SIZE];
  crypto_generichash_state hash_state;
  struct vault_box current_box;
  struct vault_map* key_info;
};

const char* filename_pattern = "%s/%s.vault";

/**
   Macros defs to wrap C lib calls.

   These exist as shorthand to be used in code further on that automatically
   check the return code of a call, and if it is not a success locks the vault
   and returns with an IO error. The do...while(0) exists to force a semicolon
   to be written at the end of the line. Using these helps with code clarity.
 */
#define WRITE(fd, addr, len, info)     \
  do {                                 \
    if (write(fd, addr, len) < 0) {    \
      fputs("Write failed\n", stderr); \
      sodium_mprotect_noaccess(info);  \
      return VE_IOERR;                 \
    }                                  \
  } while (0)
#define READ(fd, addr, len, info)     \
  do {                                \
    if (read(fd, addr, len) < 0) {    \
      fputs("Read failed\n", stderr); \
      sodium_mprotect_noaccess(info); \
      return VE_IOERR;                \
    }                                 \
  } while (0)
#define PW_HASH(result, input, inputlen, salt)                                \
  crypto_pwhash((uint8_t*)result, MASTER_KEY_SIZE, (uint8_t*)input, inputlen, \
                salt, crypto_pwhash_OPSLIMIT_MODERATE,                        \
                crypto_pwhash_MEMLIMIT_MODERATE, crypto_pwhash_ALG_ARGON2ID13)

// If debug mode set, print strings to stderr
#ifdef VAULT_DEBUG
#define FPUTS(string, output) \
  do {                        \
    fputs(string, output);    \
  } while (0)
#else
#define FPUTS(string, output)
#endif

// States that a vault entry can be in
#define STATE_UNUSED 0
#define STATE_ACTIVE ((1 << 16) | 1)
#define STATE_DELETED 1

int max_value_size() { return DATA_SIZE; }

/**
   Internal function definitions

   The following functions all have the internal_* prefix and are used as helper
   functions within the file. The main reason these functions exist is to
   prevent code copying when there is functionality shared between multiple
   different functions.

   Most importantly, these files assume that the check for whether a vault is
   opened and that the memory for the information has been made readable and
   writable, and most do not change these upon return.
 */

/**
   function internal_hash_file

   Hashes the file_size-HASH_SIZE bytes of the file. As the hash is keyed with
   the master key, the hash ensures the integrity of the vault file at rest.
   The file hash is placed in the hash parameter, expected to be HASH_SIZE
   bytes in size. The offset passed in is the number of bytes at the end of
   the file to not add into the hash, which can be useful to hash all of the
   file with the exception of the appended hash.

   Returns VE_SUCCESS if the hash was successful.
   VE_CRYPTOERR if any part of the hashing itself fails
   VE_IOERROR if reading from the file fails
 */

int internal_hash_file(struct vault_info* info, uint8_t* hash,
                       uint32_t off_end) {
  uint32_t file_size = lseek(info->user_fd, 0, SEEK_END);
  uint32_t bytes_to_hash = file_size - off_end;
  uint8_t buffer[1024];
  lseek(info->user_fd, 0, SEEK_SET);

  if (crypto_generichash_init(&info->hash_state, info->decrypted_master,
                              MASTER_KEY_SIZE, HASH_SIZE) < 0) {
    return VE_CRYPTOERR;
  }

  while (bytes_to_hash > 0) {
    uint32_t amount_at_once = bytes_to_hash > 1024 ? 1024 : bytes_to_hash;
    if (read(info->user_fd, &buffer, amount_at_once) < 0) {
      return VE_IOERR;
    }

    if (crypto_generichash_update(&info->hash_state,
                                  (const unsigned char*)&buffer,
                                  amount_at_once) < 0) {
      return VE_CRYPTOERR;
    }

    bytes_to_hash -= amount_at_once;
  }

  if (crypto_generichash_final(&info->hash_state, hash, HASH_SIZE) < 0) {
    return VE_CRYPTOERR;
  }

  return VE_SUCCESS;
}

/**
   function get_current_time

   Returns the number of milliseconds since the epoch in 8 bytes.
   Function is used to timestamp vault entries, which can be used for
   comparing against the server timestamp returned on updates.
*/
uint64_t get_current_time() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint64_t millisecondsSinceEpoch =
      (uint64_t)(tv.tv_sec) * 1000 + (uint64_t)(tv.tv_usec) / 1000;

  return millisecondsSinceEpoch;
}

/**
   function internal_append_key

   Attempts to append a key-value pair to the end of the vault file and
   place relevant location data for the entry. As the vault is append-only,
   the first free loc data field can be found and used to represent the
   data that is appended to the file.

   In the case that there are no free location data fields, the function
   returns without appending the key. The internal_condense_file function
   can then be used to remove any deleted entries from the file and double
   the location data size, after which this function can be called again.

   As this is an internal function, assumes the parameters have already been
   checked by the caller and that the info field has been made writable.

   Returns VE_SUCCESS if the key-value pair was appeneded
   VE_CRYPTOERR if the value cannot be encrypted
   VE_IOERR if the data cannot be written to the file
   VE_NOSPACE if there is no more space in the loc data field
 */
int internal_append_key(struct vault_info* info, uint8_t type, const char* key,
                        const char* value) {
  lseek(info->user_fd, HEADER_SIZE - 4, SEEK_SET);
  uint32_t loc_len;
  READ(info->user_fd, &loc_len, 4, info);
  uint32_t loc_data[LOC_SIZE / sizeof(uint32_t)];

  for (uint32_t next_loc = 0; next_loc < loc_len; ++next_loc) {
    READ(info->user_fd, &loc_data, LOC_SIZE, info);
    if (loc_data[0]) {
      continue;
    }

    uint32_t file_loc = lseek(info->user_fd, -1 * HASH_SIZE, SEEK_END);
    uint32_t key_len = strlen(key);
    uint32_t val_len = strlen(value);
    uint32_t inode_loc = HEADER_SIZE + next_loc * LOC_SIZE;

    uint64_t m_time = get_current_time();

    int input_len = ENTRY_HEADER_SIZE + key_len + val_len + MAC_SIZE +
                    NONCE_SIZE + HASH_SIZE;
    uint8_t* to_write_data = malloc(input_len);
    *((uint64_t*)to_write_data) = m_time;
    to_write_data[ENTRY_HEADER_SIZE - 1] = type;
    strncpy((char*)to_write_data + ENTRY_HEADER_SIZE, key, key_len);

    uint8_t* val_nonce = to_write_data + input_len - NONCE_SIZE - HASH_SIZE;
    randombytes_buf(val_nonce, NONCE_SIZE);

    if (crypto_secretbox_easy(to_write_data + ENTRY_HEADER_SIZE + key_len,
                              (uint8_t*)value, val_len, val_nonce,
                              (uint8_t*)&info->decrypted_master) < 0) {
      FPUTS("Could not encrypt value for key value pair\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_CRYPTOERR;
    }

    if (crypto_generichash(to_write_data + input_len - HASH_SIZE, HASH_SIZE,
                           to_write_data, input_len - HASH_SIZE,
                           info->decrypted_master, MASTER_KEY_SIZE) < 0) {
      FPUTS("Could not generate entry hash\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_CRYPTOERR;
    }

    if (write(info->user_fd, to_write_data, input_len) < 0) {
      FPUTS("Could not write key-value pair to disk\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_IOERR;
    }

    loc_data[0] = STATE_ACTIVE;
    loc_data[1] = file_loc;
    loc_data[2] = key_len;
    loc_data[3] = val_len;
    lseek(info->user_fd, inode_loc, SEEK_SET);

    if (write(info->user_fd, loc_data, LOC_SIZE) < 0) {
      FPUTS("Could not write inode pair to disk\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_IOERR;
    }

    uint8_t file_hash[HASH_SIZE];
    internal_hash_file(info, (uint8_t*)&file_hash, 0);
    lseek(info->user_fd, 0, SEEK_END);
    if (write(info->user_fd, (uint8_t*)&file_hash, HASH_SIZE) < 0) {
      FPUTS("Could not write hash to disk\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_IOERR;
    }

    struct key_info* current_info = malloc(sizeof(struct key_info));
    current_info->inode_loc = inode_loc;
    current_info->m_time = m_time;
    current_info->type = type;

    add_entry(info->key_info, key, current_info);
    free(to_write_data);
    sodium_mprotect_noaccess(info);

    FPUTS("Added key\n", stderr);
    return VE_SUCCESS;
  }

  return VE_NOSPACE;
}

/**
   function internal_append_encrypted

   Like internal_append_key, this function is used to append an entry to the
   vault if there is space available in the loc data. The main difference is
   this function takes an entire entry that has the encrypted value and hash
   at the end and adds it to the vault. The hash at the end if verified to be
   correct, and the entry is only appended if it is. This function allows the
   entries to be sent to and from the server without the server being able to
   decrypt the values, but preventing tampering using the keyed hash.

   The actual checking of the hash is in the function add_encrypted_value,
   which may call this function more than once if there is no space.

   Returns VE_SUCCESS if the entry was validated and added.
   VE_IOERR if there were issues writing to or reading from disk
   VE_CRYPTORR if the new entry hash could not be generated
   VE_NOSPACE if none of the loc entries are open

 */
int internal_append_encrypted(struct vault_info* info, uint8_t type,
                              const char* key, const char* entry, int len) {
  lseek(info->user_fd, HEADER_SIZE - 4, SEEK_SET);
  uint32_t loc_len;
  READ(info->user_fd, &loc_len, 4, info);
  uint32_t loc_data[LOC_SIZE / sizeof(uint32_t)];

  for (uint32_t next_loc = 0; next_loc < loc_len; ++next_loc) {
    READ(info->user_fd, &loc_data, LOC_SIZE, info);
    if (loc_data[0]) {
      continue;
    }

    uint32_t file_loc = lseek(info->user_fd, -1 * HASH_SIZE, SEEK_END);
    uint32_t key_len = strlen(key);
    uint32_t val_len =
        len - ENTRY_HEADER_SIZE - MAC_SIZE - NONCE_SIZE - HASH_SIZE - key_len;
    uint32_t inode_loc = HEADER_SIZE + next_loc * LOC_SIZE;

    uint64_t m_time = get_current_time();

    uint8_t* to_write_data = malloc(len);
    memcpy(to_write_data, entry, len);
    *((uint64_t*)to_write_data) = m_time;

    if (crypto_generichash(to_write_data + len - HASH_SIZE, HASH_SIZE,
                           to_write_data, len - HASH_SIZE,
                           info->decrypted_master, MASTER_KEY_SIZE) < 0) {
      FPUTS("Could not generate entry hash\n", stderr);
      free(to_write_data);
      return VE_CRYPTOERR;
    }

    if (write(info->user_fd, to_write_data, len) < 0) {
      FPUTS("Could not write key-value pair to disk\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_IOERR;
    }

    loc_data[0] = STATE_ACTIVE;
    loc_data[1] = file_loc;
    loc_data[2] = key_len;
    loc_data[3] = val_len;
    lseek(info->user_fd, inode_loc, SEEK_SET);

    if (write(info->user_fd, loc_data, LOC_SIZE) < 0) {
      FPUTS("Could not write inode pair to disk\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_IOERR;
    }

    uint8_t file_hash[HASH_SIZE];
    internal_hash_file(info, (uint8_t*)&file_hash, 0);
    lseek(info->user_fd, 0, SEEK_END);
    if (write(info->user_fd, (uint8_t*)&file_hash, HASH_SIZE) < 0) {
      FPUTS("Could not write hash to disk\n", stderr);
      free(to_write_data);
      sodium_mprotect_noaccess(info);
      return VE_IOERR;
    }

    struct key_info* current_info = malloc(sizeof(struct key_info));
    current_info->inode_loc = inode_loc;
    current_info->m_time = m_time;
    current_info->type = type;

    add_entry(info->key_info, key, current_info);
    free(to_write_data);
    sodium_mprotect_noaccess(info);

    FPUTS("Added key\n", stderr);
    return VE_SUCCESS;
  }

  return VE_NOSPACE;
}

/**
   function internal_create_key_map

   Given an open vault, construct the mapping of keys to their loc datas in the
   file and assign it to the key_info field. The map is implemented as a hash
   table with half the number of buckets as the length of the loc field. As this
   is an internal function, it assumes that info is able to be read, as well as
   the vault is opened and that key_info is not currently set to a map.

   Returns VE_SUCCESS if able to create the map
   VE_IOERR if there were issues reading from disk
 */
int internal_create_key_map(struct vault_info* info) {
  lseek(info->user_fd, HEADER_SIZE - 4, SEEK_SET);
  uint32_t loc_len;
  READ(info->user_fd, &loc_len, 4, info);
  uint32_t loc_data[LOC_SIZE / sizeof(uint32_t)];
  info->key_info = init_map(loc_len / 2);
  for (uint32_t next_loc = 0; next_loc < loc_len; ++next_loc) {
    READ(info->user_fd, &loc_data, LOC_SIZE, info);
    uint32_t is_active = STATE_ACTIVE == loc_data[0];
    if (!is_active) {
      continue;
    }

    uint32_t file_loc = loc_data[1];
    uint32_t key_len = loc_data[2];
    uint32_t inode_loc = HEADER_SIZE + next_loc * LOC_SIZE;
    char key[key_len + 1];
    struct key_info* current_info = malloc(sizeof(struct key_info));
    current_info->inode_loc = inode_loc;

    lseek(info->user_fd, file_loc, SEEK_SET);
    READ(info->user_fd, &(current_info->m_time), sizeof(uint64_t), info);
    READ(info->user_fd, &(current_info->type), sizeof(uint8_t), info);
    READ(info->user_fd, &key, key_len, info);
    key[key_len] = 0;

    add_entry(info->key_info, key, current_info);
    lseek(info->user_fd, HEADER_SIZE + (next_loc + 1) * LOC_SIZE, SEEK_SET);
  }
  return VE_SUCCESS;
}

/**
   function condense_file

   Given an open vault, remove all deleted entries, and double the size of the
   loc field while realigning the active entries to be closest to the front of
   the field. This function is used to clean up deletes and to increase the
   amount of entries that can be stored. By using this slow function rarely,
   most changes to the file are relatively fast, and the file size is still
   kept relatively small. In the case that there are many changes made, there
   is still more room made for the updates.

   Returns VE_SUCCESS upon increasing the file size and moving entries
   VE_VCLOSE if no vault is open
   VE_MEMERR if memory cannot be opened
   VE_IOERR if there are issues reading from or writing to memory
*/
int internal_condense_file(struct vault_info* info) {
  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    return VE_MEMERR;
  }

  if (!info->is_open) {
    FPUTS("Vault closed\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_VCLOSE;
  }

  uint8_t header[HEADER_SIZE];
  uint32_t loc_size;
  uint32_t* loc_data;
  uint8_t* box_data;
  uint32_t box_len;
  uint32_t current_file_size = lseek(info->user_fd, -1 * HASH_SIZE, SEEK_END);
  uint32_t data_replacement_loc = 0;
  uint32_t loc_replacement_index = 0;

  lseek(info->user_fd, 0, SEEK_SET);
  READ(info->user_fd, &header, HEADER_SIZE, info);
  loc_size = *((uint32_t*)(header + HEADER_SIZE - 4));

  uint32_t old_data_offset = (loc_size * LOC_SIZE) + HEADER_SIZE;
  uint32_t new_data_offset = (loc_size * LOC_SIZE) + old_data_offset;

  loc_data = malloc(loc_size * LOC_SIZE);
  if (read(info->user_fd, loc_data, loc_size * LOC_SIZE) < 0) {
    FPUTS("Could not read loc from disk\n", stderr);
    free(loc_data);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  box_len = current_file_size - old_data_offset;
  box_data = malloc(box_len);
  if (read(info->user_fd, box_data, box_len) < 0) {
    FPUTS("Could not read loc from disk\n", stderr);
    free(loc_data);
    free(box_data);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  for (uint32_t i = 0; i < loc_size; ++i) {
    uint32_t* current_loc_data = loc_data + i * 4;
    uint32_t current_box_len = current_loc_data[2] + current_loc_data[3] +
                               ENTRY_HEADER_SIZE + MAC_SIZE + NONCE_SIZE +
                               HASH_SIZE;
    uint32_t current_loc = current_loc_data[1] - old_data_offset;
    if (current_loc_data[0] == STATE_ACTIVE) {
      if (loc_replacement_index == i) {
        loc_replacement_index++;
        continue;
      }

      memmove(box_data + data_replacement_loc, box_data + current_loc,
              current_box_len);
      current_loc_data[1] = new_data_offset + data_replacement_loc;
      data_replacement_loc += current_box_len;

      uint32_t* new_loc_placement = loc_data + loc_replacement_index * 4;
      memmove(new_loc_placement, current_loc_data, LOC_SIZE);
      loc_replacement_index++;

      continue;
    } else if (current_loc_data[0] == STATE_UNUSED) {
      break;
    } else {
      if (data_replacement_loc == 0) data_replacement_loc = current_loc;
    }
  }

  uint32_t new_data_size = data_replacement_loc;
  uint32_t valid_loc_entries = loc_replacement_index;
  uint32_t new_file_size = new_data_offset + new_data_size;
  uint32_t new_loc_size = loc_size * 2;

  lseek(info->user_fd, new_data_offset, SEEK_SET);
  WRITE(info->user_fd, box_data, new_data_size, info);
  lseek(info->user_fd, HEADER_SIZE - 4, SEEK_SET);
  WRITE(info->user_fd, &new_loc_size, 4, info);
  WRITE(info->user_fd, loc_data, valid_loc_entries * LOC_SIZE, info);

  uint32_t num_zeros = (loc_size * 2 - valid_loc_entries) * LOC_SIZE;
  uint8_t* zeros = malloc(num_zeros);
  sodium_memzero(zeros, num_zeros);
  WRITE(info->user_fd, zeros, num_zeros, info);
  ftruncate(info->user_fd, new_file_size);

  uint8_t file_hash[HASH_SIZE];
  internal_hash_file(info, (uint8_t*)&file_hash, 0);
  lseek(info->user_fd, 0, SEEK_END);
  if (write(info->user_fd, &file_hash, HASH_SIZE) < 0) {
    FPUTS("Could not write hash to disk\n", stderr);
    free(loc_data);
    free(box_data);
    free(zeros);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  delete_map(info->key_info);
  internal_create_key_map(info);

  free(loc_data);
  free(box_data);
  free(zeros);
  sodium_mprotect_noaccess(info);
  FPUTS("Condensed file and increased loc size\n", stderr);
  return VE_SUCCESS;
}

/**
   function internal_initial_checks
 */
int internal_initial_checks(struct vault_info* info) {
  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    return VE_MEMERR;
  }

  if (!info->is_open) {
    FPUTS("No vault opened\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_VCLOSE;
  }

  return VE_SUCCESS;
}

/**
   Vault initialization functions

   The following functions are used to handle opening, closing, and creating
   the vault files, as well as initializing and releasing the internal
   information used to represent the currently opened vault.
 */

/**
   function init_vault

   The function to be called at startup of the application, this ensures
   that the vault is setup to safely load in files and run crypto operations
   safely later on.

   Core dumps are disabled to ensure that passwords in memory are not allowed
   to be part of any core dumps later on. In addition, after libsodium is
   initialized, the memory for the vault information that will be used is
   placed in secure memory and locked to prevent access. While there are
   places in code that gives the passwords to the application, all sensitive
   data handled by the library will be within this secure memory. In addition,
   as the decrypted master key is generated by the library and only ever
   kept in the memory, it should never exist in a decrypted form on hard disk.

   If there is an error, a null pointer is returned.

   Returns a pointer to the memory the vault info is being kept in.
 */
struct vault_info* init_vault() {
  if (setrlimit(RLIMIT_CORE, 0) < 0) {
    FPUTS("Could not decrease core limit", stderr);
    return NULL;
  }

  if (sodium_init() < 0) {
    FPUTS("Could not init libsodium\n", stderr);
    return NULL;
  }

  struct vault_info* info = sodium_malloc(sizeof(struct vault_info));
  if (sodium_mlock(info, sizeof(struct vault_info))) {
    FPUTS("Issues locking memory\n", stderr);
    sodium_free(info);
    return NULL;
  }

  info->is_open = 0;
  if (sodium_mprotect_noaccess(info) < 0) {
    FPUTS("Issues preventing access to memory\n", stderr);
    return NULL;
  }

  return info;
}

/**
   function release_vault

   The function to be called at the end of using the vault, this releases
   all the memory allocated for the vault information. As sodium_free will
   handle zeroing the memory allocated for the vault_info structure, it does
   not have to be manually done. However ensuring that all allocated memory
   is freed is important, and done mainly in the key map.

   Returns VE_SUCCESS upon releasing all relevant memory
 */
int release_vault(struct vault_info* info) {
  sodium_mprotect_readwrite(info);
  if (info->is_open) {
    close(info->user_fd);
    delete_map(info->key_info);
  }
  sodium_munlock(info, sizeof(struct vault_info));
  sodium_free(info);
  return VE_SUCCESS;
}

/**
   function create_vault

   The function to be called that will initially set up a vault for a user.
   Requires that a vault file for the given user in the given directory does
   not already exist, and creates a USERNAME.vault file to represent the
   vault on disk.

   The password is derived using the Argon2id 1.3 password derivation algorithm
   that is computationally and memory expensive to attempt to disuade brute
   force attacks. Besides the password which is chosen by the user themselves,
   the master nonce and password salt are chosen using libsodiums randombytes
   function, and the key with crypto_secretbox_keygen.

   As stated above, the file format consists of a header with file information,
   a loc data field with locations of key value pairs in the file, and then the
   key-value pairs. This is created in this file, and maintained throughout
   the different functions. A file hash is appended to the end to prevent
   tampering.

   Returns VE_SUCCESS upon successful creation of a file.
   VE_PARAMERR if any of the inputs are null or their string lengths too long
   VE_MEMERR if secure memory cannot be changed to read write mode
   VE_SYSCALL if snprintf or open fails for a reason besides EEXIST and EACCES
   VE_VOPEN if a vault is already open
   VE_CRYPTOERR if the derived key or encrypted master cannot be generated
   VE_IOERR if their were any issues writing to disk
 */
int create_vault(char* directory, char* username, char* password,
                 struct vault_info* info) {
  if (directory == NULL || username == NULL || password == NULL ||
      strlen(directory) > MAX_PATH_LEN || strlen(username) > MAX_USER_SIZE ||
      strlen(password) > MAX_PASS_SIZE) {
    return VE_PARAMERR;
  }

  int max_size = strlen(directory) + strlen(username) + 10;
  char* pathname = malloc(max_size);
  if (snprintf(pathname, max_size, filename_pattern, directory, username) < 0) {
    free(pathname);
    return VE_SYSCALL;
  }

  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    free(pathname);
    return VE_MEMERR;
  }

  if (info->is_open) {
    FPUTS("Already have a vault open\n", stderr);
    free(pathname);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_VOPEN;
  }

  // Specify that the file must be created, and have access set as 0600
  int open_results =
      open(pathname, O_RDWR | O_CREAT | O_EXCL | O_DSYNC, S_IRUSR | S_IWUSR);
  free(pathname);
  if (open_results < 0) {
    if (errno == EEXIST) {
      return VE_EXIST;
    } else if (errno == EACCES) {
      return VE_ACCESS;
    } else {
      return VE_SYSCALL;
    }
  }

  if (flock(open_results, LOCK_EX | LOCK_NB) < 0) {
    FPUTS("Could not get file lock\n", stderr);
    return VE_SYSCALL;
  }

  info->user_fd = open_results;
  crypto_secretbox_keygen(info->decrypted_master);

  uint8_t salt[SALT_SIZE];
  randombytes_buf(salt, sizeof salt);
  if (PW_HASH(info->derived_key, password, strlen(password), salt) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    close(open_results);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  uint8_t encrypted_master[MASTER_KEY_SIZE + MAC_SIZE];
  uint8_t master_nonce[NONCE_SIZE];
  randombytes_buf(master_nonce, sizeof master_nonce);
  if (crypto_secretbox_easy(encrypted_master, info->decrypted_master,
                            MASTER_KEY_SIZE, master_nonce,
                            info->derived_key) < 0) {
    FPUTS("Could not encrypt master key\n", stderr);
    close(open_results);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  uint32_t loc_len = INITIAL_SIZE;
  uint8_t zeros[INITIAL_SIZE * LOC_SIZE] = {0};
  uint8_t version = VERSION;
  WRITE(info->user_fd, &version, 1, info);
  WRITE(info->user_fd, &zeros, 7, info);
  WRITE(info->user_fd, &salt, crypto_pwhash_SALTBYTES, info);
  WRITE(info->user_fd, &encrypted_master, MASTER_KEY_SIZE + MAC_SIZE, info);
  WRITE(info->user_fd, &master_nonce, NONCE_SIZE, info);
  WRITE(info->user_fd, &zeros, 8, info);
  WRITE(info->user_fd, &loc_len, sizeof(uint32_t), info);
  WRITE(info->user_fd, &zeros, INITIAL_SIZE * LOC_SIZE, info);

  uint8_t file_hash[HASH_SIZE];
  internal_hash_file(info, (uint8_t*)&file_hash, 0);
  lseek(info->user_fd, 0, SEEK_END);
  if (write(info->user_fd, &file_hash, HASH_SIZE) < 0) {
    FPUTS("Could not write hash to disk\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  info->key_info = init_map(INITIAL_SIZE / 2);
  info->current_box.key[0] = 0;
  info->is_open = 1;

  if (sodium_mprotect_noaccess(info) < 0) {
    FPUTS("Issues preventing access to memory\n", stderr);
  }

  FPUTS("Created file successfully\n", stderr);
  return VE_SUCCESS;
}

/**
   function create_from_header

   Create a vault for a given user given the header of a vault file downloaded
   from the server and the password to unlock the header.
 */
int create_from_header(char* directory, char* username, char* password,
                       uint8_t* header, struct vault_info* info) {
  if (directory == NULL || username == NULL || password == NULL ||
      strlen(directory) > MAX_PATH_LEN || strlen(username) > MAX_USER_SIZE ||
      strlen(password) > MAX_PASS_SIZE) {
    return VE_PARAMERR;
  }

  int max_size = strlen(directory) + strlen(username) + 10;
  char* pathname = malloc(max_size);
  if (snprintf(pathname, max_size, filename_pattern, directory, username) < 0) {
    free(pathname);
    return VE_SYSCALL;
  }

  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    free(pathname);
    return VE_MEMERR;
  }

  if (info->is_open) {
    FPUTS("Already have a vault open\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    free(pathname);
    return VE_VOPEN;
  }

  if (PW_HASH(info->derived_key, password, strlen(password), header + 8) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    free(pathname);
    return VE_CRYPTOERR;
  }

  if (crypto_secretbox_open_easy(info->decrypted_master, header + SALT_SIZE + 8,
                                 MASTER_KEY_SIZE + MAC_SIZE,
                                 header + HEADER_SIZE - NONCE_SIZE - 12,
                                 info->derived_key) < 0) {
    FPUTS("Could not decrypt master key\n", stderr);
    sodium_memzero(info->derived_key, MASTER_KEY_SIZE);
    free(pathname);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_WRONGPASS;
  }

  // Specify that the file must be created, and have access set as 0600
  int open_results =
      open(pathname, O_RDWR | O_CREAT | O_EXCL | O_DSYNC, S_IRUSR | S_IWUSR);
  free(pathname);
  if (open_results < 0) {
    if (errno == EEXIST) {
      return VE_EXIST;
    } else if (errno == EACCES) {
      return VE_ACCESS;
    } else {
      return VE_SYSCALL;
    }
  }

  if (flock(open_results, LOCK_EX | LOCK_NB) < 0) {
    FPUTS("Could not get file lock\n", stderr);
    return VE_SYSCALL;
  }

  info->user_fd = open_results;

  uint32_t loc_len = INITIAL_SIZE;
  uint8_t zeros[INITIAL_SIZE * LOC_SIZE] = {0};
  WRITE(info->user_fd, header, HEADER_SIZE - 4, info);
  WRITE(info->user_fd, &loc_len, sizeof(uint32_t), info);
  WRITE(info->user_fd, &zeros, INITIAL_SIZE * LOC_SIZE, info);

  uint8_t file_hash[HASH_SIZE];
  internal_hash_file(info, (uint8_t*)&file_hash, 0);
  lseek(info->user_fd, 0, SEEK_END);
  if (write(info->user_fd, &file_hash, HASH_SIZE) < 0) {
    FPUTS("Could not write hash to disk\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  info->key_info = init_map(INITIAL_SIZE / 2);
  info->current_box.key[0] = 0;
  info->is_open = 1;

  if (sodium_mprotect_noaccess(info) < 0) {
    FPUTS("Issues preventing access to memory\n", stderr);
  }

  FPUTS("Created file successfully\n", stderr);
  return VE_SUCCESS;
}

/**
   function open_vault

   Given the directory containing vaults, a username and password, attempts to
   open the vault for a given user. The vault should be at the path
   directory/username.vault and have only been modified by the functions within
   this file. The decryption key is derived from the password and a salt that
   is saved within the file and Argon2id 1.3. After the decryption key is
   determined, the master key is decrypted using the nonce saved in the file,
   as well as using the mac saved with it to check its integrity. Finally file
   integrity is checked by evaluating the file hash appeneded to the file.

   Assuming all checks pass, the loc data area is processed to load all the
   vault keys into memory along with pointers into the file for where the
   relevant data to retrieve their values are.

   Returns VE_SUCCESS upon opening the vault and creating a keymap for the vault
   VE_PARAMERR if parameters are null or exceed the maximum length for their
   fields VE_MEMERR if secure memory cannot be changed to read write mode
   VE_SYSCALL if snprintf fails or open fails without ENOENT or EACCESS
   VE_EXIST if open fails with ENOENT for the file not existing
   VE_ACCESS if open fails from not having permisions to the file
   VE_CRYPTOERR if the derived password could not be computed
   VE_FILE if the master key cannot be decrypted or the file hash is invalid
 */
int open_vault(char* directory, char* username, char* password,
               struct vault_info* info) {
  if (directory == NULL || username == NULL || password == NULL ||
      strlen(directory) > MAX_PATH_LEN || strlen(username) > MAX_USER_SIZE ||
      strlen(password) > MAX_PASS_SIZE) {
    return VE_PARAMERR;
  }

  int max_size = strlen(directory) + strlen(username) + 10;
  char* pathname = malloc(max_size);
  if (snprintf(pathname, max_size, filename_pattern, directory, username) < 0) {
    free(pathname);
    return VE_SYSCALL;
  }

  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    free(pathname);
    return VE_MEMERR;
  }

  if (info->is_open) {
    FPUTS("Already have a vault open\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    free(pathname);
    return VE_VOPEN;
  }

  int open_results = open(pathname, O_RDWR | O_NOFOLLOW);
  free(pathname);
  if (open_results < 0) {
    if (errno == ENOENT) {
      return VE_EXIST;
    } else if (errno == EACCES) {
      return VE_ACCESS;
    } else {
      return VE_SYSCALL;
    }
  }

  if (flock(open_results, LOCK_EX | LOCK_NB) < 0) {
    FPUTS("Could not get file lock\n", stderr);
    return VE_SYSCALL;
  }

  lseek(open_results, 8, SEEK_SET);
  int open_info_length = SALT_SIZE + MAC_SIZE + MASTER_KEY_SIZE + NONCE_SIZE;
  uint8_t open_info[open_info_length];
  READ(open_results, open_info, open_info_length, info);

  if (PW_HASH(info->derived_key, password, strlen(password), open_info) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    close(open_results);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  if (crypto_secretbox_open_easy(info->decrypted_master, open_info + SALT_SIZE,
                                 MASTER_KEY_SIZE + MAC_SIZE,
                                 open_info + open_info_length - NONCE_SIZE,
                                 info->derived_key) < 0) {
    FPUTS("Could not decrypt master key\n", stderr);
    close(open_results);
    sodium_memzero(info->derived_key, MASTER_KEY_SIZE);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_WRONGPASS;
  }

  info->user_fd = open_results;
  char file_hash[HASH_SIZE];
  char current_hash[HASH_SIZE];
  internal_hash_file(info, (uint8_t*)&file_hash, HASH_SIZE);
  lseek(open_results, -1 * HASH_SIZE, SEEK_END);
  READ(open_results, &current_hash, HASH_SIZE, info);
  if (memcmp((const char*)&file_hash, (const char*)&current_hash, HASH_SIZE) !=
      0) {
    FPUTS("FILE HASHES DO NOT MATCH\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_FILE;
  }

  internal_create_key_map(info);

  info->current_box.key[0] = 0;
  info->is_open = 1;

  if (sodium_mprotect_noaccess(info) < 0) {
    FPUTS("Issues preventing access to memory\n", stderr);
  }

  FPUTS("Opened the vault\n", stderr);
  return VE_SUCCESS;
}

/**
   function close_vault

   Closes a currently open vault by closing the file descriptor, releasing
   the memory associated with the key map on the heap, and finally zeroing
   the memory associated with the vault. Using sodium_memzero attempts to
   circumvent compiler optimizations that would prevent zeroing memory.
   While the memory is not released to the OS and can be reused by different
   vaults, this helps prevent potential issues if their are other issues in
   the vault.

   Returns VE_SUCCESS after releasing the key map, zeroing memory, and closing
           the file descriptor associated with the current vault
   VE_MEMERR if the secure memory cannot be read
   VE_VCLOSE if there is no current vault opened
 */
int close_vault(struct vault_info* info) {
  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    return VE_MEMERR;
  }

  if (!info->is_open) {
    FPUTS("Already have a vault closed\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_VCLOSE;
  }

  close(info->user_fd);
  delete_map(info->key_info);
  sodium_memzero(info->derived_key, MASTER_KEY_SIZE);
  sodium_memzero(info->decrypted_master, MASTER_KEY_SIZE);
  sodium_memzero(&info->current_box, sizeof(struct vault_box));
  info->is_open = 0;

  if (sodium_mprotect_noaccess(info) < 0) {
    FPUTS("Issues preventing access to memory\n", stderr);
  }

  FPUTS("Closed the vault\n", stderr);
  return VE_SUCCESS;
}

/**
   Server communication functions

   The next series of functions are used to create information for the server.
   This includes and initial function which creates data for the server upon
   signup, specifically the double-derived key that the server can verify.
   In addition, responses to recovery questions are used as keys to encrypt the
   master key
 */

int create_data_for_server(struct vault_info* info, uint8_t* response1,
                           uint8_t* response2, uint8_t* first_pass_salt,
                           uint8_t* second_pass_salt, uint8_t* recovery_result,
                           uint8_t* dataencr1, uint8_t* dataencr2,
                           uint8_t* data_salt_11, uint8_t* data_salt_12,
                           uint8_t* data_salt_21, uint8_t* data_salt_22,
                           uint8_t* server_pass) {
  int check;
  if (check = internal_initial_checks(info)) {
    return check;
  }

  randombytes_buf(data_salt_11, SALT_SIZE);
  randombytes_buf(data_salt_12, SALT_SIZE);
  randombytes_buf(data_salt_21, SALT_SIZE);
  randombytes_buf(data_salt_22, SALT_SIZE);
  randombytes_buf(second_pass_salt, SALT_SIZE);

  lseek(info->user_fd, 8, SEEK_SET);
  READ(info->user_fd, first_pass_salt, SALT_SIZE, info);

  if (PW_HASH(server_pass, info->derived_key, MASTER_KEY_SIZE,
              second_pass_salt) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  uint8_t data1_master[MASTER_KEY_SIZE];
  uint8_t data2_master[MASTER_KEY_SIZE];

  if (PW_HASH(&data1_master, response1, strlen(response1), data_salt_11) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  if (PW_HASH(&data2_master, response2, strlen(response2), data_salt_21) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  uint8_t intermediate_result[MASTER_KEY_SIZE + MAC_SIZE];
  randombytes_buf(recovery_result + MASTER_KEY_SIZE + 2 * MAC_SIZE, NONCE_SIZE);
  randombytes_buf(recovery_result + MASTER_KEY_SIZE + 2 * MAC_SIZE + NONCE_SIZE,
                  NONCE_SIZE);
  if (crypto_secretbox_easy((uint8_t*)&intermediate_result,
                            info->decrypted_master, MASTER_KEY_SIZE,
                            recovery_result + MASTER_KEY_SIZE + 2 * MAC_SIZE,
                            (uint8_t*)&data1_master) < 0) {
    FPUTS("Could not encrypt master key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  if (crypto_secretbox_easy(
          recovery_result, (uint8_t*)intermediate_result,
          MASTER_KEY_SIZE + MAC_SIZE,
          recovery_result + MASTER_KEY_SIZE + 2 * MAC_SIZE + NONCE_SIZE,
          (uint8_t*)&data2_master) < 0) {
    FPUTS("Could not encrypt master key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  if (PW_HASH(dataencr1, &data1_master, MASTER_KEY_SIZE, data_salt_12) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  if (PW_HASH(dataencr2, &data2_master, MASTER_KEY_SIZE, data_salt_22) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }
  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

/**
   function create_password_for_server

   Given a currently open vault and the second salt used to create the password
   for the server, create the server password and place it into the provided
   buffer. This function is to be used for checking and updating with the
   server, while the make_passsword function below is for initially downloading
   if the user does not have a vault on the computer.

   Returns VE_SUCCESS if the password was created
   VE_CRYPTOERR if there were any errors with the computation
 */
int create_password_for_server(struct vault_info* info, uint8_t* salt,
                               uint8_t* server_pass) {
  int check;
  if (check = internal_initial_checks(info)) {
    return check;
  }

  if (PW_HASH(server_pass, info->derived_key, MASTER_KEY_SIZE, salt) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }
  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

/**
   function make_password_for_server

   Given a password and two salts, generate a doubly-derived key using Argon2id
   that will be used as a server password. This function should be used in the
   case that a user wants to download their vault from the cloud.

   Returns VE_SUCCESS if the password was created
   VE_CRYPTOERR if there were any errors with the computation
 */
int make_password_for_server(const char* password, const uint8_t* first_salt,
                             const uint8_t* second_salt, uint8_t* server_pass) {
  uint8_t derived_key[MASTER_KEY_SIZE];

  if (PW_HASH(&derived_key, password, strlen(password), first_salt) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  if (PW_HASH(server_pass, derived_key, MASTER_KEY_SIZE, second_salt) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  return VE_SUCCESS;
}

/**
   function create_responses_for_server

   Given the responses to security questions and salts from the server, create
   two keys that will be sent to the server for verification. The use of
   doubly deriving the keys is that the server is not able to invert Argon2id
   with any known methods, preventing decryption of the recovery data.

   Returns VE_SUCCESS if the two keys were created
   VE_CRYPTOERR if there were any errors in derivation
 */
int create_responses_for_server(const uint8_t* response1,
                                const uint8_t* response2,
                                const uint8_t* data_salt_11,
                                const uint8_t* data_salt_12,
                                const uint8_t* data_salt_21,
                                const uint8_t* data_salt_22, uint8_t* dataencr1,
                                uint8_t* dataencr2) {
  uint8_t data1_master[MASTER_KEY_SIZE];
  uint8_t data2_master[MASTER_KEY_SIZE];

  if (PW_HASH(&data1_master, response1, strlen(response1), data_salt_11) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  if (PW_HASH(&data2_master, response2, strlen(response2), data_salt_21) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  if (PW_HASH(dataencr1, &data1_master, MASTER_KEY_SIZE, data_salt_12) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  if (PW_HASH(dataencr2, &data2_master, MASTER_KEY_SIZE, data_salt_22) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  return VE_SUCCESS;
}

int update_key_from_recovery(struct vault_info* info, const char* directory,
                             const char* username, const uint8_t* response1,
                             const uint8_t* response2, const uint8_t* recovery,
                             const uint8_t* data_salt_1,
                             const uint8_t* data_salt_2,
                             const uint8_t* new_password,
                             uint8_t* new_first_salt, uint8_t* new_second_salt,
                             uint8_t* new_server_pass, uint8_t* new_header) {
  if (directory == NULL || username == NULL || new_password == NULL ||
      strlen(directory) > MAX_PATH_LEN || strlen(username) > MAX_USER_SIZE ||
      strlen(new_password) > MAX_PASS_SIZE) {
    return VE_PARAMERR;
  }

  uint8_t data1_master[MASTER_KEY_SIZE];
  uint8_t data2_master[MASTER_KEY_SIZE];

  if (PW_HASH(&data1_master, response1, strlen(response1), data_salt_1) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  if (PW_HASH(&data2_master, response2, strlen(response2), data_salt_2) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    return VE_CRYPTOERR;
  }

  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    return VE_MEMERR;
  }

  if (info->is_open) {
    FPUTS("Already have a vault open\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_VOPEN;
  }

  uint8_t intermediate_result[MASTER_KEY_SIZE + MAC_SIZE * 2 + NONCE_SIZE];
  if (crypto_secretbox_open_easy(
          (uint8_t*)&intermediate_result, recovery,
          MASTER_KEY_SIZE + MAC_SIZE * 2,
          recovery + MASTER_KEY_SIZE + 2 * MAC_SIZE + NONCE_SIZE,
          (uint8_t*)&data2_master) < 0) {
    FPUTS("Could not decrypt master key first time\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_WRONGPASS;
  }

  if (crypto_secretbox_open_easy(
          (uint8_t*)&info->decrypted_master, (uint8_t*)&intermediate_result,
          MASTER_KEY_SIZE + MAC_SIZE, recovery + MASTER_KEY_SIZE + 2 * MAC_SIZE,
          (uint8_t*)&data1_master) < 0) {
    FPUTS("Could not decrypt master key second time\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_WRONGPASS;
  }

  // Check file hash to see if its exact
  int max_size = strlen(directory) + strlen(username) + 10;
  char* pathname = malloc(max_size);
  if (snprintf(pathname, max_size, filename_pattern, directory, username) < 0) {
    free(pathname);
    return VE_SYSCALL;
  }

  int open_results = open(pathname, O_RDWR | O_NOFOLLOW);
  free(pathname);
  if (open_results < 0) {
    if (errno == ENOENT) {
      return VE_EXIST;
    } else if (errno == EACCES) {
      return VE_ACCESS;
    } else {
      return VE_SYSCALL;
    }
  }

  if (flock(open_results, LOCK_EX) < 0) {
    FPUTS("Could not get file lock\n", stderr);
    return VE_SYSCALL;
  }

  info->user_fd = open_results;
  uint8_t file_hash[HASH_SIZE];
  char current_hash[HASH_SIZE];
  internal_hash_file(info, (uint8_t*)&file_hash, HASH_SIZE);
  READ(open_results, &current_hash, HASH_SIZE, info);
  if (memcmp((const char*)&file_hash, (const char*)&current_hash, HASH_SIZE) !=
      0) {
    FPUTS("FILE HASHES DO NOT MATCH\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_FILE;
  }

  // Update the header
  randombytes_buf(new_first_salt, SALT_SIZE);
  if (PW_HASH(info->derived_key, new_password, strlen(new_password),
              new_first_salt) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  uint8_t encrypted_master[MASTER_KEY_SIZE + MAC_SIZE];
  uint8_t master_nonce[NONCE_SIZE];
  randombytes_buf(master_nonce, sizeof master_nonce);
  if (crypto_secretbox_easy(encrypted_master, info->decrypted_master,
                            MASTER_KEY_SIZE, master_nonce,
                            info->derived_key) < 0) {
    FPUTS("Could not encrypt master key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  lseek(info->user_fd, 8, SEEK_SET);
  WRITE(info->user_fd, new_first_salt, crypto_pwhash_SALTBYTES, info);
  WRITE(info->user_fd, &encrypted_master, MASTER_KEY_SIZE + MAC_SIZE, info);
  WRITE(info->user_fd, &master_nonce, NONCE_SIZE, info);

  internal_hash_file(info, (uint8_t*)&file_hash, HASH_SIZE);
  lseek(info->user_fd, -1 * HASH_SIZE, SEEK_END);
  if (write(info->user_fd, &file_hash, HASH_SIZE) < 0) {
    FPUTS("Could not write hash to disk\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  internal_create_key_map(info);

  info->current_box.key[0] = 0;
  info->is_open = 1;

  // Create new result for the server w/ header and salt and password

  lseek(info->user_fd, 0, SEEK_SET);
  READ(info->user_fd, new_header, HEADER_SIZE - 4, info);

  randombytes_buf(new_second_salt, SALT_SIZE);
  if (PW_HASH(new_server_pass, info->derived_key, MASTER_KEY_SIZE,
              new_second_salt) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  if (sodium_mprotect_noaccess(info) < 0) {
    FPUTS("Issues preventing access to memory\n", stderr);
  }

  FPUTS("Changed vault password from recovery\n", stderr);
  return VE_SUCCESS;
}

/**
   Vault modification functions

   The following series of functions are used to directly modify the vault file.
   As the vault is designed as append-only, that has a garbage collection phase
   to remove unused data and condense its size, adding and deleting are quick
   until the loc_data field runs out of space.
 */

/**
   function change_password
 */
int change_password(struct vault_info* info, const char* old_password,
                    const char* new_password) {
  int result;
  if ((result = internal_initial_checks(info))) {
    return result;
  }

  lseek(info->user_fd, 8, SEEK_SET);
  int open_info_length = SALT_SIZE + MAC_SIZE + MASTER_KEY_SIZE + NONCE_SIZE;
  uint8_t open_info[open_info_length];
  READ(info->user_fd, open_info, open_info_length, info);

  uint8_t keypass[MASTER_KEY_SIZE];
  if (PW_HASH((uint8_t*) &keypass, old_password, strlen(old_password),
              open_info) < 0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  uint8_t master[MASTER_KEY_SIZE];
  if (crypto_secretbox_open_easy((uint8_t*) &master, open_info + SALT_SIZE,
                                 MASTER_KEY_SIZE + MAC_SIZE,
                                 open_info + open_info_length - NONCE_SIZE,
                                 (uint8_t*) &keypass) < 0) {
    FPUTS("Could not decrypt master key\n", stderr);
    sodium_memzero(info->derived_key, MASTER_KEY_SIZE);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_WRONGPASS;
  }
  if (memcmp((uint8_t*) master, &info->decrypted_master, MASTER_KEY_SIZE) != 0) {
    FPUTS("Wrong password\n", stderr);
    sodium_memzero(info->derived_key, MASTER_KEY_SIZE);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_WRONGPASS;
  }
  sodium_memzero(&master, MASTER_KEY_SIZE);
  sodium_memzero(&keypass, MASTER_KEY_SIZE);

  uint8_t salt[SALT_SIZE];
  randombytes_buf(salt, sizeof salt);
  if (PW_HASH(info->derived_key, new_password, strlen(new_password), salt) <
      0) {
    FPUTS("Could not dervie password key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  uint8_t encrypted_master[MASTER_KEY_SIZE + MAC_SIZE];
  uint8_t master_nonce[NONCE_SIZE];
  randombytes_buf(master_nonce, sizeof master_nonce);
  if (crypto_secretbox_easy(encrypted_master, info->decrypted_master,
                            MASTER_KEY_SIZE, master_nonce,
                            info->derived_key) < 0) {
    FPUTS("Could not encrypt master key\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_CRYPTOERR;
  }

  lseek(info->user_fd, 8, SEEK_SET);
  WRITE(info->user_fd, &salt, crypto_pwhash_SALTBYTES, info);
  WRITE(info->user_fd, &encrypted_master, MASTER_KEY_SIZE + MAC_SIZE, info);
  WRITE(info->user_fd, &master_nonce, NONCE_SIZE, info);

  uint8_t file_hash[HASH_SIZE];
  internal_hash_file(info, (uint8_t*)&file_hash, HASH_SIZE);
  lseek(info->user_fd, -1 * HASH_SIZE, SEEK_END);
  if (write(info->user_fd, &file_hash, HASH_SIZE) < 0) {
    FPUTS("Could not write hash to disk\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  if (sodium_mprotect_noaccess(info) < 0) {
    FPUTS("Issues preventing access to memory\n", stderr);
  }

  FPUTS("Changed vault password\n", stderr);
  return VE_SUCCESS;
}

/**
   function add_key

   Function to add a new key to the vault. Validates that a current vault is
   opened and that the key does not already exist in the vault, and then
   attempts to append the key to the vault.
 */
int add_key(struct vault_info* info, uint8_t type, const char* key,
            const char* value) {
  if (info == NULL || key == NULL || value == NULL ||
      strlen(value) > DATA_SIZE || strlen(key) > BOX_KEY_SIZE - 1) {
    return VE_PARAMERR;
  }

  int result;
  if ((result = internal_initial_checks(info))) {
    return result;
  }

  if (get_info(info->key_info, key)) {
    FPUTS("Key already in map; use update\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_KEYEXIST;
  }

  if (internal_append_key(info, type, key, value) != 0) {
    internal_condense_file(info);
    if (sodium_mprotect_readwrite(info) < 0) {
      FPUTS("Issues gaining access to memory\n", stderr);
      return VE_MEMERR;
    }
    return internal_append_key(info, type, key, value);
  }

  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

// Result needs to be freed by caller
int get_vault_keys(struct vault_info* info, char** results) {
  int check;
  if ((check = internal_initial_checks(info))) {
    return check;
  }

  char** result = get_keys(info->key_info);
  uint32_t keynum = num_keys(info->key_info);
  for (int i = 0; i < keynum; ++i) {
    strcpy(results[i], result[i]);
    free(result[i]);
  }
  free(result);

  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

/**
   function num_vault_keys
 */
uint32_t num_vault_keys(struct vault_info* info) {
  int check;
  if ((check = internal_initial_checks(info))) {
    return check;
  }

  uint32_t result = num_keys(info->key_info);
  sodium_mprotect_noaccess(info);
  return result;
}

/**
   function last_modified_time
 */
uint64_t last_modified_time(struct vault_info* info, const char* key) {
  if (info == NULL || key == NULL || strlen(key) > BOX_KEY_SIZE - 1) {
    return VE_PARAMERR;
  }

  int result;
  if ((result = internal_initial_checks(info))) {
    return result;
  }

  const struct key_info* current_info;
  if (!(current_info = get_info(info->key_info, key))) {
    FPUTS("Key not in map\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_KEYEXIST;
  }

  sodium_mprotect_noaccess(info);
  return current_info->m_time;
}

/**
   function open_key
 */
int open_key(struct vault_info* info, const char* key) {
  if (info == NULL || key == NULL || strlen(key) > BOX_KEY_SIZE - 1) {
    return VE_PARAMERR;
  }

  int result;
  if ((result = internal_initial_checks(info))) {
    return result;
  }

  const struct key_info* current_info;
  if (!(current_info = get_info(info->key_info, key))) {
    FPUTS("Key not in map\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_KEYEXIST;
  }

  if (info->current_box.key[0] != 0 &&
      strncmp(key, (char*)&(info->current_box.key), BOX_KEY_SIZE) == 0) {
    sodium_mprotect_noaccess(info);
    return VE_SUCCESS;
  }

  lseek(info->user_fd, current_info->inode_loc, SEEK_SET);
  uint32_t loc_data[LOC_SIZE / sizeof(uint32_t)];
  READ(info->user_fd, loc_data, LOC_SIZE, info);
  uint32_t file_loc = loc_data[1];
  uint32_t key_len = loc_data[2];
  uint32_t val_len = loc_data[3];

  int box_len =
      ENTRY_HEADER_SIZE + key_len + val_len + MAC_SIZE + NONCE_SIZE + HASH_SIZE;
  uint8_t* box = malloc(box_len);
  lseek(info->user_fd, file_loc, SEEK_SET);
  READ(info->user_fd, box, box_len, info);

  uint8_t hash[HASH_SIZE];
  crypto_generichash((uint8_t*)&hash, HASH_SIZE, box, box_len - HASH_SIZE,
                     info->decrypted_master, MASTER_KEY_SIZE);

  if (memcmp((char*)&hash, box + box_len - HASH_SIZE, HASH_SIZE) != 0) {
    FPUTS("ENTRY HASH INVALID\n", stderr);
    free(box);
    sodium_mprotect_noaccess(info);
    return VE_CRYPTOERR;
  }

  uint32_t val_loc = ENTRY_HEADER_SIZE + key_len;
  if (crypto_secretbox_open_easy((uint8_t*)&(info->current_box.value),
                                 box + val_loc, val_len + MAC_SIZE,
                                 box + box_len - HASH_SIZE - NONCE_SIZE,
                                 (uint8_t*)&info->decrypted_master) < 0) {
    FPUTS("Could not decrypt value\n", stderr);
    free(box);
    sodium_mprotect_noaccess(info);
    return VE_CRYPTOERR;
  }

  strncpy((char*)&(info->current_box.key), key, BOX_KEY_SIZE);
  info->current_box.type = box[ENTRY_HEADER_SIZE - 1];
  info->current_box.val_len = val_len;
  free(box);
  sodium_mprotect_noaccess(info);

  FPUTS("Opened a key\n", stderr);
  return VE_SUCCESS;
}

/**
   function delete_key
 */
int delete_key(struct vault_info* info, const char* key) {
  if (info == NULL || key == NULL || strlen(key) > BOX_KEY_SIZE - 1) {
    return VE_PARAMERR;
  }

  int result;
  if ((result = internal_initial_checks(info))) {
    return result;
  }

  const struct key_info* current_info;
  if (!(current_info = get_info(info->key_info, key))) {
    FPUTS("Key not in map\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_KEYEXIST;
  }

  lseek(info->user_fd, current_info->inode_loc, SEEK_SET);
  uint32_t loc_data[LOC_SIZE / sizeof(uint32_t)];
  READ(info->user_fd, loc_data, LOC_SIZE, info);
  uint32_t file_loc = loc_data[1];
  uint32_t key_len = loc_data[2];
  uint32_t val_len = loc_data[3];

  delete_entry(info->key_info, key);
  lseek(info->user_fd, current_info->inode_loc, SEEK_SET);
  uint32_t state_update = 1;
  WRITE(info->user_fd, &state_update, sizeof(uint32_t), info);
  int size = val_len + MAC_SIZE;
  char* zeros = malloc(size);
  sodium_memzero(zeros, size);

  lseek(info->user_fd, file_loc + ENTRY_HEADER_SIZE + key_len, SEEK_SET);
  if (write(info->user_fd, zeros, size) < 0) {
    free(zeros);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  uint8_t file_hash[HASH_SIZE];
  internal_hash_file(info, (uint8_t*)&file_hash, 0);
  lseek(info->user_fd, 0, SEEK_END);
  if (write(info->user_fd, &file_hash, HASH_SIZE) < 0) {
    FPUTS("Could not write hash to disk\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_IOERR;
  }

  free(zeros);
  sodium_mprotect_noaccess(info);
  FPUTS("Deleted key\n", stderr);
  return VE_SUCCESS;
}

/**
   function update_key
 */
int update_key(struct vault_info* info, uint8_t type, const char* key,
               const char* value) {
  if (info == NULL || key == NULL || value == NULL ||
      strlen(value) > DATA_SIZE || strlen(key) > BOX_KEY_SIZE - 1) {
    return VE_PARAMERR;
  }

  int result = delete_key(info, key);
  if (result != VE_SUCCESS) {
    return result;
  }
  return add_key(info, type, key, value);
}

int place_open_value(struct vault_info* info, char* result, int* len,
                     char* type) {
  if (sodium_mprotect_readwrite(info) < 0) {
    FPUTS("Issues gaining access to memory\n", stderr);
    return VE_MEMERR;
  }
  memcpy(result, (char*)&info->current_box.value, info->current_box.val_len);
  *len = info->current_box.val_len;
  *type = info->current_box.type;
  result[info->current_box.val_len] = 0;

  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

int add_encrypted_value(struct vault_info* info, const char* key,
                        const char* value, int len, uint8_t type) {
  if (info == NULL || key == NULL || strlen(key) > BOX_KEY_SIZE - 1) {
    return VE_PARAMERR;
  }

  int result;
  if ((result = internal_initial_checks(info))) {
    return result;
  }

  const struct key_info* current_info;
  if ((current_info = get_info(info->key_info, key))) {
    FPUTS("Key in map\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_KEYEXIST;
  }

  uint8_t hash[HASH_SIZE];
  crypto_generichash((uint8_t*)&hash, HASH_SIZE, value, len - HASH_SIZE,
                     info->decrypted_master, MASTER_KEY_SIZE);

  if (memcmp((char*)&hash, value + len - HASH_SIZE, HASH_SIZE) != 0) {
    FPUTS("ENTRY HASH INVALID\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_FILE;
  }

  if (internal_append_encrypted(info, type, key, value, len) != VE_SUCCESS) {
    internal_condense_file(info);
    if (sodium_mprotect_readwrite(info) < 0) {
      FPUTS("Issues gaining access to memory\n", stderr);
      return VE_MEMERR;
    }
    return internal_append_encrypted(info, type, key, value, len);
  }

  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

int get_encrypted_value(struct vault_info* info, const char* key, char* result,
                        int* len, uint8_t* type) {
  if (info == NULL || key == NULL || strlen(key) > BOX_KEY_SIZE - 1) {
    return VE_PARAMERR;
  }

  int check;
  if ((check = internal_initial_checks(info))) {
    return check;
  }

  const struct key_info* current_info;
  if (!(current_info = get_info(info->key_info, key))) {
    FPUTS("Key not in map\n", stderr);
    if (sodium_mprotect_noaccess(info) < 0) {
      FPUTS("Issues preventing access to memory\n", stderr);
    }
    return VE_KEYEXIST;
  }

  lseek(info->user_fd, current_info->inode_loc, SEEK_SET);
  uint32_t loc_data[LOC_SIZE / sizeof(uint32_t)];
  READ(info->user_fd, loc_data, LOC_SIZE, info);
  uint32_t file_loc = loc_data[1];
  uint32_t key_len = loc_data[2];
  uint32_t val_len = loc_data[3];

  int box_len =
      ENTRY_HEADER_SIZE + key_len + val_len + MAC_SIZE + NONCE_SIZE + HASH_SIZE;
  lseek(info->user_fd, file_loc, SEEK_SET);
  READ(info->user_fd, result, box_len, info);

  uint8_t hash[HASH_SIZE];
  crypto_generichash((uint8_t*)&hash, HASH_SIZE, result, box_len - HASH_SIZE,
                     info->decrypted_master, MASTER_KEY_SIZE);

  if (memcmp((char*)&hash, result + box_len - HASH_SIZE, HASH_SIZE) != 0) {
    FPUTS("ENTRY HASH INVALID\n", stderr);
    sodium_mprotect_noaccess(info);
    return VE_CRYPTOERR;
  }

  *type = current_info->type;
  *len = box_len;
  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

int get_header(struct vault_info* info, char* result) {
  int check;
  if ((check = internal_initial_checks(info))) {
    return check;
  }

  lseek(info->user_fd, 0, SEEK_SET);
  READ(info->user_fd, result, HEADER_SIZE - 4, info);
  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}

uint64_t get_last_server_time(struct vault_info* info) {
  int check;
  if ((check = internal_initial_checks(info))) {
    return check;
  }

  uint64_t result;
  lseek(info->user_fd, HEADER_SIZE - 12, SEEK_SET);
  READ(info->user_fd, &result, 8, info);
  sodium_mprotect_noaccess(info);
  return result;
}

int set_last_server_time(struct vault_info* info, uint64_t timestamp) {
  int check;
  if ((check = internal_initial_checks(info))) {
    return check;
  }
  lseek(info->user_fd, HEADER_SIZE - 12, SEEK_SET);
  WRITE(info->user_fd, &timestamp, 8, info);
  sodium_mprotect_noaccess(info);
  return VE_SUCCESS;
}