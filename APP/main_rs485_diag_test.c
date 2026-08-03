/**
 * @file    main_rs485_diag_test.c
 * @brief   Standalone RS485 raw byte-arrival diagnostic (Kontrx)
 *
 * PURPOSE
 * -------
 * Every existing Modbus path (blocking modbus.c AND the DMA/IDLE engine in
 * modbus_dma.c) only ever reports "timeout". A timeout answers NOTHING
 * about what actually happened on the wire:
 *      - Did zero bytes ever arrive at USART3_RX?
 *      - Did SOME bytes arrive, but fewer than a full frame?
 *      - Did a full frame arrive but get rejected later (CRC/address)?
 *
 * This file bypasses Modbus_ReadHoldingRegisters(), the IDLE-line ISR and
 * DMA entirely. It talks to USART3 with the most primitive possible
 * technique -- a tight busy-poll on SR/DR -- and counts every single byte
 * (and every USART error flag) that shows up after a request is sent, no
 * matter how incomplete the "frame" turns out to be.
 *
 * This is deliberately NOT wired into the main application. It is a
 * separate, standalone firmware image (its own CMake target,
 * "RS485_Diag") so it can be built, flashed and iterated on without
 * touching the production Modbus stack in HAL/modbus.c or
 * HAL/modbus_dma.c. Once this proves out what the bus is actually doing,
 * the fix gets ported into the real code.
 *
 * HOW TO READ THE OUTPUT
 * -----------------------
 *  Raw bytes captured: 0          -> nothing physically reaches USART3_RX.
 *                                     Look at wiring (A/B swapped or not
 *                                     connected), MAX485 DE/RE (PD3/PD2),
 *                                     sensor power, common ground, or the
 *                                     TX side actually driving the bus.
 *  Raw bytes captured: 1-8 (< expected) -> the slave IS driving the bus and
 *                                     bytes ARE reaching the MCU, but the
 *                                     frame is incomplete. Points to noise,
 *                                     a marginal baud/timing mismatch, or
 *                                     the bus glitching mid-frame -- NOT a
 *                                     dead line.
 *  Raw bytes captured: >= expected  -> a full frame physically arrived.
 *                                     Any remaining failure is in parsing
 *                                     (CRC/address/function code), not in
 *                                     the electrical layer.
 *  ORE / FE / NE counts             -> Overrun / Framing / Noise errors
 *                                     seen on the line. Non-zero FE/NE with
 *                                     zero good bytes usually means wrong
 *                                     baud rate or A/B swapped.
 */

#include "gpio_stm32.h"
#include "uart_stm32.h"
#include "stm32f407_regs.h"
#include "led.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- USART SR bit masks (kept local so this file has zero dependency on
 *      HAL/modbus_dma.c internals -- fully standalone) ---- */
#define USART_SR_PE     (1U << 0)
#define USART_SR_FE     (1U << 1)
#define USART_SR_NE     (1U << 2)
#define USART_SR_ORE    (1U << 3)
#define USART_SR_IDLE   (1U << 4)
#define USART_SR_RXNE   (1U << 5)
#define USART_SR_TC     (1U << 6)
#define USART_SR_TXE    (1U << 7)

/* ---- printf() / scanf() routed through USART6 debug port, exactly like
 *      the other standalone sensor tests in this repo (main_ammonia_test.c
 *      etc.), so behaviour/pinout is consistent across all test images. */
int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        UART_Debug_SendByte((uint8_t)ptr[i]);
    }
    return len;
}

int _read(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        ptr[i] = (char)UART_Debug_ReceiveByte();
        UART_Debug_SendByte((uint8_t)ptr[i]);
        if (ptr[i] == '\r') {
            UART_Debug_SendByte('\n');
            ptr[i] = '\n';
            return i + 1;
        } else if (ptr[i] == '\n') {
            return i + 1;
        }
    }
    return len;
}

static void Delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 3200; j++) {
            __asm__("nop");
        }
    }
}

/* ---- Standard Modbus RTU CRC16 (identical algorithm to HAL/modbus.c, kept
 *      local so this file has no dependency on HAL/modbus.c) ---- */
static uint16_t Modbus_CRC16(const uint8_t *buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)buf[pos];
        for (uint8_t i = 8; i != 0; i--) {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else              { crc >>= 1; }
        }
    }
    return crc;
}

/* Sends a raw "Read Holding Registers" (FC 0x03) request, using the exact
 * same DE/RE + timing sequence as the production code (4000-iteration
 * 3.5-char silence, back-to-back bytes, wait TC, small settle delay before
 * dropping DE). Kept identical on purpose -- we want to test the SAME
 * physical transmission behaviour, only the RX side is replaced with a
 * raw diagnostic capture instead of the existing frame-parsing logic.
 *
 * IMPORTANT: UART_WaitTransmissionComplete() in uart_stm32.c has NO
 * timeout -- "while (!(SR & TC));" forever. If TX never physically
 * completes (e.g. USART3 clock/GPIO AF issue, MAX485 wiring fault) that
 * call hangs the whole test with zero output, which is indistinguishable
 * from "still capturing" to whoever is watching the terminal. So here we
 * poll TC ourselves with a hard-bounded timeout and report explicitly if
 * it never set, instead of calling the unbounded shared helper. Returns
 * 1 if TC was seen (frame really left the UART), 0 if it timed out. */
static uint8_t Send_Modbus_Request(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs) {
    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = 0x03;
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(num_regs >> 8);
    frame[5] = (uint8_t)(num_regs & 0xFF);
    uint16_t crc = Modbus_CRC16(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF);
    frame[7] = (uint8_t)(crc >> 8);

    for (volatile uint32_t d = 0; d < 4000; d++);   /* 3.5-char silence   */

    MAX485_Transmit_Enable();
    for (volatile uint32_t d = 0; d < 10; d++);     /* DE turn-on settle  */

    for (uint8_t i = 0; i < 8; i++) {
        UART_SendByte(frame[i]);
    }

    uint8_t tc_ok = 0;
    for (uint32_t d = 0; d < 500000UL; d++) {
        if (USART3->SR & USART_SR_TC) { tc_ok = 1; break; }
    }

    for (volatile uint32_t d = 0; d < 5; d++);      /* STOP-bit settle    */

    MAX485_Receive_Enable();
    return tc_ok;
}

#define DIAG_MAX_BYTES  64

typedef struct {
    uint16_t byte_count;
    uint16_t ore_count;
    uint16_t fe_count;
    uint16_t ne_count;
    uint8_t  bytes[DIAG_MAX_BYTES];
    uint32_t loop_iterations_used;
} RS485_DiagResult_t;

/* Raw busy-poll capture. Deliberately bypasses the IDLE-line ISR / DMA
 * entirely and reads SR/DR directly, byte by byte, for a generous window.
 * Stops early once at least one byte has arrived and the line has then
 * gone quiet for SILENCE_LIMIT iterations (end of frame), or after
 * HARD_CAP iterations total regardless of activity (absolute ceiling so a
 * permanently dead line does not hang the test forever). Every byte AND
 * every error flag is counted -- this is the direct, objective answer to
 * "did anything ever physically arrive". */
static void RS485_RawCapture(RS485_DiagResult_t *r) {
    memset(r, 0, sizeof(*r));

    const uint32_t SILENCE_LIMIT = 100000UL;   /* quiet gap => frame ended   */
    const uint32_t HARD_CAP      = 3000000UL;  /* absolute ceiling per call  */
    /* NOTE ON TIMING: this is a busy-poll on a -O0 build with no calibrated
     * clock reference, so these are loop ITERATIONS, not milliseconds.
     * 3,000,000 iterations is already a very generous window for a 9600
     * baud Modbus reply (whole frame is ~9ms on the wire, typical sensor
     * turnaround is a few ms to ~100ms) -- if genuinely nothing arrives,
     * this now returns in roughly 1-3 seconds instead of the ~20-30s the
     * old 20,000,000 ceiling caused, which was indistinguishable from a
     * real hang on the terminal. */

    uint32_t silence = 0;
    uint32_t total_iter = 0;

    while (total_iter < HARD_CAP) {
        total_iter++;
        uint32_t sr = USART3->SR;

        if (sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE)) {
            if (sr & USART_SR_ORE) r->ore_count++;
            if (sr & USART_SR_NE)  r->ne_count++;
            if (sr & USART_SR_FE)  r->fe_count++;
            /* Reading SR then DR is the documented sequence to clear
             * ORE/FE/NE (and RXNE, if it happened to be set alongside). */
            volatile uint32_t dummy = USART3->DR;
            (void)dummy;
            silence = 0;
            continue;
        }

        if (sr & USART_SR_RXNE) {
            uint8_t b = (uint8_t)(USART3->DR & 0xFF);
            if (r->byte_count < DIAG_MAX_BYTES) {
                r->bytes[r->byte_count] = b;
            }
            r->byte_count++;
            silence = 0;
            continue;
        }

        silence++;
        if (r->byte_count > 0 && silence > SILENCE_LIMIT) {
            break;
        }
    }

    r->loop_iterations_used = total_iter;
}

typedef struct {
    uint8_t     id;
    const char *label;
} SensorTarget_t;

/* Every sensor slave ID used by the real firmware (see modbus_dma.c
 * Modbus_DMA_PollSensors / default IDs), so this one diagnostic image can
 * tell you which specific slave(s) are silent vs. which are alive. */
static const SensorTarget_t targets[] = {
    { 1,  "pH"        },
    { 2,  "ORP"       },
    { 3,  "EC"        },
    { 5,  "Ammonia"   },
    { 10, "Ultrasonic"},
};
#define NUM_TARGETS (sizeof(targets) / sizeof(targets[0]))

int main(void) {
    GPIO_Init_USART3_Pins();    /* Modbus USART (PB10 TX / PB11 RX)        */
    GPIO_Init_MAX485_Pins();    /* MAX485 DE=PD3 / RE#=PD2, starts in RX   */
    GPIO_Init_USART1_Pins();    /* kept for parity with other test images  */
    UART_Init();                /* USART3 @ 9600 8N1                       */
    UART_Debug_Init();          /* USART6 debug console                    */
    LED_Init();

    printf("\r\n\r\n=== RS485 RAW BYTE-ARRIVAL DIAGNOSTIC ===\r\n");
    printf("Standalone image. Does NOT use HAL/modbus.c or HAL/modbus_dma.c.\r\n");
    printf("Question this answers: does ANY byte physically reach USART3_RX,\r\n");
    printf("even when the full frame never completes?\r\n");
    printf("Waiting 2s for sensors to boot...\r\n\r\n");
    Delay_ms(2000);

    uint32_t cycle = 0;
    uint32_t total_bytes_ever = 0;
    uint32_t cycles_with_zero_bytes = 0;
    uint32_t cycles_with_bytes = 0;

    while (1) {
        for (uint8_t t = 0; t < NUM_TARGETS; t++) {
            cycle++;

            /* Clear any stale error/data flags left over from before this
             * request, so we don't misattribute old noise to this cycle. */
            { volatile uint32_t s = USART3->SR; volatile uint32_t d = USART3->DR; (void)s; (void)d; }

            uint16_t num_regs = 2;
            uint8_t  expected_len = 3 + (num_regs * 2) + 2; /* addr+fc+bc+data+crc = 9 */

            /* Printed BEFORE the blocking calls below, and flushed via
             * UART_Debug_SendByte() directly (not buffered), so the
             * terminal shows visible progress every ~1s instead of long
             * silent gaps that look like a freeze. */
            printf("[%lu] Sending to slave 0x%02X (%s)...\r\n",
                   (unsigned long)(cycle), targets[t].id, targets[t].label);

            uint8_t tx_ok = Send_Modbus_Request(targets[t].id, 0x0000, num_regs);
            if (!tx_ok) {
                printf("!!! TX NEVER COMPLETED (TC flag never set) -- USART3 TX path itself\r\n");
                printf("    is stuck. Check GPIO_Init_USART3_Pins()/UART_Init(), PB10 wiring,\r\n");
                printf("    and that USART3 clock is actually enabled. Skipping RX capture.\r\n\r\n");
                LED_Toggle();
                Delay_ms(500);
                continue;
            }

            RS485_DiagResult_t r;
            RS485_RawCapture(&r);

            total_bytes_ever += r.byte_count;
            if (r.byte_count == 0) cycles_with_zero_bytes++;
            else                   cycles_with_bytes++;

            printf("--- Cycle %lu | Slave 0x%02X (%s) ---\r\n",
                   (unsigned long)cycle, targets[t].id, targets[t].label);
            printf("Raw bytes captured: %u (expected %u for a full reply)\r\n",
                   r.byte_count, expected_len);

            if (r.byte_count > 0) {
                printf("Hex: ");
                uint16_t show = r.byte_count < DIAG_MAX_BYTES ? r.byte_count : DIAG_MAX_BYTES;
                for (uint16_t i = 0; i < show; i++) {
                    printf("%02X ", r.bytes[i]);
                }
                printf("\r\n");
            }
            printf("Errors -> ORE:%u  FE:%u  NE:%u | loop iters used: %lu\r\n",
                   r.ore_count, r.fe_count, r.ne_count,
                   (unsigned long)r.loop_iterations_used);

            if (r.byte_count == 0) {
                printf("VERDICT: NOTHING arrived. Check A/B wiring (swapped?), MAX485\r\n");
                printf("         DE/RE (PD3/PD2), sensor power, common ground, termination.\r\n");
            } else if (r.byte_count < expected_len) {
                printf("VERDICT: PARTIAL frame -- bytes ARE reaching the MCU, but the\r\n");
                printf("         frame cuts short. Not a dead line -- look at timing/noise.\r\n");
            } else {
                printf("VERDICT: Full-length byte count arrived physically. Any remaining\r\n");
                printf("         failure is in CRC/address parsing, not the electrical link.\r\n");
            }

            printf("Running totals: %lu cycles | %lu with ZERO bytes | %lu with >=1 byte | %lu bytes total\r\n\r\n",
                   (unsigned long)cycle, (unsigned long)cycles_with_zero_bytes,
                   (unsigned long)cycles_with_bytes, (unsigned long)total_bytes_ever);

            LED_Toggle();
            Delay_ms(500);
        }
    }

    return 0;
}
