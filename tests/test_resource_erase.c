/* Reuse the simulated SPI Flash, power-cut injector and real protocol harness. */
#define main resource_store_existing_main
#include "test_resource_store.c"
#undef main

static void all_bytes(uint32_t start, uint32_t end, uint8_t value)
{
    for (uint32_t i = start; i < end; i++) assert(flash[i] == value);
}

int main(void)
{
    resource_protocol_set_handler(NULL, NULL);
    blank(); make_table();
    assert(upload(1, table, sizeof(table)) == RS_OK);
    uint8_t data[] = {1,2,3,4};
    assert(upload(2, data, sizeof(data)) == RS_OK);
    uint32_t length;
    assert(resource_store_open(2, &length) == RS_OK);
    memcpy(snapshot, flash, sizeof(flash));
    unsigned before = mutations;
    assert(resource_store_erase(2, 1) == RS_INVALID);
    assert(resource_store_erase(2, 4096) == RS_INVALID);
    assert(resource_store_erase(0, sizeof(flash)) == RS_INVALID);
    assert(resource_store_erase(0, UINT32_MAX) == RS_INVALID);
    assert(resource_store_erase(77, 0) == RS_NOT_FOUND);
    assert(mutations == before);
    assert(resource_store_erase(2, 0) == RS_OK);
    assert(resource_store_read(2, 0, data, sizeof(data)) == RS_NOT_FOUND);
    assert(!memcmp(flash, snapshot, 4096));
    assert(!memcmp(flash + 8192, snapshot + 8192, sizeof(flash) - 8192));
    all_bytes(4096, 8192, 255);

    /* Revalidate the table before erasing; never use cached old locators. */
    assert(upload(1, table, sizeof(table)) == RS_OK);
    flash[32 + 16 + 4] ^= 1;
    before = mutations;
    assert(resource_store_erase(2, 0) == RS_CRC && before == mutations);
    assert(resource_store_erase(1, 0) == RS_OK);
    assert(resource_store_mount() == RS_NOT_FOUND);
    assert(upload(1, table, sizeof(table)) == RS_OK);

    /* System/reserved partitions are deletable, although uploads protect them. */
    memset(flash + 8192, 0x31, sizeof(flash) - 8192);
    assert(resource_store_erase(8, 0) == RS_OK);
    all_bytes(8192, 12288, 255);
    assert(flash[12288] == 0x31);
    table[32] = table[34] = 4;
    assert(upload(1, table, sizeof(table)) == RS_OK);
    assert(resource_store_erase(4, 4096) == RS_OK);
    all_bytes(12288, 16384, 255);

    /* Full 16-bit partition IDs use the same bounded wire operation. */
    table[16] = 0x2c; table[17] = 1;
    assert(upload(1, table, sizeof(table)) == RS_OK);
    uint8_t q[] = {0x26,11,10,0x2c,1,0,0,0,0};
    before = mutations;
    resource_protocol_execute(q,sizeof(q),reply);
    assert(reply[3] == RS_WRITE_PROTECTED && mutations == before);
    uint8_t enter[] = {0x26,6,11,'M','I','G','1'};
    resource_protocol_set_handler(on_resource,&reply);
    resource_protocol_execute(enter,sizeof(enter),reply); assert(reply[3] == RS_OK);
    assert(resource_store_begin(300,4,0,1) == RS_OK);
    resource_protocol_execute(q,sizeof(q),reply); assert(reply[3] == RS_BUSY);
    resource_store_abort();
    resource_protocol_execute(q,sizeof(q)-1,reply); assert(reply[3] == RS_INVALID);
    resource_protocol_execute(q,sizeof(q),reply); assert(reply[3] == RS_OK);
    activation_result = RS_NOT_FOUND;
    uint8_t leave[] = {0x26,7,12};
    resource_protocol_execute(leave,sizeof(leave),reply);
    assert(reply[3] == RS_NOT_FOUND && !resource_protocol_entered());
    assert(activated_partitions & (1U << 1));
    activation_result = RS_OK;

    /* A power cut can leave a partial sector; reboot never reuses its cache. */
    make_table(); assert(upload(1,table,sizeof(table)) == RS_OK);
    assert(upload(2,data,sizeof(data)) == RS_OK);
    if (!setjmp(power_loss)) {
        cut = 0; prefix = 128;
        resource_store_erase(2,0);
        assert(!"power cut did not occur");
    }
    cut = -1; reboot();
    assert(resource_store_open(2,&length) == RS_NOT_FOUND);
    assert(resource_store_erase(2,0) == RS_OK);
    assert(upload(2,data,sizeof(data)) == RS_OK);

    /* Hardware errors and incomplete erase never report success. */
    erase_fail = true;
    assert(resource_store_erase(0,4096) == RS_IO); erase_fail = false;
    erase_partial = true;
    assert(resource_store_erase(0,4096) == RS_CRC); erase_partial = false;
    read_fail = 0;
    assert(resource_store_erase(0,4096) == RS_IO); read_fail = -1;

    /* Duplicate retry replays the ACK without a second erase. */
    resource_protocol_set_handler(NULL,NULL);
    process(71,true,100);
    resource_transfer_receive(enter,sizeof(enter),71); process(71,true,101);
    q[3] = q[4] = 0;
    resource_transfer_receive(q,sizeof(q),71); process(71,true,102);
    assert(reply[3] == RS_OK); before = mutations;
    resource_transfer_receive(q,sizeof(q),71); process(71,true,103);
    assert(reply[3] == RS_OK && mutations == before);
    process(72,false,104); assert(!resource_protocol_entered());

    /* Full-chip removal works without a valid table and clears every byte. */
    memset(flash,0x19,sizeof(flash)); before = mutations;
    unsigned feeds = watchdog_feeds;
    for (uint32_t offset = 0; offset < sizeof(flash); offset += 4096)
        assert(resource_store_erase(0,offset) == RS_OK);
    assert(mutations == before + 128 && watchdog_feeds > feeds);
    all_bytes(0,sizeof(flash),255);
    reboot(); assert(resource_store_mount() == RS_NOT_FOUND);
    uint8_t status[] = {0x26,0,13};
    resource_protocol_execute(status,sizeof(status),reply);
    assert(reply[3] == RS_NOT_FOUND && (reply[4] & RESOURCE_CAP_ERASE));
    make_table(); assert(upload(1,table,sizeof(table)) == RS_OK);
    assert(upload(2,data,sizeof(data)) == RS_OK);
    assert(resource_store_open(2,&length) == RS_OK && length == sizeof(data));
    puts("PASS: sector/full Flash erase, bounds, CRC, cache invalidation, protected sessions, retries and reinstall");
    return 0;
}
