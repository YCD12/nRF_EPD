/* 轻量级 RTT 故障诊断（仅 GCC 构建引用，不进入 Keil 工程）。
 *
 * 用途: 在不引入完整 NRF_LOG(~20KB flash，装不下 Bootloader)的前提下，
 *       把"为什么不停重启"的关键信息直接用已链接的 SEGGER_RTT 打出来:
 *         - 启动时的复位原因 RESETREAS（DOG=看门狗 等）
 *         - APP_ERROR_CHECK 失败的 文件:行号:错误码
 *         - HardFault 的 PC/LR/xPSR 以及 CFSR/HFSR 故障状态寄存器
 *       并且发生故障后 **停在原地循环**（不复位），方便 RTTView 读取。
 *
 * 用 J-Link RTT Viewer / RTTView 连接即可看到 [DIAG] 开头的行。
 * 排查完删掉本文件在 Makefile.nRF52_linux 中的引用即可。
 */
#include "nrf.h"
#include "app_error.h"
#include "SEGGER_RTT.h"
#include <stdint.h>

/* 启动期(main 之前)打印复位原因，此时 RESETREAS 尚未被 main 清除 */
__attribute__((constructor))
static void diag_boot_banner(void)
{
    SEGGER_RTT_printf(0, "\n[DIAG] ==== boot, RESETREAS=0x%08X ====\n",
                      (unsigned)NRF_POWER->RESETREAS);
}

/* 覆盖 SDK 的弱定义: 所有 APP_ERROR_CHECK / ASSERT 失败都会走到这里 */
void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info)
{
    SEGGER_RTT_printf(0, "[DIAG] FAULT id=%u pc=0x%08X info=0x%08X\n",
                      (unsigned)id, (unsigned)pc, (unsigned)info);

    if (id == NRF_FAULT_ID_SDK_ERROR && info != 0) {
        error_info_t *e = (error_info_t *)info;
        SEGGER_RTT_printf(0, "[DIAG]   APP_ERROR err=0x%08X line=%u file=%s\n",
                          (unsigned)e->err_code, (unsigned)e->line_num,
                          (const char *)e->p_file_name);
    } else if (id == NRF_FAULT_ID_SDK_ASSERT && info != 0) {
        assert_info_t *a = (assert_info_t *)info;
        SEGGER_RTT_printf(0, "[DIAG]   ASSERT line=%u file=%s\n",
                          (unsigned)a->line_num, (const char *)a->p_file_name);
    }

    __disable_irq();
    for (;;) { /* 停住，便于读取 RTT；不复位 */ }
}

/* 真正的转储函数: sp 指向异常压栈帧 [r0,r1,r2,r3,r12,lr,pc,xpsr] */
void hardfault_dump(uint32_t *sp)
{
    SEGGER_RTT_printf(0, "[DIAG] ==== HARDFAULT ====\n");
    SEGGER_RTT_printf(0, "[DIAG]   PC =0x%08X  LR =0x%08X  xPSR=0x%08X\n",
                      (unsigned)sp[6], (unsigned)sp[5], (unsigned)sp[7]);
    SEGGER_RTT_printf(0, "[DIAG]   R0 =0x%08X  R1 =0x%08X  R2 =0x%08X  R3=0x%08X\n",
                      (unsigned)sp[0], (unsigned)sp[1], (unsigned)sp[2], (unsigned)sp[3]);
    SEGGER_RTT_printf(0, "[DIAG]   CFSR=0x%08X HFSR=0x%08X\n",
                      (unsigned)SCB->CFSR, (unsigned)SCB->HFSR);
    __disable_irq();
    for (;;) { }
}

/* naked: 取出故障前使用的栈指针(MSP/PSP)再交给 C 函数转储 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4            \n"
        "ite eq                \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "ldr r1, =hardfault_dump\n"
        "bx r1                 \n"
    );
}
