#ifndef CLOCK_TEXT_H
#define CLOCK_TEXT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline size_t clock_text_append(char *out, size_t out_sz, size_t pos, const char *s)
{
    if (out == NULL || out_sz == 0U || s == NULL) {
        return pos;
    }
    while (*s != '\0' && pos + 1U < out_sz) {
        out[pos++] = *s++;
    }
    out[(pos < out_sz) ? pos : (out_sz - 1U)] = '\0';
    return pos;
}

static inline size_t clock_text_append_u8(char *out, size_t out_sz, size_t pos, uint8_t value)
{
    if (value >= 100U) {
        pos = clock_text_append(out, out_sz, pos, "100");
        return pos;
    }
    if (value >= 10U) {
        char tens[2] = {(char)('0' + (value / 10U)), '\0'};
        pos = clock_text_append(out, out_sz, pos, tens);
    }
    {
        char ones[2] = {(char)('0' + (value % 10U)), '\0'};
        pos = clock_text_append(out, out_sz, pos, ones);
    }
    return pos;
}

static inline void clock_format_lunar_md(char *out, size_t out_sz,
                                         const char *leap, const char *month, const char *date)
{
    size_t pos = 0;

    pos = clock_text_append(out, out_sz, pos, leap);
    pos = clock_text_append(out, out_sz, pos, month);
    (void)clock_text_append(out, out_sz, pos, date);
}

static inline void clock_format_jieqi_countdown(char *line1, size_t line1_sz,
                                                char *line2, size_t line2_sz,
                                                const char *jq_name, uint8_t days)
{
    size_t pos = 0;

    if (line1 == NULL || line1_sz == 0U) {
        return;
    }
    if (days == 0U) {
        (void)clock_text_append(line1, line1_sz, 0, jq_name);
        if (line2 != NULL && line2_sz > 0U) {
            line2[0] = '\0';
        }
        return;
    }

    pos = clock_text_append(line1, line1_sz, 0, "离");
    (void)clock_text_append(line1, line1_sz, pos, jq_name);

    if (line2 == NULL || line2_sz == 0U) {
        return;
    }
    pos = clock_text_append(line2, line2_sz, 0, "还有");
    pos = clock_text_append_u8(line2, line2_sz, pos, days);
    (void)clock_text_append(line2, line2_sz, pos, "天");
}

/* Last four characters of the BLE name, or the whole name if shorter. */
static inline void clock_ssid_tail4(char *out, size_t out_sz, const char *ssid)
{
    size_t len;
    size_t n;

    if (out == NULL || out_sz == 0U) {
        return;
    }
    out[0] = '\0';
    if (ssid == NULL) {
        return;
    }
    len = strlen(ssid);
    n = (len > 4U) ? 4U : len;
    if (n + 1U > out_sz) {
        n = out_sz - 1U;
    }
    if (n > 0U) {
        memcpy(out, ssid + (len - n), n);
    }
    out[n] = '\0';
}

/* One decimal volt string, e.g. 3300 -> "3.3V". No battery-level icon. */
static inline void clock_format_voltage(char *out, size_t out_sz, uint16_t voltage_mv)
{
    uint16_t volts;
    uint16_t decivolts;
    size_t pos = 0;

    if (out == NULL || out_sz == 0U) {
        return;
    }
    volts = (uint16_t)(voltage_mv / 1000U);
    decivolts = (uint16_t)((voltage_mv % 1000U) / 100U);
    if (volts >= 10U) {
        pos = clock_text_append_u8(out, out_sz, pos, (uint8_t)volts);
    } else {
        char ones[2] = {(char)('0' + (uint8_t)volts), '\0'};
        pos = clock_text_append(out, out_sz, pos, ones);
    }
    pos = clock_text_append(out, out_sz, pos, ".");
    {
        char dec[2] = {(char)('0' + (uint8_t)decivolts), '\0'};
        pos = clock_text_append(out, out_sz, pos, dec);
    }
    (void)clock_text_append(out, out_sz, pos, "V");
}

#endif
