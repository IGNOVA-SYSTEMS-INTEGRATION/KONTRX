#ifndef SPARKPLUG_B_ENC_H
#define SPARKPLUG_B_ENC_H

#include <stdint.h>
#include <stddef.h>
#include "modbus_dma.h"

/* Sparkplug B Datatypes */
#define SPB_DATA_TYPE_INT32   3
#define SPB_DATA_TYPE_FLOAT   9
#define SPB_DATA_TYPE_BOOLEAN 11

/* Encodes an NBIRTH (Node Birth) payload into the destination buffer.
 * Returns the final encoded length, or 0 on failure/buffer overflow. */
size_t sparkplug_encode_nbirth(uint8_t *buf, size_t max_len, uint64_t timestamp_ms, uint64_t seq,
                               const TelemetryBatch_t *batch, const Gateway_Config_t *cfg, const uint8_t *relays);

/* Encodes a DDATA (Device/Node Data) payload into the destination buffer.
 * Returns the final encoded length, or 0 on failure/buffer overflow. */
size_t sparkplug_encode_ddata(uint8_t *buf, size_t max_len, uint64_t timestamp_ms, uint64_t seq,
                              const TelemetryBatch_t *batch, const Gateway_Config_t *cfg, const uint8_t *relays);

/* Encodes an NDEATH (Node Death) payload into the destination buffer.
 * Returns the final encoded length, or 0 on failure/buffer overflow. */
size_t sparkplug_encode_ndeath(uint8_t *buf, size_t max_len, uint64_t timestamp_ms, uint64_t seq);

#endif /* SPARKPLUG_B_ENC_H */
