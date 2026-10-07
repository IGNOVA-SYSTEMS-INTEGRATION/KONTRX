# Core System State Machines

## 1. Modbus RTU Polling & DMA Acquisition State Machine

```mermaid
stateDiagram-v2
    [*] --> Idle_InterFrameWait: Power Up / Bus Init
    
    Idle_InterFrameWait --> SelectNextSensor: 275ms Interval Timer
    SelectNextSensor --> Assert_DE: Fetch configured sensor slave ID & register
    Assert_DE --> Transmit_TxDMA: Set DE/RE# High, Send request frame
    
    Transmit_TxDMA --> Deassert_DE: DMA TX Complete Interrupt (TC)
    Deassert_DE --> Await_RxDMA: Set DE/RE# Low, Arm RX DMA on USART3
    
    Await_RxDMA --> Validate_CRC: DMA RX Complete or Idle Line (IDLEF)
    Await_RxDMA --> Handle_Timeout: 100ms Timeout Expired
    
    Handle_Timeout --> LogSensorFault: Increment failure counter
    LogSensorFault --> Idle_InterFrameWait: Advance to next channel
    
    Validate_CRC --> DSP_Filter: CRC16 Valid
    Validate_CRC --> DiscardFrame: CRC16 Corrupted
    DiscardFrame --> Idle_InterFrameWait
    
    DSP_Filter --> UpdateCache: Moving Average + Median Spike Rejection
    UpdateCache --> Idle_InterFrameWait: Release sensorMutex
```

---

## 2. Dual-Bank Staging OTA Bootloader State Machine

```mermaid
stateDiagram-v2
    [*] --> App_NormalRunning: Boot into Application (0x08008000)
    
    App_NormalRunning --> HTTP_ReceivingFirmware: POST /api/ota/upload
    HTTP_ReceivingFirmware --> StreamToStaging: Write 1024B chunks to Sector 6-7 (0x08040000)
    
    StreamToStaging --> VerifyStagingCRC: Upload Complete (Post sem_ota_start)
    
    VerifyStagingCRC --> Abort_InvalidCRC: CRC Mismatch or Stack Pointer Out of Bounds
    Abort_InvalidCRC --> App_NormalRunning: Erase staging metadata, return 400 Bad Request
    
    VerifyStagingCRC --> Commit_Pending: CRC32 Valid!
    Commit_Pending --> Trigger_SystemReset: Write OTA_STATUS_PENDING to Sector 1 (0x08004000)
    
    Trigger_SystemReset --> Bootloader_Init: Hardware Reset (SCB_AIRCR)
    
    Bootloader_Init --> Check_OTA_Meta: Read Sector 1
    Check_OTA_Meta --> App_NormalRunning: Status == OK (Jump to App directly)
    
    Check_OTA_Meta --> Flash_Copy_Process: Status == PENDING
    Flash_Copy_Process --> Erase_AppSectors: Erase Sectors 2-5 (0x08008000 - 0x0803FFFF)
    Erase_AppSectors --> CopyStagingToApp: Copy 256KB from Staging to Application
    CopyStagingToApp --> Final_Verification: Verify App CRC32 matches staging
    
    Final_Verification --> Mark_Status_OK: App verified! Write OTA_STATUS_OK
    Mark_Status_OK --> Relocate_VTOR: SCB_VTOR = 0x08008000
    Relocate_VTOR --> Jump_Application: Set MSP & Call App Reset_Handler
```

---

## 3. Store-and-Forward Offline Queue State Machine

```mermaid
stateDiagram-v2
    [*] --> EvaluatingNetwork: Boot
    
    EvaluatingNetwork --> Online_MQTT_Stream: W5500 Connected & Sparkplug NBIRTH Acked
    EvaluatingNetwork --> Offline_Queueing: W5500 Link Down or Broker Disconnect
    
    Online_MQTT_Stream --> Offline_Queueing: Socket Timeout / TCP Reset
    
    state Offline_Queueing {
        [*] --> CheckSDMounted
        CheckSDMounted --> Append_SDCard: SD Card Mounted (FatFS /queue/telemetry.bin)
        CheckSDMounted --> Append_SPIFlash: SD Card Absent (W25Q16 Sector 136-259)
        Append_SDCard --> AwaitNextRecord
        Append_SPIFlash --> AwaitNextRecord
        AwaitNextRecord --> CheckSDMounted: New Telemetry Sample Ready
    }
    
    Offline_Queueing --> NetworkRestored: Ethernet Link Up & Socket Established
    
    state NetworkRestored {
        [*] --> Send_NBIRTH: Issue Sparkplug Node Birth Certificate
        Send_NBIRTH --> StartReplay: NBIRTH Published
        StartReplay --> Burst_PopQueue: Read oldest record (FIFO)
        Burst_PopQueue --> Publish_Historical_DDATA: Encode original timestamp & publish
        Publish_Historical_DDATA --> CheckQueueEmpty: Check SD/Flash Queue Count
        CheckQueueEmpty --> Burst_PopQueue: Count > 0
        CheckQueueEmpty --> Complete: Count == 0
    }
    
    NetworkRestored --> Online_MQTT_Stream: All queued records replayed
```
