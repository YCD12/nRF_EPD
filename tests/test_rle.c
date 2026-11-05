/* EPD 传图块 RLE 解码器主机测试。
 *
 * 编码器镜像网页 html/js/rle.js 的格式（repeat 0x80|(count-3)+value；
 * literal (count-1)+bytes），用于生成随机数据做往返校验；同时用固定字节
 * 向量锁定固件解码器与网页编码器的格式契约。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "EPD/service/epd_rle.h"

/* ---- 测试用镜像编码器（仅测试，格式必须与 html/js/rle.js 一致） ---- */
static uint16_t encode(const uint8_t *in, uint16_t len, uint8_t *out, uint16_t cap)
{
    uint16_t si = 0U, di = 0U;
    while (si < len) {
        uint16_t run = 1U;
        while ((uint16_t)(si + run) < len && run < 130U && in[si + run] == in[si]) run++;

        if (run >= 3U) {
            if ((uint32_t)di + 2U > cap) return 0U;
            out[di++] = (uint8_t)(0x80U | (run - 3U));
            out[di++] = in[si];
            si = (uint16_t)(si + run);
        } else {
            uint16_t start = si;
            uint16_t lit = 0U;
            while (si < len && lit < 128U) {
                if ((uint16_t)(si + 2U) < len && in[si] == in[si + 1U] &&
                    in[si] == in[si + 2U])
                    break;
                lit++;
                si++;
            }
            if (lit == 0U) return 0U;
            if ((uint32_t)di + 1U + lit > cap) return 0U;
            out[di++] = (uint8_t)(lit - 1U);
            for (uint16_t i = 0U; i < lit; i++) out[di++] = in[start + i];
        }
    }
    return di;
}

static void expect_decode(const uint8_t *src, uint16_t src_len, const uint8_t *want,
                          uint16_t want_len)
{
    uint8_t out[EPD_RLE_MAX_BLOCK];
    uint16_t out_len = 0xFFFFU;
    bool ok = epd_rle_decompress(src, src_len, out, sizeof(out), &out_len);
    assert(ok);
    assert(out_len == want_len);
    assert(memcmp(out, want, want_len) == 0);
}

static void test_fixed_vectors(void)
{
    /* repeat 0x82 -> count 5，value 0xFF */
    static const uint8_t rep[] = {0x82, 0xFF};
    static const uint8_t rep_want[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    expect_decode(rep, sizeof(rep), rep_want, sizeof(rep_want));

    /* repeat 0xFF -> count 130 */
    static const uint8_t rep130[2] = {0xFF, 0x00};
    uint8_t want130[130];
    memset(want130, 0x00, sizeof(want130));
    expect_decode(rep130, sizeof(rep130), want130, sizeof(want130));

    /* literal 'ABC'，随后 repeat 2 个 0x00（run 2 走 literal） */
    static const uint8_t mixed[] = {0x02, 'A', 'B', 'C', 0x01, 0x00, 0x00};
    static const uint8_t mixed_want[] = {'A', 'B', 'C', 0x00, 0x00};
    expect_decode(mixed, sizeof(mixed), mixed_want, sizeof(mixed_want));

    /* 空输入 -> 空输出 */
    uint8_t out[4];
    uint16_t out_len = 0xFFFFU;
    assert(epd_rle_decompress(NULL, 0U, out, sizeof(out), &out_len));
    assert(out_len == 0U);

    /* 边界：130 连 + 1 连 = 131 个相同字节（literal 1 的 control 是 0x00） */
    static const uint8_t split[] = {0xFF, 0x55, 0x00, 0x55};
    uint8_t want131[131];
    memset(want131, 0x55, sizeof(want131));
    expect_decode(split, sizeof(split), want131, sizeof(want131));
}

static void test_malformed(void)
{
    uint8_t out[8];
    uint16_t out_len;

    /* repeat 缺 value */
    static const uint8_t trunc_rep[] = {0x82};
    assert(!epd_rle_decompress(trunc_rep, sizeof(trunc_rep), out, sizeof(out), &out_len));

    /* literal 越界 */
    static const uint8_t trunc_lit[] = {0x02, 'A'};
    assert(!epd_rle_decompress(trunc_lit, sizeof(trunc_lit), out, sizeof(out), &out_len));

    /* 输出容量不足：repeat 5 只给 4 字节 */
    static const uint8_t rep5[] = {0x82, 0xFF};
    assert(!epd_rle_decompress(rep5, sizeof(rep5), out, 4U, &out_len));

    /* NULL 参数 */
    assert(!epd_rle_decompress(trunc_rep, sizeof(trunc_rep), NULL, sizeof(out), &out_len));
    assert(!epd_rle_decompress(trunc_rep, sizeof(trunc_rep), out, sizeof(out), NULL));
}

static void test_roundtrip_deterministic(void)
{
    static uint8_t plain[512];
    static uint8_t coded[1024];
    static uint8_t back[512];

    /* 固定伪随机序列 + 周期性长 run，覆盖 literal/repeat 混合。 */
    uint32_t seed = 0x12345678UL;
    for (uint16_t len = 0U; len <= 512U; len += 17U) {
        for (uint16_t i = 0U; i < len; i++) {
            seed = seed * 1664525UL + 1013904223UL;
            plain[i] = (i % 97U) < 40U ? 0x5AU : (uint8_t)(seed >> 24);
        }
        uint16_t coded_len = encode(plain, len, coded, sizeof(coded));
        assert(coded_len != 0U || len == 0U);
        uint16_t back_len = 0xFFFFU;
        assert(epd_rle_decompress(coded, coded_len, back, sizeof(back), &back_len));
        assert(back_len == len);
        assert(memcmp(back, plain, len) == 0);
    }
}

static void test_compress_ratio(void)
{
    uint8_t plain[EPD_RLE_MAX_BLOCK];
    uint8_t coded[32];
    memset(plain, 0xFF, sizeof(plain));
    uint16_t n = encode(plain, (uint16_t)sizeof(plain), coded, sizeof(coded));
    assert(n == 4U); /* 236B 平色块 = run130 + run106 -> 4B（run 上限 130） */

    uint8_t back[EPD_RLE_MAX_BLOCK];
    uint16_t back_len = 0U;
    assert(epd_rle_decompress(coded, n, back, sizeof(back), &back_len));
    assert(back_len == sizeof(plain));
    assert(memcmp(back, plain, sizeof(plain)) == 0);
}

int main(void)
{
    test_fixed_vectors();
    test_malformed();
    test_roundtrip_deterministic();
    test_compress_ratio();
    printf("PASS: EPD RLE block codec (format contract, malformed input, roundtrip)\n");
    return 0;
}
