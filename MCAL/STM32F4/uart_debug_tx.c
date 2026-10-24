/**
 * @file  uart_debug_tx.c
 * @brief Interrupt-driven, non-blocking debug output on USART6 (RTOS build).
 *        Kept apart from uart_stm32.c so the bootloader and test images do
 *        not link the ISR or the ring buffer.
 */
#include "uart_stm32.h"
#include "stm32f407_regs.h"

/* Debug TX ring. At 9600 baud each character takes ~1 ms on the wire, so a
 * polled printf of one log line stalled the calling task for tens of ms.
 * Producers (any task) copy into the ring with IRQs masked for the copy only;
 * the USART6 TXE interrupt is the single consumer. A full ring drops the
 * excess: losing debug text is preferable to stalling a control task. */
#define DBG_TX_RING_SIZE  1024U          /* power of two */
#define DBG_TX_RING_MASK  (DBG_TX_RING_SIZE - 1U)
#define USART6_IRQN       71U
#define USART_CR1_TXEIE   (1U << 7)

static volatile uint8_t  s_dbg_tx_ring[DBG_TX_RING_SIZE] __attribute__((section(".ccmram")));
static volatile uint32_t s_dbg_tx_head;  /* next free slot, producers only */
static volatile uint32_t s_dbg_tx_tail;  /* next byte to send, consumer only */
static uint8_t           s_dbg_tx_irq_ready;
volatile uint32_t        g_dbg_tx_dropped;

static inline uint32_t Dbg_IrqSave(void) {
    uint32_t primask;
    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (primask) :: "memory");
    return primask;
}

static inline void Dbg_IrqRestore(uint32_t primask) {
    __asm volatile ("msr primask, %0" :: "r" (primask) : "memory");
}

uint32_t UART_Debug_WriteAsync(const uint8_t *data, uint32_t len) {
    if (!data || len == 0U) return 0U;

    uint32_t primask = Dbg_IrqSave();
    if (!s_dbg_tx_irq_ready) {
        NVIC_IPR[USART6_IRQN] = (uint8_t)(15U << 4);  /* lowest urgency; ISR makes no RTOS calls */
        NVIC_ISER[USART6_IRQN / 32U] = (1U << (USART6_IRQN % 32U));
        s_dbg_tx_irq_ready = 1U;
    }
    uint32_t used = s_dbg_tx_head - s_dbg_tx_tail;
    uint32_t room = DBG_TX_RING_SIZE - used;
    uint32_t n = (len < room) ? len : room;
    uint32_t head = s_dbg_tx_head;
    for (uint32_t i = 0; i < n; i++) {
        s_dbg_tx_ring[(head + i) & DBG_TX_RING_MASK] = data[i];
    }
    s_dbg_tx_head = head + n;
    if (n < len) g_dbg_tx_dropped += (len - n);
    if (n > 0U) USART6->CR1 |= USART_CR1_TXEIE;
    Dbg_IrqRestore(primask);
    return n;
}

void UART_Debug_WriteBlocking(const uint8_t *data, uint32_t len) {
    /* Stop the ISR, then send what it had queued so output stays in order. */
    USART6->CR1 &= ~USART_CR1_TXEIE;
    while (s_dbg_tx_tail != s_dbg_tx_head) {
        UART_Debug_SendByte(s_dbg_tx_ring[s_dbg_tx_tail & DBG_TX_RING_MASK]);
        s_dbg_tx_tail++;
    }
    for (uint32_t i = 0; data && i < len; i++) {
        UART_Debug_SendByte(data[i]);
    }
}

void USART6_IRQHandler(void) {
    if ((USART6->CR1 & USART_CR1_TXEIE) && (USART6->SR & (1U << 7))) {
        if (s_dbg_tx_tail != s_dbg_tx_head) {
            USART6->DR = s_dbg_tx_ring[s_dbg_tx_tail & DBG_TX_RING_MASK];
            s_dbg_tx_tail++;
        } else {
            USART6->CR1 &= ~USART_CR1_TXEIE;
        }
    }
}
