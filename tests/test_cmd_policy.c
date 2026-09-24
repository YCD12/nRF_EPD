#include <assert.h>
#include <stdio.h>

#include "epd_cmd_policy.h"

static void test_init_and_write_block_are_urgent(void)
{
    assert(epd_cmd_is_urgent(0x01)); /* INIT */
    assert(epd_cmd_is_urgent(0x31)); /* WRITE_BLOCK */
    assert(epd_cmd_is_urgent(0x32)); /* QUERY_STATUS */
    assert(epd_cmd_is_urgent(0x33)); /* RESET_TRANSFER */
    assert(epd_cmd_is_urgent(0x91)); /* SYS_RESET */
    assert(epd_cmd_is_urgent(0x92)); /* SYS_SLEEP */
    assert(epd_cmd_is_urgent(0x99)); /* CFG_ERASE */
}

static void test_refresh_and_set_time_stay_queued(void)
{
    assert(!epd_cmd_is_urgent(0x05)); /* REFRESH: blocking, must stay queued */
    assert(!epd_cmd_is_urgent(0x20)); /* SET_TIME */
    assert(!epd_cmd_is_urgent(0x22)); /* SET_MODE */
    assert(!epd_cmd_is_urgent(0x23)); /* SYNC_TIME_SILENT */
    assert(!epd_cmd_is_urgent(0x30)); /* WRITE_IMAGE: fallback, queued after INIT */
    assert(epd_cmd_is_urgent(0x24)); /* SET_ANNOTATIONS: sequential chunks, queue depth is 2 */
}

static void test_week_start_is_immediate_so_mode_switch_fits_queue(void)
{
    /* First mode click sends WEEK_START + SILENT + SET_MODE. Queue depth is 2.
     * WEEK_START must not take a slot, or SET_MODE is dropped and the screen
     * stays on the previous picture until the next click. */
    assert(epd_cmd_is_immediate(0x21));
    assert(!epd_cmd_is_urgent(0x21));
    assert(!epd_cmd_is_immediate(0x22));
    assert(!epd_cmd_is_immediate(0x23));
    assert(!epd_cmd_is_immediate(0x05));
    assert(epd_cmd_mode_switch_queued_count(true) == 2);
    assert(epd_cmd_mode_switch_queued_count(false) == 2);
}

static void test_picture_writes_need_a_live_display(void)
{
    assert(epd_cmd_needs_display(0x02)); /* CLEAR */
    assert(epd_cmd_needs_display(0x03)); /* SEND_COMMAND */
    assert(epd_cmd_needs_display(0x04)); /* SEND_DATA */
    assert(epd_cmd_needs_display(0x05)); /* REFRESH */
    assert(epd_cmd_needs_display(0x30)); /* WRITE_IMAGE */
    assert(epd_cmd_needs_display(0x31)); /* WRITE_BLOCK */
    assert(!epd_cmd_needs_display(0x20));
    assert(!epd_cmd_needs_display(0x01)); /* INIT creates the display */
    assert(!epd_cmd_needs_display(0x24)); /* SET_ANNOTATIONS writes FDS, refresh is separate */
}

static void test_write_block_accepts_only_sequential_ids(void)
{
    assert(epd_block_accept(0, 0, false) == EPD_BLOCK_WRITE);
    assert(epd_block_accept(1, 0, false) == EPD_BLOCK_REJECT);
    assert(epd_block_accept(1, 1, true) == EPD_BLOCK_WRITE);
    assert(epd_block_accept(0, 3, true) == EPD_BLOCK_WRITE);
    assert(epd_block_accept(2, 3, true) == EPD_BLOCK_DUP);
    assert(epd_block_accept(4, 3, true) == EPD_BLOCK_REJECT);
}

static void test_write_block_rejects_more_than_512_blocks(void)
{
    assert(epd_block_total_valid(1));
    assert(epd_block_total_valid(512));
    assert(!epd_block_total_valid(0));
    assert(!epd_block_total_valid(513));
    assert(!epd_block_total_valid(2400));
    /* Default web MTU=20 uses 20-byte payloads: 800x480 plane cannot fit. */
    assert(!epd_block_plane_fits(48000U, 20U));
    assert(epd_block_plane_fits(48000U, 172U));
}

static void test_sequential_status_does_not_need_a_bitmap(void)
{
    /* WRITE_BLOCK is a sequential stream; QUERY_STATUS only needs received. */
    assert(EPD_TRANSFER_BITMAP_BYTES == 0U);
    assert(epd_transfer_status_complete(204, 204));
    assert(!epd_transfer_status_complete(203, 204));
    assert(!epd_transfer_status_complete(0, 204));
    assert(!epd_transfer_status_complete(0, 0));
    assert(epd_transfer_missing_from(0, 204) == 0);
    assert(epd_transfer_missing_from(100, 204) == 100);
}

static void test_set_time_rate_limit_still_applies_valid_mode(void)
{
    epd_set_time_action_t a;

    a = epd_set_time_action(false, true, 2);
    assert(a.apply_time);
    assert(a.apply_mode);
    assert(a.mode == 2);

    a = epd_set_time_action(true, true, 3);
    assert(!a.apply_time);
    assert(a.apply_mode);
    assert(a.mode == 3);

    a = epd_set_time_action(false, true, 9);
    assert(a.apply_time);
    assert(!a.apply_mode);
}

int main(void)
{
    test_init_and_write_block_are_urgent();
    test_refresh_and_set_time_stay_queued();
    test_week_start_is_immediate_so_mode_switch_fits_queue();
    test_picture_writes_need_a_live_display();
    test_write_block_accepts_only_sequential_ids();
    test_write_block_rejects_more_than_512_blocks();
    test_sequential_status_does_not_need_a_bitmap();
    test_set_time_rate_limit_still_applies_valid_mode();
    puts("PASS: cmd policy");
    return 0;
}
