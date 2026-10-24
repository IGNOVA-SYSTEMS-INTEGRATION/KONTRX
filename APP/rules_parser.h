/**
 * @file    rules_parser.h
 * @brief   POST /api/rules body -> RuleConfig_t, without heap allocation.
 *
 * Same accepted input and same resulting RuleConfig_t as the former cJSON
 * implementation (checked by tests/test_rules_parser.c against it).
 */
#ifndef RULES_PARSER_H
#define RULES_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include "flash_partition.h"

/* Parses body[0..len) into *out (fully overwritten). auto_tick names the
 * version when the body has no version_id. Returns NULL on success, else
 * the error text sent back to the client. */
const char *RulesParser_Parse(const char *body, size_t len, RuleConfig_t *out, uint32_t auto_tick);

#endif /* RULES_PARSER_H */
