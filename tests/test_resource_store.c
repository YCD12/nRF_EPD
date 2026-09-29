#include "resource_store.h"
#include "resource_protocol.h"
#include "resource_transfer.h"
#include "persistent_record.h"
#include "fm25q04b.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[FM25Q04B_SIZE_BYTES], snapshot[FM25Q04B_SIZE_BYTES];
static int cut = -1;
static size_t prefix;
static jmp_buf power_loss;
static unsigned mutations, reads, depth, executions, sent;
static bool block_send;
static uint8_t reply[20];
static unsigned invalidations, activations, offline_requests;
static uint16_t activated_partitions;
static rs_result_t activation_result;
static bool entry_busy;

static rs_result_t on_resource(void *context, resource_event_t event, uint16_t changed)
{
    assert(context == &reply && depth == 0 && resource_protocol_entered());
    if (event == RESOURCE_ENTER) return entry_busy ? RS_BUSY : RS_OK;
    if (event == RESOURCE_INVALIDATE) invalidations++;
    else if (event == RESOURCE_OFFLINE) offline_requests++;
    else {
        activations++;
        activated_partitions = changed;
        return activation_result;
    }
    return RS_OK;
}

void test_critical_enter(void) { ++depth; }
void test_critical_exit(void) { assert(depth); --depth; }
ret_code_t fm25q04b_init(void) { return NRF_SUCCESS; }
ret_code_t fm25q04b_probe(fm25q04b_jedec_id_t *id) { memset(id, 0, sizeof(*id)); return NRF_SUCCESS; }
ret_code_t fm25q04b_read(uint32_t at, void *p, size_t n)
{
    assert(depth == 0 && at <= sizeof(flash) && n <= sizeof(flash) - at);
    ++reads;
    memcpy(p, flash + at, n);
    return NRF_SUCCESS;
}
static void mutate(uint32_t at, const uint8_t *p, size_t n)
{
    assert(depth == 0 && at <= sizeof(flash) && n <= sizeof(flash) - at);
    ++mutations;
    bool stop = cut == 0;
    if (cut >= 0) --cut;
    if (stop && n > prefix) n = prefix;
    for (size_t i = 0; i < n; ++i) {
        if (p) flash[at + i] &= p[i];
        else flash[at + i] = 0xFF;
    }
    if (stop) longjmp(power_loss, 1);
}
ret_code_t fm25q04b_write(uint32_t at, const void *p, size_t n) { mutate(at, p, n); return NRF_SUCCESS; }
ret_code_t fm25q04b_erase_sector(uint32_t at)
{
    assert(at % 4096 == 0);
    mutate(at, NULL, 4096);
    return NRF_SUCCESS;
}
static uint32_t crc(const uint8_t *p, size_t n) { return resource_crc32(UINT32_MAX, p, n) ^ UINT32_MAX; }
static void reboot(void) { resource_protocol_reset(); (void)resource_store_mount(); }
static uint8_t table[48];
static void make_table(void)
{
    memset(table, 0, sizeof(table));
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t *p = table + i * 16;
        p[0] = p[2] = i == 2 ? 8 : i + 1;
        resource_put_u32(p + 4, i * 4096);
        resource_put_u32(p + 8, i == 2 ? sizeof(flash) - 8192 : 4096);
        p[12] = 1;
    }
}
static rs_result_t upload(unsigned id, const uint8_t *p, size_t n)
{
    rs_result_t r = resource_store_begin(id, n, crc(p, n), 1);
    if (r == RS_OK) r = resource_store_write(0, p, n);
    if (r == RS_OK) r = resource_store_commit();
    return r;
}
static void blank(void)
{
    cut = -1;
    memset(flash, 0xFF, sizeof(flash));
    reboot();
    make_table();
    assert(upload(1, table, sizeof(table)) == RS_OK);
}
static void test_boundaries(void)
{
    blank();
    uint8_t data[32], actual[32];
    memset(data, 0x39, sizeof(data));
    unsigned before = mutations;
    assert(resource_store_begin(2, 4096, 0, 1) == RS_INVALID);
    assert(resource_store_begin(8, 12, 0, 1) == RS_WRITE_PROTECTED);
    assert(resource_store_begin(99, 12, 0, 1) == RS_NOT_FOUND);
    assert(before == mutations);
    assert(resource_store_begin(2, sizeof(data), crc(data, sizeof(data)), 1) == RS_OK);
    assert(resource_store_mount() == RS_BUSY);
    assert(resource_store_write(UINT32_MAX, data, 1) == RS_INVALID);
    assert(resource_store_write(1, data, 1) == RS_INVALID);
    assert(resource_store_write(0, data, 16) == RS_OK);
    assert(resource_store_write(0, data, 16) == RS_OK);
    data[0]++;
    assert(resource_store_write(0, data, 16) == RS_INVALID);
    data[0]--;
    assert(resource_store_commit() == RS_INVALID);
    assert(resource_store_write(16, data + 16, 16) == RS_OK);
    assert(resource_store_commit() == RS_OK);
    uint32_t length;
    assert(resource_store_open(2, &length) == RS_OK && length == sizeof(data));
    unsigned reads_before = reads;
    assert(resource_store_read(2, 0, actual, sizeof(actual)) == RS_OK);
    assert(reads == reads_before + 1U); /* Opened package metadata is cached. */
    assert(!memcmp(actual, data, sizeof(data)));
    assert(resource_store_read(2, UINT32_MAX, actual, 1) == RS_INVALID);
    flash[4096 + 32] ^= 1;
    assert(resource_store_open(2, &length) == RS_CRC && length == 0);
    /* A well-checksummed but overlapping table must never commit. */
    resource_put_u32(table + 16 + 4, 0);
    assert(upload(1, table, sizeof(table)) == RS_INVALID);
    reboot();
    assert(resource_store_mount() != RS_OK);
    make_table();
    assert(upload(1, table, sizeof(table)) == RS_OK); /* damaged table is repairable */
    table[16] = 1;
    assert(upload(1, table, sizeof(table)) == RS_INVALID); /* duplicate ID */
}
static void test_power_loss(unsigned id)
{
    uint8_t old[40], next[40], actual[40];
    memset(old, 0x11, sizeof(old));
    memset(next, 0x22, sizeof(next));
    blank();
    assert(upload(2, old, sizeof(old)) == RS_OK);
    memcpy(snapshot, flash, sizeof(flash));
    unsigned interruptions = 0;
    for (volatile unsigned step = 0; step < 9; ++step) {
        for (volatile unsigned bytes = 0; bytes <= 49; ++bytes) {
            memcpy(flash, snapshot, sizeof(flash));
            reboot();
            prefix = bytes == 49 ? 4096 : bytes;
            cut = step;
            if (setjmp(power_loss) == 0) {
                (void)upload(id, id == 1 ? table : next, id == 1 ? sizeof(table) : sizeof(next));
            } else ++interruptions;
            cut = -1;
            reboot();
            uint32_t length;
            rs_result_t r = resource_store_open(id, &length);
            if (r == RS_OK && id == 2) {
                assert(length == sizeof(old));
                assert(resource_store_read(2, 0, actual, sizeof(actual)) == RS_OK);
                assert(!memcmp(actual, old, sizeof(old)) || !memcmp(actual, next, sizeof(next)));
            }
            if (id == 1 && r == RS_OK) assert(length == sizeof(table));
        }
    }
    assert(interruptions);
}
static void test_sector_crossing(void)
{
    blank();
    resource_put_u32(table + 16 + 8, 8192);
    resource_put_u32(table + 32 + 4, 12288);
    resource_put_u32(table + 32 + 8, sizeof(flash) - 12288);
    assert(upload(1, table, sizeof(table)) == RS_OK);
    uint8_t data[5000], actual[31];
    memset(data, 0x53, sizeof(data));
    flash[12288] = 0x76; /* adjacent partition must remain untouched */
    assert(resource_store_begin(2, sizeof(data), crc(data, sizeof(data)), 7) == RS_OK);
    assert(resource_store_write(0, data, 4050) == RS_OK);
    assert(resource_store_write(4050, data + 4050, 950) == RS_OK);
    assert(resource_store_commit() == RS_OK && flash[12288] == 0x76);
    uint32_t length;
    assert(resource_store_open(2, &length) == RS_OK && length == sizeof(data));
    assert(resource_store_read(2, 4050, actual, sizeof(actual)) == RS_OK);
    assert(!memcmp(actual, data, sizeof(actual)));
    /* A changed table cannot redirect an erase after the original mount. */
    flash[32 + 16 + 4] ^= 1;
    unsigned before = mutations;
    assert(resource_store_begin(2, 1, 0, 1) == RS_CRC);
    assert(before == mutations);
}
static void test_persistent_record(void)
{
    const uint32_t magic = 0x31545354UL; /* TST1 */
    uint8_t data[] = {1, 2, 3, 4, 5}, actual[8] = {0};
    persistent_record_info_t info;
    blank();
    assert(persistent_record_save(2, magic, data, sizeof(data), false, &info) == RS_OK);
    assert(info.revision == 1 && info.length == sizeof(data));
    assert(info.data_crc == crc(data, sizeof(data)));
    assert(persistent_record_load(2, magic, actual, sizeof(actual), &info) == RS_OK);
    assert(info.revision == 1 && info.length == sizeof(data));
    assert(!memcmp(actual, data, sizeof(data)));
    data[0] = 9;
    assert(persistent_record_save(2, magic, data, sizeof(data), false, &info) == RS_OK);
    assert(info.revision == 2);
    assert(persistent_record_save(2, magic, NULL, 0, true, &info) == RS_OK);
    assert(info.revision == 3 && info.data_crc == 0);
    assert(persistent_record_load(2, magic, actual, sizeof(actual), &info) == RS_NOT_FOUND);
}
static uint32_t send_reply(void *context, uint32_t generation, uint8_t *p, uint16_t n)
{
    (void)context;
    assert(generation && depth == 0 && n <= 20);
    ++executions;
    if (block_send) return NRF_ERROR_RESOURCES;
    memcpy(reply, p, n);
    ++sent;
    return NRF_SUCCESS;
}
static void process(unsigned gen, bool connected, unsigned now)
{
    resource_transfer_process(NULL, gen, connected, true, now, send_reply);
}
static void test_protocol(void)
{
    blank();
    uint8_t request[] = {0x26, 6, 1, 'M', 'I', 'G', '1'};
    resource_transfer_receive(request, sizeof(request), 1);
    block_send = true;
    process(1, true, 1);
    assert(executions == 1 && !sent);
    block_send = false;
    process(1, true, 2);
    assert(sent == 1 && reply[3] == RS_OK);
    unsigned before = mutations;
    resource_transfer_receive(request, sizeof(request), 1);
    process(1, true, 3);
    assert(sent == 2 && before == mutations);
    uint8_t begin[16] = {0x26, 2, 2, 2};
    resource_put_u32(begin + 4, 32);
    resource_transfer_receive(begin, sizeof(begin), 1);
    process(1, true, 4);
    assert(reply[3] == RS_OK);
    before = mutations;
    resource_transfer_receive(begin, sizeof(begin), 1);
    process(1, true, 5);
    assert(mutations == before); /* duplicate BEGIN must not erase again */
    uint8_t leave[] = {0x26, 7, 3};
    resource_transfer_receive(leave, sizeof(leave), 1);
    process(1, true, 6);
    assert(reply[3] == RS_OK && !resource_store_active() && !resource_protocol_entered());
    process(2, false, 6);
    assert(!resource_store_active());
    resource_transfer_receive(begin, sizeof(begin), 3);
    process(3, true, 7);
    assert(reply[3] == RS_WRITE_PROTECTED); /* ENTER is connection owned */
    uint8_t read_bad[] = {0x26, 1, 3, 0xff, 0xff, 0xff, 0xff, 16};
    resource_transfer_receive(read_bad, sizeof(read_bad), 3);
    process(3, true, 8);
    assert(reply[3] == RS_INVALID);
}
static void test_activation(void)
{
    blank();
    resource_protocol_set_handler(on_resource, &reply);
    process(4, true, 100);
    uint8_t enter[] = {0x26, 6, 10, 'M', 'I', 'G', '1'};
    uint8_t begin[16] = {0x26, 2, 11, 2};
    uint8_t write[] = {0x26, 3, 12, 0, 0, 0, 0, 42};
    uint8_t commit[] = {0x26, 4, 13}, leave[] = {0x26, 7, 14}, run[] = {0x26, 8, 15};
    resource_put_u32(begin + 4, 1);
    resource_put_u32(begin + 8, crc(write + 7, 1));
    resource_put_u32(begin + 12, 2);

    entry_busy = true;
    resource_transfer_receive(enter, sizeof(enter), 4); process(4, true, 99);
    assert(reply[3] == RS_BUSY && !resource_protocol_entered());
    entry_busy = false; enter[2]++;

    resource_transfer_receive(enter, sizeof(enter), 4); process(4, true, 100);
    process(4, true, 159); assert(resource_protocol_entered());
    process(4, true, 160); assert(!resource_protocol_entered() && !activations);
    /* Timeout covers ENTER with no BEGIN and idle time after COMMIT. */
    resource_transfer_receive(enter, sizeof(enter), 4); process(4, true, 161);
    resource_transfer_receive(begin, sizeof(begin), 4); process(4, true, 162);
    assert(invalidations == 1);
    resource_transfer_receive(begin, sizeof(begin), 4); process(4, true, 163);
    assert(invalidations == 1);
    resource_transfer_receive(write, sizeof(write), 4); process(4, true, 164);
    resource_transfer_receive(commit, sizeof(commit), 4); process(4, true, 165);
    assert(reply[3] == RS_OK && !resource_store_active());
    process(4, true, 225);
    assert(!resource_protocol_entered() && activations == 1 && activated_partitions == (1U << 2));

    /* A semantic error must prevent offline mode, including retry after LEAVE. */
    resource_transfer_receive(enter, sizeof(enter), 4); process(4, true, 226);
    resource_transfer_receive(begin, sizeof(begin), 4); process(4, true, 227);
    activation_result = RS_INVALID;
    resource_transfer_receive(leave, sizeof(leave), 4); process(4, true, 228);
    assert(reply[3] == RS_INVALID && !resource_protocol_entered());
    leave[2]++;
    resource_transfer_receive(leave, sizeof(leave), 4); process(4, true, 228);
    assert(reply[3] == RS_INVALID && !resource_protocol_entered());
    resource_transfer_receive(run, sizeof(run), 4); process(4, true, 229);
    assert(reply[3] == RS_INVALID && offline_requests == 0 && activated_partitions == (1U << 2));
    activation_result = RS_OK;
    run[2]++;
    resource_transfer_receive(run, sizeof(run), 4); process(4, true, 230);
    assert(reply[3] == RS_OK && offline_requests == 1);
    unsigned before = activations;
    resource_transfer_receive(run, sizeof(run), 4); process(4, true, 231);
    assert(activations == before && offline_requests == 1);

    resource_transfer_receive(enter, sizeof(enter), 4); process(4, true, 232);
    resource_transfer_receive(begin, sizeof(begin), 4); process(4, true, 233);
    process(5, false, 234);
    assert(activations == before + 1 && !resource_store_active() && !resource_protocol_entered());
    process(5, false, 235);
    assert(activations == before + 1); /* Disconnected main loop does not keep reloading. */
    resource_protocol_set_handler(NULL, NULL);
}

static void test_disconnected_read_cache(void)
{
    blank();
    uint8_t data[] = {7, 8}, actual[2];
    assert(upload(2, data, sizeof(data)) == RS_OK);
    process(6, false, 240);
    uint32_t length;
    assert(resource_store_open(2, &length) == RS_OK);
    for (unsigned i = 0; i < 5; ++i) process(6, false, 241 + i);
    unsigned before = reads;
    assert(resource_store_read(2, 0, actual, sizeof(actual)) == RS_OK);
    assert(!memcmp(data, actual, sizeof(data)) && reads == before + 1);
}

int main(void)
{
    assert(crc((const uint8_t *)"123456789", 9) == 0xCBF43926UL);
    test_boundaries();
    test_power_loss(1);
    test_power_loss(2);
    test_sector_crossing();
    test_persistent_record();
    test_protocol();
    test_activation();
    test_disconnected_read_cache();
    puts("resource store: boundaries, CRC, interrupted erase/program/commit, repair, protocol retries OK");
    return 0;
}
