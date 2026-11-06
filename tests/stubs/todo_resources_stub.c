#include "resource_layout.h"
#include "resource_store.h"
#include "todo_items.h"

#include <assert.h>
#include <string.h>

/* Real scheduling/validation reads this in-memory external Flash package. */
static uint8_t payload[TODO_ITEMS_MAX_BYTES];
static size_t payload_length;
static bool fail_reads;

void todo_resources_stub_load(const uint8_t *data, size_t length)
{
    assert(length <= sizeof(payload));
    assert(data != NULL || length == 0U);
    if (length != 0U) memcpy(payload, data, length);
    payload_length = length;
    fail_reads = false;
}

void todo_resources_stub_fail_reads(bool fail) { fail_reads = fail; }

rs_result_t resource_store_open(uint16_t id, uint32_t *length)
{
    if (id != RS_PART_TODO_ITEMS || payload_length == 0U) return RS_NOT_FOUND;
    if (length == NULL) return RS_INVALID;
    *length = (uint32_t)payload_length;
    return RS_OK;
}

rs_result_t resource_store_read(uint16_t id, uint32_t offset, void *data, size_t length)
{
    if (id != RS_PART_TODO_ITEMS || payload_length == 0U) return RS_NOT_FOUND;
    if (fail_reads) return RS_IO;
    if (data == NULL || offset > payload_length || length > payload_length - offset) return RS_INVALID;
    memcpy(data, payload + offset, length);
    return RS_OK;
}
