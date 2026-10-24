/* Reference copy of the former cJSON-based rules parser (taken verbatim from
 * APP/http_server_task.c before the jspan rewrite). Used only by
 * tests/test_rules_parser.c as the oracle the new parser must match. */
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "flash_partition.h"

static RuleConfig_t s_tempRules;

static uint8_t Parse_Op_String(const char *op_str) {
    if (!op_str) return 0;
    if (strcmp(op_str, ">") == 0) return 1;
    if (strcmp(op_str, "<") == 0) return 2;
    if (strcmp(op_str, "==") == 0) return 3;
    if (strcmp(op_str, "!=") == 0) return 4;
    if (strcmp(op_str, ">=") == 0) return 5;
    if (strcmp(op_str, "<=") == 0) return 6;
    return 0;
}

static int8_t Parse_Condition_Tree(const cJSON *cond, RuleConfig_t *cfg) {
    if (!cond || !cJSON_IsObject(cond) || !cfg) return -1;
    if (cfg->cond_node_count >= MAX_COND_NODES) return -1;

    int8_t node_idx = (int8_t)cfg->cond_node_count;
    CondNode_t *n = &cfg->cond_nodes[node_idx];
    memset(n, 0, sizeof(CondNode_t));
    for (int k = 0; k < 4; k++) n->children[k] = -1;
    cfg->cond_node_count++;

    cJSON *t_item = cJSON_GetObjectItemCaseSensitive(cond, "type");
    const char *type_str = (t_item && cJSON_IsString(t_item)) ? t_item->valuestring : "compare";

    if (strcmp(type_str, "range") == 0) {
        n->type = COND_NODE_RANGE;
        cJSON *min_i = cJSON_GetObjectItemCaseSensitive(cond, "min");
        cJSON *max_i = cJSON_GetObjectItemCaseSensitive(cond, "max");
        cJSON *mode_i = cJSON_GetObjectItemCaseSensitive(cond, "mode");
        cJSON *in_i = cJSON_GetObjectItemCaseSensitive(cond, "input_id");
        if (in_i) {
            if (cJSON_IsString(in_i)) strncpy(n->input_id, in_i->valuestring, sizeof(n->input_id) - 1);
            else if (cJSON_IsNumber(in_i)) snprintf(n->input_id, sizeof(n->input_id), "%d", in_i->valueint);
        }
        if (min_i && cJSON_IsNumber(min_i)) n->threshold = (float)min_i->valuedouble;
        if (max_i && cJSON_IsNumber(max_i)) n->threshold_b = (float)max_i->valuedouble;
        n->range_mode = (mode_i && cJSON_IsString(mode_i) && strcmp(mode_i->valuestring, "outside") == 0) ? 1 : 0;
        cJSON *ch = cJSON_GetObjectItemCaseSensitive(cond, "child");
        if (ch && cJSON_IsObject(ch)) {
            int8_t c_idx = Parse_Condition_Tree(ch, cfg);
            if (c_idx >= 0) { n->children[0] = c_idx; n->child_count = 1; }
        }
    } else if (strcmp(type_str, "hysteresis") == 0) {
        n->type = COND_NODE_HYSTERESIS;
        cJSON *hth = cJSON_GetObjectItemCaseSensitive(cond, "high_threshold");
        cJSON *lth = cJSON_GetObjectItemCaseSensitive(cond, "low_threshold");
        cJSON *in_i = cJSON_GetObjectItemCaseSensitive(cond, "input_id");
        if (in_i) {
            if (cJSON_IsString(in_i)) strncpy(n->input_id, in_i->valuestring, sizeof(n->input_id) - 1);
            else if (cJSON_IsNumber(in_i)) snprintf(n->input_id, sizeof(n->input_id), "%d", in_i->valueint);
        }
        if (hth && cJSON_IsNumber(hth)) n->threshold = (float)hth->valuedouble;
        if (lth && cJSON_IsNumber(lth)) n->threshold_b = (float)lth->valuedouble;
    } else if (strcmp(type_str, "timer_on") == 0) {
        n->type = COND_NODE_TIMER_ON;
        cJSON *dms = cJSON_GetObjectItemCaseSensitive(cond, "delay_ms");
        cJSON *dsec = cJSON_GetObjectItemCaseSensitive(cond, "delay_sec");
        if (dms && cJSON_IsNumber(dms)) n->delay_ms = (uint32_t)dms->valueint;
        else if (dsec && cJSON_IsNumber(dsec)) n->delay_ms = (uint32_t)(dsec->valuedouble * 1000.0);
        else n->delay_ms = 1000;
        cJSON *ch = cJSON_GetObjectItemCaseSensitive(cond, "child");
        if (ch && cJSON_IsObject(ch)) {
            int8_t c_idx = Parse_Condition_Tree(ch, cfg);
            if (c_idx >= 0) { n->children[0] = c_idx; n->child_count = 1; }
        }
    } else if (strcmp(type_str, "pulse_timer") == 0) {
        n->type = COND_NODE_PULSE_TIMER;
        cJSON *pms = cJSON_GetObjectItemCaseSensitive(cond, "pulse_ms");
        cJSON *psec = cJSON_GetObjectItemCaseSensitive(cond, "pulse_sec");
        if (pms && cJSON_IsNumber(pms)) n->delay_ms = (uint32_t)pms->valueint;
        else if (psec && cJSON_IsNumber(psec)) n->delay_ms = (uint32_t)(psec->valuedouble * 1000.0);
        else n->delay_ms = 3000;
        cJSON *ch = cJSON_GetObjectItemCaseSensitive(cond, "child");
        if (ch && cJSON_IsObject(ch)) {
            int8_t c_idx = Parse_Condition_Tree(ch, cfg);
            if (c_idx >= 0) { n->children[0] = c_idx; n->child_count = 1; }
        }
    } else if (strcmp(type_str, "sr_latch") == 0) {
        n->type = COND_NODE_SR_LATCH;
        cJSON *prio = cJSON_GetObjectItemCaseSensitive(cond, "priority");
        n->sr_priority = (prio && cJSON_IsString(prio) && strcmp(prio->valuestring, "set") == 0) ? 1 : 0;
        cJSON *sc = cJSON_GetObjectItemCaseSensitive(cond, "set_condition");
        if (sc && cJSON_IsObject(sc)) {
            int8_t s_idx = Parse_Condition_Tree(sc, cfg);
            if (s_idx >= 0) { n->children[0] = s_idx; n->child_count = 1; }
        }
        cJSON *rc = cJSON_GetObjectItemCaseSensitive(cond, "reset_condition");
        if (rc && cJSON_IsObject(rc)) {
            int8_t r_idx = Parse_Condition_Tree(rc, cfg);
            if (r_idx >= 0) { n->children[1] = r_idx; n->child_count = 2; }
        }
    } else if (strcmp(type_str, "and") == 0 || strcmp(type_str, "or") == 0) {
        n->type = (strcmp(type_str, "and") == 0) ? COND_NODE_AND : COND_NODE_OR;
        cJSON *children = cJSON_GetObjectItemCaseSensitive(cond, "children");
        if (children && cJSON_IsArray(children)) {
            int sz = cJSON_GetArraySize(children);
            for (int c = 0; c < sz && c < 4; c++) {
                cJSON *ch_item = cJSON_GetArrayItem(children, c);
                int8_t ch_idx = Parse_Condition_Tree(ch_item, cfg);
                if (ch_idx >= 0) {
                    n->children[n->child_count++] = ch_idx;
                }
            }
        }
    } else if (strcmp(type_str, "not") == 0) {
        n->type = COND_NODE_NOT;
        cJSON *ch = cJSON_GetObjectItemCaseSensitive(cond, "child");
        if (ch && cJSON_IsObject(ch)) {
            int8_t c_idx = Parse_Condition_Tree(ch, cfg);
            if (c_idx >= 0) { n->children[0] = c_idx; n->child_count = 1; }
        }
    } else if (strcmp(type_str, "sensor_state") == 0) {
        n->type = COND_NODE_SENSOR_STATE;
        cJSON *in_i = cJSON_GetObjectItemCaseSensitive(cond, "input_id");
        if (in_i) {
            if (cJSON_IsString(in_i)) strncpy(n->input_id, in_i->valuestring, sizeof(n->input_id) - 1);
            else if (cJSON_IsNumber(in_i)) snprintf(n->input_id, sizeof(n->input_id), "%d", in_i->valueint);
        }
        n->op = 4; // != -999.0
        n->threshold = -999.0f;
    } else {
        // Default: compare
        n->type = COND_NODE_COMPARE;
        cJSON *in_i = cJSON_GetObjectItemCaseSensitive(cond, "input_id");
        cJSON *op_i = cJSON_GetObjectItemCaseSensitive(cond, "operator");
        cJSON *th_i = cJSON_GetObjectItemCaseSensitive(cond, "threshold");
        cJSON *cm_i = cJSON_GetObjectItemCaseSensitive(cond, "compare_mode");
        cJSON *in_b_i = cJSON_GetObjectItemCaseSensitive(cond, "input_b_id");

        if (in_i) {
            if (cJSON_IsString(in_i)) strncpy(n->input_id, in_i->valuestring, sizeof(n->input_id) - 1);
            else if (cJSON_IsNumber(in_i)) snprintf(n->input_id, sizeof(n->input_id), "%d", in_i->valueint);
        }
        n->op = (op_i && cJSON_IsString(op_i)) ? Parse_Op_String(op_i->valuestring) : 1; // Default '>'
        n->threshold = (th_i && cJSON_IsNumber(th_i)) ? (float)th_i->valuedouble : 0.0f;

        if (cm_i && cJSON_IsString(cm_i) && strcmp(cm_i->valuestring, "input") == 0) {
            n->compare_mode = 1;
            if (in_b_i) {
                if (cJSON_IsString(in_b_i)) strncpy(n->input_b_id, in_b_i->valuestring, sizeof(n->input_b_id) - 1);
                else if (cJSON_IsNumber(in_b_i)) snprintf(n->input_b_id, sizeof(n->input_b_id), "%d", in_b_i->valueint);
            }
        }
    }

    return node_idx;
}

static int8_t Parse_Action_Sequence(const cJSON *seq_obj, RuleConfig_t *cfg) {
    if (!seq_obj || !cJSON_IsObject(seq_obj) || !cfg) return -1;
    if (cfg->seq_count >= MAX_SEQUENCES) return -1;

    int8_t s_idx = (int8_t)cfg->seq_count;
    ActionSequence_t *seq = &cfg->sequences[s_idx];
    memset(seq, 0, sizeof(ActionSequence_t));
    seq->enabled = 1;

    cJSON *m_item = cJSON_GetObjectItemCaseSensitive(seq_obj, "mode");
    if (m_item && cJSON_IsString(m_item)) {
        if (strcmp(m_item->valuestring, "loop") == 0) seq->mode = SEQ_MODE_LOOP;
        else if (strcmp(m_item->valuestring, "count") == 0) seq->mode = SEQ_MODE_COUNT;
        else seq->mode = SEQ_MODE_ONCE;
    }
    cJSON *of_item = cJSON_GetObjectItemCaseSensitive(seq_obj, "on_false");
    if (of_item && cJSON_IsString(of_item)) {
        if (strcmp(of_item->valuestring, "finish_cycle") == 0) seq->on_false = SEQ_ON_FALSE_FINISH_CYCLE;
        else if (strcmp(of_item->valuestring, "hold") == 0) seq->on_false = SEQ_ON_FALSE_HOLD;
        else seq->on_false = SEQ_ON_FALSE_ABORT_SAFE;
    }
    cJSON *rc_item = cJSON_GetObjectItemCaseSensitive(seq_obj, "repeat_count");
    if (rc_item && cJSON_IsNumber(rc_item)) seq->repeat_count = (uint8_t)rc_item->valueint;

    cJSON *steps_arr = cJSON_GetObjectItemCaseSensitive(seq_obj, "steps");
    if (steps_arr && cJSON_IsArray(steps_arr)) {
        int sz = cJSON_GetArraySize(steps_arr);
        for (int s = 0; s < sz && s < MAX_SEQ_STEPS; s++) {
            cJSON *st_item = cJSON_GetArrayItem(steps_arr, s);
            if (st_item && cJSON_IsObject(st_item)) {
                cJSON *act_i = cJSON_GetObjectItemCaseSensitive(st_item, "action");
                cJSON *val_i = cJSON_GetObjectItemCaseSensitive(st_item, "value");
                cJSON *hold_i = cJSON_GetObjectItemCaseSensitive(st_item, "hold_ms");
                cJSON *hold_sec = cJSON_GetObjectItemCaseSensitive(st_item, "hold_sec");

                SeqStep_t *st = &seq->steps[seq->step_count++];
                if (act_i && cJSON_IsString(act_i)) strncpy(st->action, act_i->valuestring, sizeof(st->action) - 1);
                if (val_i && cJSON_IsNumber(val_i)) st->value = (float)val_i->valuedouble;
                if (hold_i && cJSON_IsNumber(hold_i)) st->hold_ms = (uint32_t)hold_i->valueint;
                else if (hold_sec && cJSON_IsNumber(hold_sec)) st->hold_ms = (uint32_t)(hold_sec->valuedouble * 1000.0);
                else st->hold_ms = 1000;
            }
        }
    }

    if (seq->step_count > 0) {
        cfg->seq_count++;
        return s_idx;
    }
    return -1;
}


const char *Legacy_RulesParse(char *body, RuleConfig_t *out, uint32_t auto_tick) {
        cJSON *root = cJSON_Parse(body);
        if (!root) {
            return "Invalid JSON syntax";
        }

        /* Accept both wrapped {"version_id","timestamp","rules":[...]} and bare array [...] */
        cJSON *version_item = NULL;
        cJSON *timestamp_item = NULL;
        cJSON *rules_arr = NULL;

        if (cJSON_IsArray(root)) {
            rules_arr = root;
        } else {
            version_item = cJSON_GetObjectItemCaseSensitive(root, "version_id");
            timestamp_item = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
            rules_arr = cJSON_GetObjectItemCaseSensitive(root, "rules");
        }

        if (!rules_arr || !cJSON_IsArray(rules_arr)) {
            cJSON_Delete(root);
            return "Missing rules array";
        }

        int rules_count = cJSON_GetArraySize(rules_arr);
        /* printf("[HTTP] POST /api/rules: Parsing %d rules, version '%s'\r\n", rules_count, version_item->valuestring); */
        if (rules_count > MAX_RULES) {
            cJSON_Delete(root);
            return "Rule count exceeds MAX_RULES";
        }

        // Use s_tempRules located in CCMRAM to avoid stack or BSS overflow
        #define tempRules s_tempRules
        memset(&tempRules, 0, sizeof(tempRules));
        tempRules.magic = RULES_MAGIC_CURRENT;
        if (version_item && cJSON_IsString(version_item))
            strncpy(tempRules.version_id, version_item->valuestring, sizeof(tempRules.version_id) - 1);
        else
            snprintf(tempRules.version_id, sizeof(tempRules.version_id), "auto_%lu", (unsigned long)auto_tick);
        if (timestamp_item && cJSON_IsString(timestamp_item))
            strncpy(tempRules.timestamp, timestamp_item->valuestring, sizeof(tempRules.timestamp) - 1);
        else
            strncpy(tempRules.timestamp, "auto-generated", sizeof(tempRules.timestamp) - 1);
        tempRules.rule_count = 0;
        tempRules.rules_valid = 0; // Starts unvalidated

        for (int i = 0; i < rules_count; i++) {
            cJSON *rule_obj = cJSON_GetArrayItem(rules_arr, i);
            if (!cJSON_IsObject(rule_obj)) {
                cJSON_Delete(root);
                return "Rule item must be an object";
            }

            cJSON *r_id = cJSON_GetObjectItemCaseSensitive(rule_obj, "rule_id");
            cJSON *in_id = cJSON_GetObjectItemCaseSensitive(rule_obj, "input_id");
            cJSON *op = cJSON_GetObjectItemCaseSensitive(rule_obj, "operator");
            cJSON *thresh = cJSON_GetObjectItemCaseSensitive(rule_obj, "threshold");
            cJSON *out_id = cJSON_GetObjectItemCaseSensitive(rule_obj, "output_id");
            cJSON *act = cJSON_GetObjectItemCaseSensitive(rule_obj, "action");

            /* If rule uses nested "condition" object, walk the tree to extract flat fields.
               Searches condition → child → set_condition → children[0] recursively for input_id/operator/threshold. */
            cJSON *cond = cJSON_GetObjectItemCaseSensitive(rule_obj, "condition");
            if (cond && cJSON_IsObject(cond)) {
                cJSON *walk = cond;
                for (int depth = 0; depth < 5 && walk; depth++) {
                    if (!in_id)  in_id  = cJSON_GetObjectItemCaseSensitive(walk, "input_id");
                    if (!op)     op     = cJSON_GetObjectItemCaseSensitive(walk, "operator");
                    if (!thresh) thresh = cJSON_GetObjectItemCaseSensitive(walk, "threshold");
                    if (in_id && op && thresh) break;
                    /* Descend into "set_condition", "child", or first element of "children" */
                    cJSON *set_c = cJSON_GetObjectItemCaseSensitive(walk, "set_condition");
                    if (set_c && cJSON_IsObject(set_c)) { walk = set_c; continue; }
                    cJSON *child = cJSON_GetObjectItemCaseSensitive(walk, "child");
                    if (child && cJSON_IsObject(child)) { walk = child; continue; }
                    cJSON *children = cJSON_GetObjectItemCaseSensitive(walk, "children");
                    if (children && cJSON_IsArray(children) && cJSON_GetArraySize(children) > 0) {
                        walk = cJSON_GetArrayItem(children, 0);
                        continue;
                    }
                    break;
                }
                /* For sensor_state / AND / OR without operator, default to != -999 (always true when sensor online) */
                if (in_id && !op) {
                    static cJSON default_op;
                    default_op.type = cJSON_String;
                    default_op.valuestring = "!=";
                    op = &default_op;
                }
            }

            if (!r_id || !cJSON_IsString(r_id) ||
                !in_id || (!cJSON_IsString(in_id) && !cJSON_IsNumber(in_id)) ||
                !op || !cJSON_IsString(op) ||
                !out_id || (!cJSON_IsString(out_id) && !cJSON_IsNumber(out_id)) ||
                !act || !cJSON_IsString(act)) {
                cJSON_Delete(root);
                return "Rule field missing or invalid type";
            }

            /* threshold is optional for condition-based rules (default 0) */

            strncpy(tempRules.rules[i].rule_id, r_id->valuestring, sizeof(tempRules.rules[i].rule_id) - 1);
            if (cJSON_IsString(in_id)) {
                strncpy(tempRules.rules[i].input_id, in_id->valuestring, sizeof(tempRules.rules[i].input_id) - 1);
            } else {
                snprintf(tempRules.rules[i].input_id, sizeof(tempRules.rules[i].input_id), "%d", in_id->valueint);
            }
            strncpy(tempRules.rules[i].operator, op->valuestring, sizeof(tempRules.rules[i].operator) - 1);
            tempRules.rules[i].threshold = (thresh && cJSON_IsNumber(thresh)) ? (float)thresh->valuedouble : -999.0f;
            if (cJSON_IsString(out_id)) {
                strncpy(tempRules.rules[i].output_id, out_id->valuestring, sizeof(tempRules.rules[i].output_id) - 1);
            } else {
                snprintf(tempRules.rules[i].output_id, sizeof(tempRules.rules[i].output_id), "%d", out_id->valueint);
            }
            strncpy(tempRules.rules[i].action, act->valuestring, sizeof(tempRules.rules[i].action) - 1);
            tempRules.rules[i].active = 1;

            /* Sensor-to-sensor comparison: compare_mode "input" with input_b_id */
            tempRules.rules[i].compare_mode = 0;
            memset(tempRules.rules[i].input_b_id, 0, sizeof(tempRules.rules[i].input_b_id));
            if (cond && cJSON_IsObject(cond)) {
                cJSON *cm_walk = cond;
                for (int d = 0; d < 5 && cm_walk; d++) {
                    cJSON *cm = cJSON_GetObjectItemCaseSensitive(cm_walk, "compare_mode");
                    if (cm && cJSON_IsString(cm) && strcmp(cm->valuestring, "input") == 0) {
                        tempRules.rules[i].compare_mode = 1;
                        cJSON *ib = cJSON_GetObjectItemCaseSensitive(cm_walk, "input_b_id");
                        if (ib) {
                            if (cJSON_IsString(ib))
                                strncpy(tempRules.rules[i].input_b_id, ib->valuestring, sizeof(tempRules.rules[i].input_b_id) - 1);
                            else if (cJSON_IsNumber(ib))
                                snprintf(tempRules.rules[i].input_b_id, sizeof(tempRules.rules[i].input_b_id), "%d", ib->valueint);
                        }
                        break;
                    }
                    cJSON *ch = cJSON_GetObjectItemCaseSensitive(cm_walk, "child");
                    if (ch && cJSON_IsObject(ch)) { cm_walk = ch; continue; }
                    cJSON *chs = cJSON_GetObjectItemCaseSensitive(cm_walk, "children");
                    if (chs && cJSON_IsArray(chs) && cJSON_GetArraySize(chs) > 0) {
                        cm_walk = cJSON_GetArrayItem(chs, 0); continue;
                    }
                    break;
                }
            }

            /* --- Parse condition_type and advanced parameters --- */
            tempRules.rules[i].condition_type = COND_TYPE_COMPARE;
            if (cond && cJSON_IsObject(cond)) {
                cJSON *ctype = cJSON_GetObjectItemCaseSensitive(cond, "type");
                if (ctype && cJSON_IsString(ctype)) {
                    if (strcmp(ctype->valuestring, "hysteresis") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_HYSTERESIS;
                        cJSON *hth = cJSON_GetObjectItemCaseSensitive(cond, "high_threshold");
                        cJSON *lth = cJSON_GetObjectItemCaseSensitive(cond, "low_threshold");
                        if (hth && cJSON_IsNumber(hth)) tempRules.rules[i].high_threshold = (float)hth->valuedouble;
                        if (lth && cJSON_IsNumber(lth)) tempRules.rules[i].low_threshold = (float)lth->valuedouble;
                    } else if (strcmp(ctype->valuestring, "range") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_RANGE;
                        cJSON *rmin = cJSON_GetObjectItemCaseSensitive(cond, "min");
                        cJSON *rmax = cJSON_GetObjectItemCaseSensitive(cond, "max");
                        cJSON *rmode = cJSON_GetObjectItemCaseSensitive(cond, "mode");
                        if (rmin && cJSON_IsNumber(rmin)) tempRules.rules[i].min_threshold = (float)rmin->valuedouble;
                        if (rmax && cJSON_IsNumber(rmax)) tempRules.rules[i].max_threshold = (float)rmax->valuedouble;
                        tempRules.rules[i].range_mode = (rmode && cJSON_IsString(rmode) && strcmp(rmode->valuestring, "outside") == 0) ? 1 : 0;
                    } else if (strcmp(ctype->valuestring, "timer_on") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_TIMER_ON;
                        cJSON *dms = cJSON_GetObjectItemCaseSensitive(cond, "delay_ms");
                        cJSON *dsec = cJSON_GetObjectItemCaseSensitive(cond, "delay_sec");
                        if (dms && cJSON_IsNumber(dms)) tempRules.rules[i].delay_ms = (uint32_t)dms->valueint;
                        else if (dsec && cJSON_IsNumber(dsec)) tempRules.rules[i].delay_ms = (uint32_t)(dsec->valuedouble * 1000.0);
                    } else if (strcmp(ctype->valuestring, "pulse_timer") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_PULSE_TIMER;
                        cJSON *pms = cJSON_GetObjectItemCaseSensitive(cond, "pulse_ms");
                        cJSON *psec = cJSON_GetObjectItemCaseSensitive(cond, "pulse_sec");
                        if (pms && cJSON_IsNumber(pms)) tempRules.rules[i].delay_ms = (uint32_t)pms->valueint;
                        else if (psec && cJSON_IsNumber(psec)) tempRules.rules[i].delay_ms = (uint32_t)(psec->valuedouble * 1000.0);
                    } else if (strcmp(ctype->valuestring, "sr_latch") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_SR_LATCH;
                        cJSON *prio = cJSON_GetObjectItemCaseSensitive(cond, "priority");
                        tempRules.rules[i].sr_priority = (prio && cJSON_IsString(prio) && strcmp(prio->valuestring, "set") == 0) ? 1 : 0;
                        /* Check nested set_condition & reset_condition objects if present */
                        cJSON *set_c = cJSON_GetObjectItemCaseSensitive(cond, "set_condition");
                        if (set_c && cJSON_IsObject(set_c)) {
                            cJSON *s_in = cJSON_GetObjectItemCaseSensitive(set_c, "input_id");
                            cJSON *s_op = cJSON_GetObjectItemCaseSensitive(set_c, "operator");
                            cJSON *s_th = cJSON_GetObjectItemCaseSensitive(set_c, "threshold");
                            if (s_in) {
                                if (cJSON_IsString(s_in))
                                    strncpy(tempRules.rules[i].input_id, s_in->valuestring, sizeof(tempRules.rules[i].input_id) - 1);
                                else if (cJSON_IsNumber(s_in))
                                    snprintf(tempRules.rules[i].input_id, sizeof(tempRules.rules[i].input_id), "%d", s_in->valueint);
                            }
                            if (s_op && cJSON_IsString(s_op))
                                strncpy(tempRules.rules[i].operator, s_op->valuestring, sizeof(tempRules.rules[i].operator) - 1);
                            if (s_th && cJSON_IsNumber(s_th))
                                tempRules.rules[i].threshold = (float)s_th->valuedouble;
                        }
                        cJSON *rst_c = cJSON_GetObjectItemCaseSensitive(cond, "reset_condition");
                        if (rst_c && cJSON_IsObject(rst_c)) {
                            cJSON *r_in = cJSON_GetObjectItemCaseSensitive(rst_c, "input_id");
                            if (r_in) {
                                if (cJSON_IsString(r_in))
                                    strncpy(tempRules.rules[i].input_b_id, r_in->valuestring, sizeof(tempRules.rules[i].input_b_id) - 1);
                                else if (cJSON_IsNumber(r_in))
                                    snprintf(tempRules.rules[i].input_b_id, sizeof(tempRules.rules[i].input_b_id), "%d", r_in->valueint);
                            }
                        }
                    } else if (strcmp(ctype->valuestring, "and") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_AND;
                    } else if (strcmp(ctype->valuestring, "or") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_OR;
                    } else if (strcmp(ctype->valuestring, "not") == 0) {
                        tempRules.rules[i].condition_type = COND_TYPE_NOT;
                    }
                }
            }

            /* --- Parse actuator type and specialized action values --- */
            cJSON *atype = cJSON_GetObjectItemCaseSensitive(rule_obj, "actuator_type");
            cJSON *aval  = cJSON_GetObjectItemCaseSensitive(rule_obj, "value");
            cJSON *aparams = cJSON_GetObjectItemCaseSensitive(rule_obj, "params");
            cJSON *asteps = cJSON_GetObjectItemCaseSensitive(rule_obj, "steps");
            cJSON *afreq  = cJSON_GetObjectItemCaseSensitive(rule_obj, "frequency");
            cJSON *adir   = cJSON_GetObjectItemCaseSensitive(rule_obj, "direction");

            tempRules.rules[i].actuator_type = (atype && cJSON_IsNumber(atype)) ? (uint8_t)atype->valueint : 0;
            tempRules.rules[i].action_value = (aval && cJSON_IsNumber(aval)) ? (float)aval->valuedouble : 0.0f;

            if (asteps && cJSON_IsNumber(asteps)) tempRules.rules[i].action_steps = (int32_t)asteps->valueint;
            if (afreq && cJSON_IsNumber(afreq)) tempRules.rules[i].action_frequency = (uint32_t)afreq->valueint;
            if (adir && cJSON_IsString(adir)) {
                tempRules.rules[i].action_direction = (strcmp(adir->valuestring, "CCW") == 0) ? 1 : 0;
            }

            /* params.* fields override top-level value/frequency when present */
            if (aparams && cJSON_IsObject(aparams)) {
                cJSON *pduty = cJSON_GetObjectItemCaseSensitive(aparams, "duty");
                cJSON *pfreq = cJSON_GetObjectItemCaseSensitive(aparams, "frequency");
                cJSON *pma   = cJSON_GetObjectItemCaseSensitive(aparams, "current_ma");
                cJSON *pv    = cJSON_GetObjectItemCaseSensitive(aparams, "voltage_v");
                cJSON *pstep = cJSON_GetObjectItemCaseSensitive(aparams, "steps");

                if (pduty && cJSON_IsNumber(pduty)) tempRules.rules[i].action_value = (float)pduty->valuedouble;
                else if (pma && cJSON_IsNumber(pma)) tempRules.rules[i].action_value = (float)pma->valuedouble;
                else if (pv && cJSON_IsNumber(pv))   tempRules.rules[i].action_value = (float)pv->valuedouble;
                if (pfreq && cJSON_IsNumber(pfreq)) tempRules.rules[i].action_frequency = (uint32_t)pfreq->valueint;
                if (pstep && cJSON_IsNumber(pstep)) tempRules.rules[i].action_steps = (int32_t)pstep->valueint;
            }

            /* auto_reverse: automatically undo action when condition becomes false */
            cJSON *auto_rev = cJSON_GetObjectItemCaseSensitive(rule_obj, "auto_reverse");
            tempRules.rules[i].auto_reverse = (auto_rev && cJSON_IsTrue(auto_rev)) ? 1 : 0;

            /* Parse rule priority (default 10, range 1..16) */
            cJSON *prio_item = cJSON_GetObjectItemCaseSensitive(rule_obj, "priority");
            tempRules.rules[i].priority = (prio_item && cJSON_IsNumber(prio_item)) ? (uint8_t)prio_item->valueint : 10;

            /* Parse condition tree if present */
            tempRules.rules[i].root_node = -1;
            if (cond && cJSON_IsObject(cond)) {
                int8_t rn = Parse_Condition_Tree(cond, &tempRules);
                tempRules.rules[i].root_node = rn;
            }

            /* Parse action sequence if present */
            tempRules.rules[i].sequence_idx = -1;
            cJSON *seq_obj = cJSON_GetObjectItemCaseSensitive(rule_obj, "sequence");
            if (seq_obj && cJSON_IsObject(seq_obj)) {
                int8_t sq = Parse_Action_Sequence(seq_obj, &tempRules);
                tempRules.rules[i].sequence_idx = sq;
            }

            tempRules.rule_count++;
        }

        cJSON_Delete(root);
        *out = tempRules;
        return NULL;
}
