"""Execute the production SAADC routine with MMIO fault and IRQ injection.

This checks control flow and cleanup, not electrical ADC accuracy/timing.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "EPD/EPD_driver.c").read_text(encoding="utf-8")
start = source.index("static bool epd_adc_wait(")
end = source.index("\n}\n", source.index("uint16_t EPD_ReadVoltage(void)")) + 3
routine = source[start:end].replace("(uint32_t)&value", "(uintptr_t)&value")
constants = sorted(set(re.findall(r"\bSAADC_\w+", routine)))
definitions = "\n".join(
    f"#define {name} {1 if name == 'SAADC_ENABLE_ENABLE_Enabled' else 0}"
    for name in constants
)
fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
uint16_t EPD_ReadVoltage(void);
static bool inject_irq;
static unsigned irq, delay_us, accesses, fault;
static uint16_t irq_voltage;
struct SimSAADC {
    uint32_t RESOLUTION, ENABLE;
    struct { uint32_t CONFIG, PSELN, PSELP; } CH[1];
    struct { uintptr_t PTR; uint32_t MAXCNT; } RESULT;
    uint32_t TASKS_START, TASKS_SAMPLE, TASKS_STOP;
    uint32_t EVENTS_STARTED, EVENTS_END, EVENTS_STOPPED;
} chip;
#define __get_IPSR() irq
#define NRF_SAADC sim_adc_access()
#define NRF_LOG_DEBUG(...) ((void)0)
static struct SimSAADC *sim_adc_access(void) {
    accesses++;
    if (chip.ENABLE && chip.TASKS_START) {
        chip.TASKS_START = 0;
        if (fault != 1U) chip.EVENTS_STARTED = 1;
        if (inject_irq) {
            inject_irq = false;
            unsigned before = accesses;
            irq = 1;
            irq_voltage = EPD_ReadVoltage();
            irq = 0;
            assert(accesses == before); /* IRQ cannot touch active EasyDMA. */
        }
    }
    if (chip.ENABLE && chip.TASKS_SAMPLE) {
        chip.TASKS_SAMPLE = 0;
        if (fault != 2U) {
            *(volatile int16_t *)chip.RESULT.PTR = 790;
            chip.EVENTS_END = 1;
        }
    }
    if (chip.ENABLE && chip.TASKS_STOP) {
        chip.TASKS_STOP = 0;
        if (fault != 3U) chip.EVENTS_STOPPED = 1;
    }
    return &chip;
}
static void nrf_delay_us(unsigned us) {
    delay_us += us;
    assert(delay_us <= 15000U);
    (void)sim_adc_access();
}
/* PRODUCTION */
int main(void) {
    assert(EPD_ReadVoltage() == 2777U);
    inject_irq = true;
    assert(EPD_ReadVoltage() == 2777U && irq_voltage == 0U);
    for (fault = 1U; fault <= 3U; fault++) {
        delay_us = 0;
        /* Old events must not turn a broken conversion into a valid sample. */
        chip.EVENTS_STARTED = chip.EVENTS_END = chip.EVENTS_STOPPED = 1U;
        assert(EPD_ReadVoltage() == 0U && !chip.ENABLE);
        assert(delay_us >= 5000U && delay_us <= 15000U);
        assert(!chip.EVENTS_STARTED && !chip.EVENTS_END && !chip.EVENTS_STOPPED);
        unsigned saved_fault = fault;
        fault = 0;
        delay_us = 0;
        assert(EPD_ReadVoltage() == 2777U);
        fault = saved_fault;
    }
    puts("PASS: SAADC rejects IRQ reentry; START/END/STOP timeouts clean up and recover");
}
"""
out = Path(os.environ.get("HOST_TEST_OUT", ROOT / "_build/host_tests"))
out.mkdir(parents=True, exist_ok=True)
generated = out / "adc_voltage_generated.c"
generated.write_text(fixture.replace("/* PRODUCTION */", definitions + "\n" + routine), encoding="utf-8")
exe = out / ("test_adc_voltage.exe" if os.name == "nt" else "test_adc_voltage")
subprocess.run(shlex.split(os.environ.get("HOST_CC", "gcc")) +
               ["-Wall", "-Werror", "-std=c11", str(generated), "-o", str(exe)],
               cwd=ROOT, check=True)
subprocess.run([str(exe.resolve())], check=True)
