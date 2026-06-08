/* 仅用于 GCC/newlib 构建的带边界 _sbrk。
 *
 * 默认 newlib(-lnosys) 的 _sbrk 只从 .bss 末尾向上增长堆，且不检查栈：
 * 一次较大的 malloc（如 GUI 帧缓冲，见 GUI.c 中按 __HEAP_SIZE 计算的 ph）
 * 会让堆悄悄长进栈区，运行期栈递归下去就会踩坏堆 -> HardFault -> 不停重启。
 *
 * 本实现把堆限制在链接脚本(nrf_common.ld)保留的 [__HeapBase, __HeapLimit) 区间内，
 * 超出即返回 ENOMEM 失败而不是越界增长——行为与 Keil/armcc 的"堆/栈各自独立固定区"一致。
 *
 * 该文件只被 Makefile.nRF52_linux 引用，不会进入 Keil 工程，故不影响 armcc 构建。
 */
#include <errno.h>
#include <sys/types.h>

/* 由 nrf_common.ld 提供 */
extern char __HeapBase;
extern char __HeapLimit;

void *_sbrk(int incr)
{
    static char *heap = 0;
    char *prev;

    if (heap == 0) {
        heap = &__HeapBase;
    }

    if (incr > 0 && (heap + incr) > &__HeapLimit) {
        errno = ENOMEM;
        return (void *)-1;
    }

    prev = heap;
    heap += incr;
    return prev;
}
