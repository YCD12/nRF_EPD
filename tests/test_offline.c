#include <assert.h>
#include <stdio.h>
#include "epd_offline.h"

int main(void)
{
    epd_offline_t s = {0};
    epd_offline_queued(&s, false);
    s.state = EPD_OFFLINE_ACK;
    epd_offline_completed(&s, 1);
    assert(s.state == EPD_OFFLINE_ACK); /* Queue-full final ACK must be retried. */
    epd_offline_disconnected(&s);
    assert(s.state == EPD_ONLINE); /* No accepted final ACK: remain repairable. */

    s.state = EPD_OFFLINE_ACK;
    epd_offline_queued(&s, false);
    epd_offline_queued(&s, true);
    epd_offline_completed(&s, 1);
    assert(s.state == EPD_OFFLINE_DRAIN && s.notifications == 1);
    epd_offline_completed(&s, 1);
    assert(s.state == EPD_OFFLINE_READY);
    s.state = EPD_OFFLINE_ACTIVE;
    epd_offline_disconnected(&s);
    assert(s.state == EPD_OFFLINE_ACTIVE);

    s.state = EPD_OFFLINE_DRAIN;
    epd_offline_disconnected(&s);
    assert(s.state == EPD_OFFLINE_READY && s.notifications == 0);
    epd_offline_completed(&s, 5); /* Other service notifications cannot underflow. */
    assert(s.notifications == 0);
    puts("PASS: offline ACK backpressure, TX completion and disconnect ordering");
}
