# 04 - KONTRX Offline Storage, Queue Streaming & SD Card Virtual FS

## 1. The Challenge of Offline Industrial Telemetry
Industrial sites frequently endure intermittent 4G/LTE or satellite connectivity. Dropped packets mean lost audit trails, compliance violations, and missing operational data. 
KONTRX implements an offline-first storage architecture that guarantees zero telemetry loss.

---

## 2. Flash FIFO Ring Buffer (`0x00088000`)
* **Capacity**: 496 KB allocated across sectors 136–259 of the W25Q16 flash chip.
* **Record Structure**: 16 bytes per entry (`timestamp_ms`, `sensor_id`, `sensor_type`, `avg_value`, `status_flags`).
* **Total Retention**: Up to 31,744 offline measurement records (over 88 hours of data at 10-second polling for typical multi-sensor installations).

---

## 3. High-Throughput Burst Upload Optimization (`PeekAt` & `Discard`)
### Historical Bottleneck:
Earlier versions popped records individually under an SPI flash mutex lock:
1. Lock flash.
2. Read 1 record from flash.
3. Erase/update head pointer in flash.
4. Unlock flash.
5. Transmit 1 MQTT packet.
This introduced excessive SPI write overhead, flash wear, and slowed queue drainage to only 5–10 records per second.

### The v2.3 Burst Solution:
* **`SDCard_Queue_PeekAt(index, &rec)`**: Reads up to 64 consecutive records into memory without modifying the flash pointers.
* **Single Batch Publish**: Encodes all 64 records into a combined burst MQTT payload.
* **`SDCard_Queue_Discard(count)`**: Once the broker confirms receipt (`PUBACK` / `SUCCESSS`), the head pointer advances by 64 in a single atomic flash update.
* **Result**: Queue drain speed increased by **>1200%**, clearing 30,000 backlogged records in under 3 minutes upon reconnect.

---

## 4. Virtual Filesystem on SD Card & Web Explorer
The `/api/fs/ls` and `/api/fs/download` endpoints expose a virtual hierarchical filesystem:
* `/LOGS`: System event audit logs (`system_events.log`, error logs).
* `/QUEUE`: Raw binary offline telemetry dumps (`telemetry_queue.dat`).
* `/RULES`: Virtual directory generated on-the-fly from the 12 flash archive slots, allowing engineers to download past configurations directly as `rules_<version>.json`.
