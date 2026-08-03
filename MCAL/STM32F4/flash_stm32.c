#include "flash_stm32.h"
#include "stm32f407_regs.h"

static void FLASH_WaitBusy(void) {
    while (FLASH_CTRL->SR & FLASH_SR_BSY);
}

void FLASH_Unlock(void) {
    if (FLASH_CTRL->CR & FLASH_CR_LOCK) {
        FLASH_CTRL->KEYR = FLASH_KEY1;
        FLASH_CTRL->KEYR = FLASH_KEY2;
    }
}

void FLASH_Lock(void) {
    FLASH_CTRL->CR |= FLASH_CR_LOCK;
}

void FLASH_EraseSector(uint8_t sector) {
    /* Only hold interrupts during register setup (microseconds) */
    __asm volatile ("cpsid i" : : : "memory");

    FLASH_WaitBusy();
    FLASH_Unlock();

    /* Clear any pending error flags in Status Register */
    FLASH_CTRL->SR = 0xF3U;

    /* Clear sector selection and set new sector number */
    FLASH_CTRL->CR &= ~(0xFU << FLASH_CR_SNB_POS);
    FLASH_CTRL->CR |= (sector << FLASH_CR_SNB_POS) | FLASH_CR_SER | FLASH_CR_PSIZE32;
    FLASH_CTRL->CR |= FLASH_CR_STRT;

    /* Re-enable interrupts during the long busy-wait (50-100ms)
     * so W5500 RX buffer can be serviced by other tasks. */
    __asm volatile ("cpsie i" : : : "memory");

    FLASH_WaitBusy();

    __asm volatile ("cpsid i" : : : "memory");
    FLASH_CTRL->CR &= ~FLASH_CR_SER;
    FLASH_Lock();
    __asm volatile ("cpsie i" : : : "memory");
}

void FLASH_WriteWord(uint32_t address, uint32_t data) {
    /* Disable interrupts during flash write */
    __asm volatile ("cpsid i" : : : "memory");

    FLASH_WaitBusy();
    FLASH_Unlock();

    /* Clear any pending error flags in Status Register */
    FLASH_CTRL->SR = 0xF3U;

    /* Set programming size to 32-bit and enable PG */
    FLASH_CTRL->CR &= ~(3U << 8);
    FLASH_CTRL->CR |= FLASH_CR_PSIZE32 | FLASH_CR_PG;

    /* Write data word */
    *(__IO uint32_t *)address = data;

    FLASH_WaitBusy();

    /* Disable PG */
    FLASH_CTRL->CR &= ~FLASH_CR_PG;
    FLASH_Lock();

    /* Enable interrupts again */
    __asm volatile ("cpsie i" : : : "memory");
}

void FLASH_WriteBuffer(uint32_t start_addr, const uint8_t *data, uint32_t len) {
    uint32_t addr = start_addr;
    uint32_t i = 0;

    while (i < len) {
        uint32_t word = 0xFFFFFFFFU;
        uint8_t bytes_to_copy = (len - i >= 4) ? 4 : (len - i);
        
        for (uint8_t b = 0; b < bytes_to_copy; b++) {
            ((uint8_t *)&word)[b] = data[i + b];
        }

        FLASH_WriteWord(addr, word);
        addr += 4;
        i += bytes_to_copy;
    }
}
