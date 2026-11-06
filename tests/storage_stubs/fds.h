#ifndef TEST_FDS_H
#define TEST_FDS_H
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t ret_code_t;
#define NRF_SUCCESS 0U
enum { FDS_ERR_NOT_FOUND=10, FDS_ERR_BUSY, FDS_ERR_NO_SPACE_IN_QUEUES,
       FDS_ERR_NO_SPACE_IN_FLASH, FDS_ERR_NOT_INITIALIZED, FDS_ERR_INTERNAL };
typedef enum { FDS_EVT_INIT, FDS_EVT_WRITE, FDS_EVT_UPDATE, FDS_EVT_DEL_RECORD,
               FDS_EVT_DEL_FILE, FDS_EVT_GC } fds_evt_id_t;
typedef struct { uint16_t length_words; } fds_header_t;
typedef struct { uint32_t record_id; } fds_record_desc_t;
typedef struct { uint32_t cursor; } fds_find_token_t;
typedef struct { const fds_header_t *p_header; const void *p_data; } fds_flash_record_t;
typedef struct { uint16_t file_id, key;
    struct { const void *p_data; uint16_t length_words; } data;
} fds_record_t;
typedef struct { fds_evt_id_t id; uint32_t result;
    union {
        struct { uint32_t record_id; uint16_t file_id, record_key; bool is_record_updated; } write;
        struct { uint32_t record_id; uint16_t file_id, record_key; } del;
    };
} fds_evt_t;
uint32_t fds_register(void (*fn)(const fds_evt_t *));
uint32_t fds_init(void);
uint32_t fds_gc(void);
uint32_t fds_record_find(uint16_t, uint16_t, fds_record_desc_t *, fds_find_token_t *);
uint32_t fds_record_open(fds_record_desc_t *, fds_flash_record_t *);
uint32_t fds_record_close(fds_record_desc_t *);
uint32_t fds_record_write(fds_record_desc_t *, const fds_record_t *);
uint32_t fds_record_update(fds_record_desc_t *, const fds_record_t *);
uint32_t fds_record_delete(fds_record_desc_t *);
#endif
