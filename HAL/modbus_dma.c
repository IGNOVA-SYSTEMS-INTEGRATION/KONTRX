/**
 * @file    modbus_dma.c
 * @brief   Kontrx — Non-blocking Modbus RTU DMA Engine
 *
 * Hardware mapping:
 *   USART3  TX  = PB10 (AF7),  RX  = PB11 (AF7)
 *   MAX485  DE  = PD3,         RE# = PD2
 *   DMA1    USART3_RX = Stream 1 Channel 4
 *   DMA1    USART3_TX = Stream 3 Channel 4
 *
 * Strategy:
 *   - TX: CPU loads frame into DMA buffer; DMA fires autonomously; TC-ISR
 *         flips MAX485 back to RX and gives the dma_tx_done semaphore.
 *   - RX: DMA runs in circular / normal mode; USART3 IDLE-line IRQ fires
 *         at end of every Modbus frame; ISR gives dma_rx_done semaphore
 *         and captures byte count via NDTR.
 *   - Polling task sleeps on a semaphore until RX is complete — NO busy
 *     loops, zero wasted CPU cycles.
 *
 * @note    All stm32f407_regs.h definitions used here.  DMA register structs
 *          are defined inline to avoid HAL dependencies.
 */

#include "modbus_dma.h"
#include "stm32f407_regs.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>

/* ======================================================================
 *  DMA1 Register Definitions (STM32F407)
 * ====================================================================== */
#define DMA1_BASE       (AHB1PERIPH_BASE + 0x6000U)
#define DMA2_BASE       (AHB1PERIPH_BASE + 0x6400U)

typedef struct {
    volatile uint32_t CR;     /* 0x00 Stream config register   */
    volatile uint32_t NDTR;   /* 0x04 Number of data register  */
    volatile uint32_t PAR;    /* 0x08 Peripheral address       */
    volatile uint32_t M0AR;   /* 0x0C Memory 0 address         */
    volatile uint32_t M1AR;   /* 0x10 Memory 1 address         */
    volatile uint32_t FCR;    /* 0x14 FIFO control register    */
} DMA_Stream_TypeDef;

typedef struct {
    volatile uint32_t LISR;   /* 0x00 Low interrupt status  */
    volatile uint32_t HISR;   /* 0x04 High interrupt status */
    volatile uint32_t LIFCR;  /* 0x08 Low interrupt flag clear  */
    volatile uint32_t HIFCR;  /* 0x0C High interrupt flag clear */
    DMA_Stream_TypeDef Stream[8];
} DMA_TypeDef;

#define DMA1    ((DMA_TypeDef *)DMA1_BASE)
#define DMA2    ((DMA_TypeDef *)DMA2_BASE)

/* Stream register bit positions */
#define DMA_CR_EN       (1U << 0)
#define DMA_CR_TCIE     (1U << 4)   /* Transfer complete interrupt enable */
#define DMA_CR_HTIE     (1U << 3)
#define DMA_CR_TEIE     (1U << 2)
#define DMA_CR_DIR_P2M  (0U << 6)
#define DMA_CR_DIR_M2P  (1U << 6)
#define DMA_CR_MINC     (1U << 10)  /* Memory increment */
#define DMA_CR_MSIZE8   (0U << 13)
#define DMA_CR_PSIZE8   (0U << 11)
#define DMA_CR_CHSEL(n) ((n) << 25)

/* DMA1 LISR / LIFCR flags for Stream 1 (bits 6..11) and Stream 3 (bits 22..27) */
#define DMA1_S1_TCIF    (1U << 11)  /* LISR Stream 1 TC flag */
#define DMA1_S1_CTCIF   (1U << 11)  /* LIFCR Stream 1 TC clear */
#define DMA1_S3_TCIF    (1U << 27)  /* HISR Stream 3 TC flag  */
#define DMA1_S3_CTCIF   (1U << 27)  /* HIFCR Stream 3 TC clear*/

/* USART SR / CR bits */
#define USART_SR_IDLE   (1U << 4)
#define USART_SR_TC     (1U << 6)
#define USART_SR_RXNE   (1U << 5)
#define USART_CR1_IDLEIE (1U << 4)
#define USART_CR3_DMAT  (1U << 7)
#define USART_CR3_DMAR  (1U << 6)

/* NVIC helper */
#define NVIC_ISER_BASE  ((volatile uint32_t *)0xE000E100U)
#define NVIC_IPR_BASE   ((volatile uint8_t  *)0xE000E400U)
#define NVIC_EnableIRQ(n) (NVIC_ISER_BASE[(n) >> 5] = 1U << ((n) & 0x1F))
#define NVIC_SetPriority(n, prio) (NVIC_IPR_BASE[n] = (prio) << 4)

/* IRQ numbers for STM32F407 */
#define IRQ_DMA1_STREAM1  12
#define IRQ_DMA1_STREAM3  14
#define IRQ_USART3        39

/* ======================================================================
 *  Module-level state
 * ====================================================================== */
#define MODBUS_RX_BUF_SIZE  256
#define MODBUS_TX_BUF_SIZE  16
#define MODBUS_BAUD         9600U
#define APB1_CLOCK_HZ       16000000UL

/* DMA-owned buffers */
static uint8_t rx_dma_buf[MODBUS_RX_BUF_SIZE];
static uint8_t tx_dma_buf[MODBUS_TX_BUF_SIZE];

/* Byte count captured in IDLE ISR */
static volatile uint16_t rx_received_len = 0;

/* RTOS synchronisation primitives */
static osSemaphoreId_t sem_dma_rx_done  = NULL;
static osSemaphoreId_t sem_dma_tx_done  = NULL;

/* Shared state (declared in modbus_dma.h as extern) */
osMutexId_t         sensorMutex;
osMutexId_t         configMutex;
Modbus_SensorData_t sharedSensorData;
Gateway_Config_t    sharedConfig;
uint8_t             relayStates[MAX_RELAYS];
volatile ModbusScanStatus_t g_scan_status = {0};

/* ======================================================================
 *  CRC16 — standard Modbus polynomial
 * ====================================================================== */
static uint16_t Modbus_CRC16(const uint8_t *buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else               { crc >>= 1; }
        }
    }
    return crc;
}

/* ======================================================================
 *  MAX485 direction helpers
 * ====================================================================== */
static inline void MAX485_TX(void) {
    GPIOD->BSRR = (1U << 3) | (1U << 2);       /* DE=1, RE#=1 */
}
static inline void MAX485_RX(void) {
    GPIOD->BSRR = (1U << (3+16)) | (1U << (2+16)); /* DE=0, RE#=0 */
}

/* ======================================================================
 *  DMA1 Stream 1 — USART3 RX  (Peripheral → Memory)
 * ====================================================================== */
static void DMA1_Rx_Init(void) {
    /* Disable stream first */
    DMA1->Stream[1].CR &= ~DMA_CR_EN;
    while (DMA1->Stream[1].CR & DMA_CR_EN); /* wait for disable */

    /* Clear any stale flags in LISR for stream 1 (bits 6..11) */
    DMA1->LIFCR = 0x0F7D0000UL >> 16; /* bits 11:6 = 0b111111 << 6 = 0xFC0 */
    DMA1->LIFCR |= (0x3FU << 6);

    DMA1->Stream[1].PAR  = (uint32_t)&USART3->DR;
    DMA1->Stream[1].M0AR = (uint32_t)rx_dma_buf;
    DMA1->Stream[1].NDTR = MODBUS_RX_BUF_SIZE;
    DMA1->Stream[1].CR   = DMA_CR_CHSEL(4)  /* Channel 4 = USART3_RX */
                           | DMA_CR_DIR_P2M
                           | DMA_CR_MINC
                           | DMA_CR_PSIZE8
                           | DMA_CR_MSIZE8;
    /* Do NOT enable TC interrupt on RX DMA; we use USART IDLE instead */

    DMA1->Stream[1].CR |= DMA_CR_EN;
}

/* ======================================================================
 *  DMA1 Stream 3 — USART3 TX  (Memory → Peripheral)
 * ====================================================================== */
static void DMA1_Tx_Start(const uint8_t *data, uint8_t len) {
    /* Disable before reconfiguring */
    DMA1->Stream[3].CR &= ~DMA_CR_EN;
    while (DMA1->Stream[3].CR & DMA_CR_EN);

    /* Clear stream 3 flags in LISR (bits 22..27) */
    DMA1->LIFCR |= (0x3FU << 22);

    memcpy(tx_dma_buf, data, len);

    DMA1->Stream[3].PAR  = (uint32_t)&USART3->DR;
    DMA1->Stream[3].M0AR = (uint32_t)tx_dma_buf;
    DMA1->Stream[3].NDTR = len;
    DMA1->Stream[3].CR   = DMA_CR_CHSEL(4)  /* Channel 4 = USART3_TX */
                           | DMA_CR_DIR_M2P
                           | DMA_CR_MINC
                           | DMA_CR_PSIZE8
                           | DMA_CR_MSIZE8
                           | DMA_CR_TCIE;    /* TC interrupt for TX done */
    DMA1->Stream[3].CR |= DMA_CR_EN;
}

/* ======================================================================
 *  USART3 + DMA Initialisation
 * ====================================================================== */
void Modbus_DMA_Init(void) {
    printf("[Modbus_DMA] Initializing USART3 Modbus RTU @ 9600 baud...\r\n");
    
    /* ---- Create RTOS primitives ---- */
    sem_dma_rx_done = osSemaphoreNew(1, 0, NULL);
    sem_dma_tx_done = osSemaphoreNew(1, 0, NULL);
    printf("[Modbus_DMA] Semaphores created\r\n");
    
    /* Guard: only create mutexes if not already created by main_kontrx.c.
     * Unconditional osMutexNew() here would silently overwrite the handle,
     * leaving other tasks using stale (unsynchronised) mutex objects. */
    if (!sensorMutex) sensorMutex = osMutexNew(NULL);
    if (!configMutex) configMutex = osMutexNew(NULL);
    printf("[Modbus_DMA] Mutexes ready (or already created)\r\n");

    /* Initialise shared state to safe defaults */
    memset(&sharedSensorData, 0, sizeof(sharedSensorData));
    sharedSensorData.ph = sharedSensorData.orp = sharedSensorData.ec
        = sharedSensorData.do_val = sharedSensorData.ammonia
        = sharedSensorData.ultrasonic_dist = sharedSensorData.ultrasonic_temp = -999.0f;
    printf("[Modbus_DMA] Shared sensor data initialized to -999.0f\r\n");

    /* ---- GPIO clocks ---- */
    RCC->AHB1ENR |= (1U << 1) | (1U << 3) | (1U << 4); /* GPIOB, GPIOD, GPIOE */

    /* GPIOB PB10=TX, PB11=RX → AF7 (USART3) */
    GPIOB->MODER  &= ~((3U << 20) | (3U << 22));
    GPIOB->MODER  |=  ((2U << 20) | (2U << 22)); /* Alternate function */
    GPIOB->OSPEEDR|=  ((2U << 20) | (2U << 22)); /* High speed */
    GPIOB->PUPDR  &= ~((3U << 20) | (3U << 22));
    GPIOB->PUPDR  |=  ((1U << 20) | (1U << 22)); /* Pull-up */
    GPIOB->AFR[1] &= ~((0xFU << 8) | (0xFU << 12));
    GPIOB->AFR[1] |=  ((7U  << 8) | (7U  << 12)); /* AF7 = USART3 */

    /* PD3=DE, PD2=RE# → outputs, start in RX mode */
    GPIOD->MODER  &= ~((3U << 4) | (3U << 6));
    GPIOD->MODER  |=  ((1U << 4) | (1U << 6));
    GPIOD->OSPEEDR|=  ((2U << 4) | (2U << 6));
    GPIOD->OTYPER &= ~((1U << 2) | (1U << 3));
    GPIOD->PUPDR  &= ~((3U << 4) | (3U << 6));
    MAX485_RX();
    printf("[Modbus_DMA] GPIO configured: USART3 (PB10/11), MAX485 (PD2/3)\r\n");

    /* ---- Enable clocks for USART3 and DMA1 ---- */
    RCC->APB1ENR |= (1U << 18); /* USART3 */
    RCC->AHB1ENR |= (1U << 21); /* DMA1   */

    /* ---- Configure USART3 ---- */
    USART3->CR1 = 0;
    /* BRR = APB1_CLK / BAUD = 16000000/9600 ≈ 1667 = 0x0683 */
    USART3->BRR = (uint32_t)(APB1_CLOCK_HZ / MODBUS_BAUD);
    USART3->CR2 = 0; /* 1 stop bit */
    USART3->CR3 = USART_CR3_DMAT | USART_CR3_DMAR; /* Enable DMA for TX and RX */
    USART3->CR1 = (1U << 13) /* UE */
                | (1U <<  3) /* TE */
                | (1U <<  2) /* RE */
                | USART_CR1_IDLEIE; /* IDLE interrupt → end-of-frame detection */

    /* ---- Initialise DMA RX stream (stays running) ---- */
    DMA1_Rx_Init();
    printf("[Modbus_DMA] DMA RX stream initialized\r\n");

    /* ---- Enable interrupts ---- */
    /* USART3: priority 5 (below max syscall, so FreeRTOS API is safe) */
    NVIC_SetPriority(IRQ_USART3, 5);
    NVIC_EnableIRQ(IRQ_USART3);

    /* DMA1 Stream 3 TX complete: same priority */
    NVIC_SetPriority(IRQ_DMA1_STREAM3, 5);
    NVIC_EnableIRQ(IRQ_DMA1_STREAM3);
    printf("[Modbus_DMA] Interrupts enabled (USART3, DMA1 Stream 3)\r\n");
    printf("[Modbus_DMA] === Initialization Complete ===\r\n\r\n");
}

/* ======================================================================
 *  Non-blocking TX helper — sends a Modbus frame via DMA
 * ====================================================================== */
static uint8_t Modbus_Send_Frame(const uint8_t *frame, uint8_t len) {
    /* Arm RX DMA to capture the response */
    DMA1->Stream[1].CR &= ~DMA_CR_EN;
    while (DMA1->Stream[1].CR & DMA_CR_EN);
    DMA1->LIFCR |= (0x3FU << 6);
    DMA1->Stream[1].NDTR = MODBUS_RX_BUF_SIZE;
    DMA1->Stream[1].CR  |= DMA_CR_EN;

    /* 3.5-char inter-frame delay (≈4ms @ 9600 baud) */
    osDelay(5);

    MAX485_TX();
    osDelay(1); /* MAX485 driver settling */

    /* Kick off TX via DMA */
    DMA1_Tx_Start(frame, len);

    /* Block until TX DMA fires TC interrupt (max 50 ms) */
    osStatus_t st = osSemaphoreAcquire(sem_dma_tx_done, 50);
    MAX485_RX();

    return (st == osOK) ? 1 : 0;
}

/* ======================================================================
 *  Non-blocking read of N holding registers from slave
 * ====================================================================== */
static uint8_t Modbus_Read_Registers_Timeout(uint8_t slave, uint16_t reg,
                                             uint8_t num_regs, uint16_t *out, uint32_t timeout_ms) {
    uint8_t frame[8];
    frame[0] = slave;
    frame[1] = 0x03; /* FC: Read Holding Registers */
    frame[2] = (uint8_t)(reg >> 8);
    frame[3] = (uint8_t)(reg & 0xFF);
    frame[4] = 0;
    frame[5] = num_regs;
    uint16_t crc = Modbus_CRC16(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF);
    frame[7] = (uint8_t)(crc >> 8);

    if (!Modbus_Send_Frame(frame, 8)) {
        printf("[Modbus] TX failed for slave=%d reg=0x%04X\r\n", slave, reg);
        return 0;
    }

    /* Wait for IDLE IRQ to post the semaphore */
    if (osSemaphoreAcquire(sem_dma_rx_done, timeout_ms) != osOK) {
        printf("[Modbus] RX timeout for slave=%d reg=0x%04X (timeout=%lu ms)\r\n", slave, reg, timeout_ms);
        return 0;
    }

    uint16_t len = rx_received_len;
    uint8_t  expected = 3 + (num_regs * 2) + 2;

    if (len < expected) {
        printf("[Modbus] RX too short for slave=%d: got %d, expected %d\r\n", slave, len, expected);
        return 0;
    }
    if (rx_dma_buf[0] != slave || rx_dma_buf[1] != 0x03) {
        printf("[Modbus] RX header mismatch: slave=%d (got %d), fc=0x03 (got 0x%02X)\r\n", 
               slave, rx_dma_buf[0], rx_dma_buf[1]);
        return 0;
    }

    uint16_t rx_crc = rx_dma_buf[len-2] | ((uint16_t)rx_dma_buf[len-1] << 8);
    if (Modbus_CRC16(rx_dma_buf, (uint8_t)(len - 2)) != rx_crc) {
        printf("[Modbus] CRC error for slave=%d\r\n", slave);
        return 0;
    }

    uint8_t idx = 3;
    for (uint8_t i = 0; i < num_regs; i++) {
        out[i] = ((uint16_t)rx_dma_buf[idx] << 8) | rx_dma_buf[idx+1];
        idx += 2;
    }
    return 1;
}

static uint8_t Modbus_Read_Registers(uint8_t slave, uint16_t reg,
                                     uint8_t num_regs, uint16_t *out) {
    return Modbus_Read_Registers_Timeout(slave, reg, num_regs, out, 200);
}

void Modbus_DMA_PerformScan(void) {
    g_scan_status.count = 0;
    g_scan_status.progress = 0;
    
    uint16_t regs[10];
    
    for (int id = 1; id <= 247; id++) {
        if (!g_scan_status.is_scanning) {
            break;
        }
        
        // 1. Try reading DO float registers at 0x2600 (6 registers) with 50ms timeout
        if (Modbus_Read_Registers_Timeout(id, 0x2600, 6, regs, 50)) {
            if (g_scan_status.count < SCAN_MAX_DEVICES) {
                g_scan_status.devices[g_scan_status.count].id = id;
                g_scan_status.devices[g_scan_status.count].type = 4; // DO
                g_scan_status.count++;
            }
        }
        // 2. Try reading 10 registers at 0x0000 with 50ms timeout (Ultrasonic)
        else if (Modbus_Read_Registers_Timeout(id, 0x0000, 10, regs, 50)) {
            if (g_scan_status.count < SCAN_MAX_DEVICES) {
                g_scan_status.devices[g_scan_status.count].id = id;
                g_scan_status.devices[g_scan_status.count].type = 6; // Ultrasonic
                g_scan_status.count++;
            }
        }
        // 3. Try reading 2 registers at 0x0000 with 50ms timeout (pH, ORP, EC, Ammonia)
        else if (Modbus_Read_Registers_Timeout(id, 0x0000, 2, regs, 50)) {
            if (g_scan_status.count < SCAN_MAX_DEVICES) {
                g_scan_status.devices[g_scan_status.count].id = id;
                uint8_t type = 255; // Unknown/Generic device
                if (id == 1) type = 1;      // pH
                else if (id == 2) type = 2; // ORP
                else if (id == 3) type = 3; // EC
                else if (id == 5) type = 5; // Ammonia
                
                g_scan_status.devices[g_scan_status.count].type = type;
                g_scan_status.count++;
            }
        }
        
        g_scan_status.progress = (uint8_t)((id * 100) / 247);
        osDelay(5);
    }
    
    g_scan_status.progress = 100;
    g_scan_status.is_scanning = 0;
}

/* ======================================================================
 *  Decode big-endian float (DCBA order used by DO/EC sensors)
 * ====================================================================== */
static float Decode_Float_DCBA(uint16_t r0, uint16_t r1) {
    union { float f; uint8_t b[4]; } u;
    u.b[0] = (uint8_t)(r0 >> 8);
    u.b[1] = (uint8_t)(r0 & 0xFF);
    u.b[2] = (uint8_t)(r1 >> 8);
    u.b[3] = (uint8_t)(r1 & 0xFF);
    return u.f;
}

/* ======================================================================
 *  Sensor Polling Logic
 *  Polls each sensor ID 0x01..0x05 round-robin with DMA-based comm.
 * ====================================================================== */
static volatile uint8_t poll_cycle_count = 0;
static volatile uint8_t read_failures = 0;

void Modbus_DMA_PollSensors(void) {
    Modbus_SensorData_t local;
    Get_Shared_Sensor_Data(&local);

    Gateway_Config_t cfg;
    Get_Shared_Config(&cfg);

    uint8_t ph_id = (cfg.sensor_ids[0] == 0 || cfg.sensor_ids[0] == 0xFF) ? 1 : cfg.sensor_ids[0];
    uint8_t orp_id = (cfg.sensor_ids[1] == 0 || cfg.sensor_ids[1] == 0xFF) ? 2 : cfg.sensor_ids[1];
    uint8_t ec_id = (cfg.sensor_ids[2] == 0 || cfg.sensor_ids[2] == 0xFF) ? 3 : cfg.sensor_ids[2];
    uint8_t do_id = (cfg.sensor_ids[3] == 0 || cfg.sensor_ids[3] == 0xFF) ? 4 : cfg.sensor_ids[3];
    uint8_t ammonia_id = (cfg.sensor_ids[4] == 0 || cfg.sensor_ids[4] == 0xFF) ? 5 : cfg.sensor_ids[4];
    uint8_t ultra_id = (cfg.sensor_ids[5] == 0 || cfg.sensor_ids[5] == 0xFF) ? 10 : cfg.sensor_ids[5];

    uint16_t regs[10];
    uint8_t read_count = 0, fail_count = 0;
    
    /* Only log every 5 cycles to avoid console spam */
    int log_this_cycle = (poll_cycle_count % 5) == 0;
    if (log_this_cycle) {
        printf("\r\n[Modbus] === Poll Cycle %d ===\r\n", poll_cycle_count);
    }

    /* --- pH (ph_id, reg 0x0000, 2 regs) --- */
    if (Modbus_Read_Registers(ph_id, 0x0000, 2, regs)) {
        if (regs[0] != 0x7FFF) local.ph      = regs[0] / 100.0f;
        if (regs[1] != 0x7FFF) local.ph_temp = regs[1] / 100.0f;
        if (log_this_cycle) printf("[pH] OK: ID=%d  val=%.2f  temp=%.2f\r\n", ph_id, local.ph, local.ph_temp);
        read_count++;
    } else {
        local.ph = local.ph_temp = -999.0f;
        if (log_this_cycle) printf("[pH] FAIL: ID=%d timeout\r\n", ph_id);
        fail_count++;
    }
    osDelay(10);

    /* --- ORP (orp_id, reg 0x0000, 2 regs) --- */
    if (Modbus_Read_Registers(orp_id, 0x0000, 2, regs)) {
        if (regs[0] != 0x7FFF) local.orp      = (float)(int16_t)regs[0];
        if (regs[1] != 0x7FFF) local.orp_temp = regs[1] / 100.0f;
        if (log_this_cycle) printf("[ORP] OK: ID=%d  val=%.2f  temp=%.2f\r\n", orp_id, local.orp, local.orp_temp);
        read_count++;
    } else {
        local.orp = local.orp_temp = -999.0f;
        if (log_this_cycle) printf("[ORP] FAIL: ID=%d timeout\r\n", orp_id);
        fail_count++;
    }
    osDelay(10);

    /* --- EC (ec_id, reg 0x0000, 2 regs) --- */
    if (Modbus_Read_Registers(ec_id, 0x0000, 2, regs)) {
        if (regs[0] != 0x7FFF) local.ec      = regs[0] / 10.0f;
        if (regs[1] != 0x7FFF) local.ec_temp = regs[1] / 100.0f;
        if (log_this_cycle) printf("[EC] OK: ID=%d  val=%.2f  temp=%.2f\r\n", ec_id, local.ec, local.ec_temp);
        read_count++;
    } else {
        local.ec = local.ec_temp = -999.0f;
        if (log_this_cycle) printf("[EC] FAIL: ID=%d timeout\r\n", ec_id);
        fail_count++;
    }
    osDelay(10);

    /* --- DO (do_id, reg 0x2600, 6 regs — 32-bit floats) --- */
    if (Modbus_Read_Registers(do_id, 0x2600, 6, regs)) {
        local.do_temp = Decode_Float_DCBA(regs[0], regs[1]);
        local.do_val  = Decode_Float_DCBA(regs[4], regs[5]);
        if (log_this_cycle) printf("[DO] OK: ID=%d  val=%.2f  temp=%.2f\r\n", do_id, local.do_val, local.do_temp);
        read_count++;
    } else {
        local.do_temp = local.do_val = -999.0f;
        if (log_this_cycle) printf("[DO] FAIL: ID=%d timeout\r\n", do_id);
        fail_count++;
    }
    osDelay(10);

    /* --- Ammonia (ammonia_id, reg 0x0000, 2 regs) --- */
    if (Modbus_Read_Registers(ammonia_id, 0x0000, 2, regs)) {
        if (regs[0] != 0x7FFF) local.ammonia      = (float)regs[0];
        if (regs[1] != 0x7FFF) local.ammonia_temp = regs[1] / 100.0f;
        if (log_this_cycle) printf("[Ammonia] OK: ID=%d  val=%.2f  temp=%.2f\r\n", ammonia_id, local.ammonia, local.ammonia_temp);
        read_count++;
    } else {
        local.ammonia = local.ammonia_temp = -999.0f;
        if (log_this_cycle) printf("[Ammonia] FAIL: ID=%d timeout\r\n", ammonia_id);
        fail_count++;
    }
    osDelay(10);

    /* --- Ultrasonic (ultra_id, reg 0x0000, 10 regs) --- */
    if (Modbus_Read_Registers(ultra_id, 0x0000, 10, regs)) {
        local.ultrasonic_dist = regs[9] / 10.0f;
        local.ultrasonic_temp = regs[8] / 10.0f;
        if (log_this_cycle) printf("[Ultrasonic] OK: ID=%d  dist=%.2f  temp=%.2f\r\n", ultra_id, local.ultrasonic_dist, local.ultrasonic_temp);
        read_count++;
    } else {
        local.ultrasonic_dist = local.ultrasonic_temp = -999.0f;
        if (log_this_cycle) printf("[Ultrasonic] FAIL: ID=%d timeout\r\n", ultra_id);
        fail_count++;
    }

    if (log_this_cycle) {
        printf("[Modbus] Summary: %d/%d reads successful, %d failed\r\n", read_count, 6, fail_count);
    }

    read_failures = fail_count;
    poll_cycle_count++;

    local.last_update_time = (uint32_t)xTaskGetTickCount();
    Update_Shared_Sensor_Data(&local);
}

/* ======================================================================
 *  Thread-safe accessors for shared state
 * ====================================================================== */
void Get_Shared_Sensor_Data(Modbus_SensorData_t *dest) {
    osMutexAcquire(sensorMutex, osWaitForever);
    memcpy(dest, &sharedSensorData, sizeof(Modbus_SensorData_t));
    osMutexRelease(sensorMutex);
}

void Update_Shared_Sensor_Data(const Modbus_SensorData_t *src) {
    osMutexAcquire(sensorMutex, osWaitForever);
    memcpy(&sharedSensorData, src, sizeof(Modbus_SensorData_t));
    osMutexRelease(sensorMutex);
}

void Get_Shared_Config(Gateway_Config_t *dest) {
    osMutexAcquire(configMutex, osWaitForever);
    memcpy(dest, &sharedConfig, sizeof(Gateway_Config_t));
    osMutexRelease(configMutex);
}

void Update_Shared_Config(const Gateway_Config_t *src) {
    osMutexAcquire(configMutex, osWaitForever);
    memcpy(&sharedConfig, src, sizeof(Gateway_Config_t));
    osMutexRelease(configMutex);
}

/* ======================================================================
 *  INTERRUPT SERVICE ROUTINES
 * ====================================================================== */

/**
 * @brief  USART3 IRQ Handler — fires on IDLE line detection (end of Modbus frame).
 *         Captures how many bytes DMA has received and posts the RX semaphore.
 */
void USART3_IRQHandler(void) {
    if (USART3->SR & USART_SR_IDLE) {
        /* Reading SR then DR clears the IDLE flag */
        volatile uint32_t tmp = USART3->SR;
        tmp = USART3->DR;
        (void)tmp;

        /* Stop RX DMA to freeze NDTR */
        DMA1->Stream[1].CR &= ~DMA_CR_EN;

        /* Bytes received = total buffer - remaining */
        rx_received_len = (uint16_t)(MODBUS_RX_BUF_SIZE - DMA1->Stream[1].NDTR);

        /* Signal the polling task */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR((SemaphoreHandle_t)sem_dma_rx_done, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/**
 * @brief  DMA1 Stream 3 IRQ Handler — fires when USART3 TX DMA transfer completes.
 *         Signals the sending task that all bytes have physically left the UART shift register.
 */
void DMA1_Stream3_IRQHandler(void) {
    if (DMA1->LISR & (1U << 27)) {      /* TCIF3 is bit 27 in LISR */
        DMA1->LIFCR = (1U << 27);        /* Clear TCIF3 */

        /* Disable TX DMA */
        DMA1->Stream[3].CR &= ~DMA_CR_EN;

        /* Wait for TC bit (shift register drained) */
        while (!(USART3->SR & USART_SR_TC));

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR((SemaphoreHandle_t)sem_dma_tx_done, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
