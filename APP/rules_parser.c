/**
 * @file    rules_parser.c
 * @brief   POST /api/rules body -> RuleConfig_t (see rules_parser.h).
 *
 * The previous version built a cJSON tree on the FreeRTOS heap: a full rule
 * set needed 15-45 KB while ~6 KB was free, and the malloc-failed hook then
 * reset the board. This version reads the request text in place (jspan),
 * so a rule upload needs no heap at all. Field handling is unchanged.
 */
#include "rules_parser.h"
#include "jspan.h"
#include <stdio.h>
#include <string.h>

#define JV_PRESENT(v) ((v).p != NULL)

/* String values are copied; numbers are written in decimal (as before). */
static void Copy_Id(JVal v, char *dst, size_t n) {
    if (jv_is_string(v)) jv_string(v, dst, n);
    else if (jv_is_number(v)) snprintf(dst, n, "%d", jv_int(v));
}

static uint8_t Parse_Op_String(JVal v) {
    if (jv_string_eq(v, ">")) return 1;
    if (jv_string_eq(v, "<")) return 2;
    if (jv_string_eq(v, "==")) return 3;
    if (jv_string_eq(v, "!=")) return 4;
    if (jv_string_eq(v, ">=")) return 5;
    if (jv_string_eq(v, "<=")) return 6;
    return 0;
}

static int8_t Parse_Condition_Tree(JVal cond, RuleConfig_t *cfg);

/* "child" object -> children[0] (range, timer_on, pulse_timer, not). */
static void Parse_Single_Child(JVal cond, CondNode_t *n, RuleConfig_t *cfg) {
    JVal ch = jv_get(cond, "child");
    if (jv_is_object(ch)) {
        int8_t c_idx = Parse_Condition_Tree(ch, cfg);
        if (c_idx >= 0) { n->children[0] = c_idx; n->child_count = 1; }
    }
}

static int8_t Parse_Condition_Tree(JVal cond, RuleConfig_t *cfg) {
    if (!jv_is_object(cond) || !cfg) return -1;
    if (cfg->cond_node_count >= MAX_COND_NODES) return -1;

    int8_t node_idx = (int8_t)cfg->cond_node_count;
    CondNode_t *n = &cfg->cond_nodes[node_idx];
    memset(n, 0, sizeof(CondNode_t));
    for (int k = 0; k < 4; k++) n->children[k] = -1;
    cfg->cond_node_count++;

    JVal t_item = jv_get(cond, "type");
    char type_str[16] = "compare";
    if (jv_is_string(t_item)) jv_string(t_item, type_str, sizeof(type_str));

    if (strcmp(type_str, "range") == 0) {
        n->type = COND_NODE_RANGE;
        JVal min_i = jv_get(cond, "min");
        JVal max_i = jv_get(cond, "max");
        Copy_Id(jv_get(cond, "input_id"), n->input_id, sizeof(n->input_id));
        if (jv_is_number(min_i)) n->threshold = (float)jv_number(min_i);
        if (jv_is_number(max_i)) n->threshold_b = (float)jv_number(max_i);
        n->range_mode = jv_string_eq(jv_get(cond, "mode"), "outside") ? 1 : 0;
        Parse_Single_Child(cond, n, cfg);
    } else if (strcmp(type_str, "hysteresis") == 0) {
        n->type = COND_NODE_HYSTERESIS;
        JVal hth = jv_get(cond, "high_threshold");
        JVal lth = jv_get(cond, "low_threshold");
        Copy_Id(jv_get(cond, "input_id"), n->input_id, sizeof(n->input_id));
        if (jv_is_number(hth)) n->threshold = (float)jv_number(hth);
        if (jv_is_number(lth)) n->threshold_b = (float)jv_number(lth);
    } else if (strcmp(type_str, "timer_on") == 0) {
        n->type = COND_NODE_TIMER_ON;
        JVal dms = jv_get(cond, "delay_ms");
        JVal dsec = jv_get(cond, "delay_sec");
        if (jv_is_number(dms)) n->delay_ms = (uint32_t)jv_int(dms);
        else if (jv_is_number(dsec)) n->delay_ms = (uint32_t)(jv_number(dsec) * 1000.0);
        else n->delay_ms = 1000;
        Parse_Single_Child(cond, n, cfg);
    } else if (strcmp(type_str, "pulse_timer") == 0) {
        n->type = COND_NODE_PULSE_TIMER;
        JVal pms = jv_get(cond, "pulse_ms");
        JVal psec = jv_get(cond, "pulse_sec");
        if (jv_is_number(pms)) n->delay_ms = (uint32_t)jv_int(pms);
        else if (jv_is_number(psec)) n->delay_ms = (uint32_t)(jv_number(psec) * 1000.0);
        else n->delay_ms = 3000;
        Parse_Single_Child(cond, n, cfg);
    } else if (strcmp(type_str, "sr_latch") == 0) {
        n->type = COND_NODE_SR_LATCH;
        n->sr_priority = jv_string_eq(jv_get(cond, "priority"), "set") ? 1 : 0;
        JVal sc = jv_get(cond, "set_condition");
        if (jv_is_object(sc)) {
            int8_t s_idx = Parse_Condition_Tree(sc, cfg);
            if (s_idx >= 0) { n->children[0] = s_idx; n->child_count = 1; }
        }
        JVal rc = jv_get(cond, "reset_condition");
        if (jv_is_object(rc)) {
            int8_t r_idx = Parse_Condition_Tree(rc, cfg);
            if (r_idx >= 0) { n->children[1] = r_idx; n->child_count = 2; }
        }
    } else if (strcmp(type_str, "and") == 0 || strcmp(type_str, "or") == 0) {
        n->type = (strcmp(type_str, "and") == 0) ? COND_NODE_AND : COND_NODE_OR;
        JVal children = jv_get(cond, "children");
        if (jv_is_array(children)) {
            int sz = jv_array_size(children);
            for (int c = 0; c < sz && c < 4; c++) {
                int8_t ch_idx = Parse_Condition_Tree(jv_array_item(children, c), cfg);
                if (ch_idx >= 0) {
                    n->children[n->child_count++] = ch_idx;
                }
            }
        }
    } else if (strcmp(type_str, "not") == 0) {
        n->type = COND_NODE_NOT;
        Parse_Single_Child(cond, n, cfg);
    } else if (strcmp(type_str, "sensor_state") == 0) {
        n->type = COND_NODE_SENSOR_STATE;
        Copy_Id(jv_get(cond, "input_id"), n->input_id, sizeof(n->input_id));
        n->op = 4; // != -999.0
        n->threshold = -999.0f;
    } else {
        // Default: compare
        n->type = COND_NODE_COMPARE;
        JVal op_i = jv_get(cond, "operator");
        JVal th_i = jv_get(cond, "threshold");
        Copy_Id(jv_get(cond, "input_id"), n->input_id, sizeof(n->input_id));
        n->op = jv_is_string(op_i) ? Parse_Op_String(op_i) : 1; // Default '>'
        n->threshold = jv_is_number(th_i) ? (float)jv_number(th_i) : 0.0f;

        if (jv_string_eq(jv_get(cond, "compare_mode"), "input")) {
            n->compare_mode = 1;
            Copy_Id(jv_get(cond, "input_b_id"), n->input_b_id, sizeof(n->input_b_id));
        }
    }

    return node_idx;
}

static int8_t Parse_Action_Sequence(JVal seq_obj, RuleConfig_t *cfg) {
    if (!jv_is_object(seq_obj) || !cfg) return -1;
    if (cfg->seq_count >= MAX_SEQUENCES) return -1;

    int8_t s_idx = (int8_t)cfg->seq_count;
    ActionSequence_t *seq = &cfg->sequences[s_idx];
    memset(seq, 0, sizeof(ActionSequence_t));
    seq->enabled = 1;

    JVal m_item = jv_get(seq_obj, "mode");
    if (jv_is_string(m_item)) {
        if (jv_string_eq(m_item, "loop")) seq->mode = SEQ_MODE_LOOP;
        else if (jv_string_eq(m_item, "count")) seq->mode = SEQ_MODE_COUNT;
        else seq->mode = SEQ_MODE_ONCE;
    }
    JVal of_item = jv_get(seq_obj, "on_false");
    if (jv_is_string(of_item)) {
        if (jv_string_eq(of_item, "finish_cycle")) seq->on_false = SEQ_ON_FALSE_FINISH_CYCLE;
        else if (jv_string_eq(of_item, "hold")) seq->on_false = SEQ_ON_FALSE_HOLD;
        else seq->on_false = SEQ_ON_FALSE_ABORT_SAFE;
    }
    JVal rc_item = jv_get(seq_obj, "repeat_count");
    if (jv_is_number(rc_item)) seq->repeat_count = (uint8_t)jv_int(rc_item);

    JVal steps_arr = jv_get(seq_obj, "steps");
    if (jv_is_array(steps_arr)) {
        int sz = jv_array_size(steps_arr);
        for (int s = 0; s < sz && s < MAX_SEQ_STEPS; s++) {
            JVal st_item = jv_array_item(steps_arr, s);
            if (jv_is_object(st_item)) {
                JVal act_i = jv_get(st_item, "action");
                JVal val_i = jv_get(st_item, "value");
                JVal hold_i = jv_get(st_item, "hold_ms");
                JVal hold_sec = jv_get(st_item, "hold_sec");

                SeqStep_t *st = &seq->steps[seq->step_count++];
                if (jv_is_string(act_i)) jv_string(act_i, st->action, sizeof(st->action));
                if (jv_is_number(val_i)) st->value = (float)jv_number(val_i);
                if (jv_is_number(hold_i)) st->hold_ms = (uint32_t)jv_int(hold_i);
                else if (jv_is_number(hold_sec)) st->hold_ms = (uint32_t)(jv_number(hold_sec) * 1000.0);
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

/* Fills the flat legacy fields of one rule from its "condition" object. */
static void Parse_Rule_Condition_Fields(JVal cond, Rule_t *r) {
    JVal ctype = jv_get(cond, "type");
    if (!jv_is_string(ctype)) return;

    if (jv_string_eq(ctype, "hysteresis")) {
        r->condition_type = COND_TYPE_HYSTERESIS;
        JVal hth = jv_get(cond, "high_threshold");
        JVal lth = jv_get(cond, "low_threshold");
        if (jv_is_number(hth)) r->high_threshold = (float)jv_number(hth);
        if (jv_is_number(lth)) r->low_threshold = (float)jv_number(lth);
    } else if (jv_string_eq(ctype, "range")) {
        r->condition_type = COND_TYPE_RANGE;
        JVal rmin = jv_get(cond, "min");
        JVal rmax = jv_get(cond, "max");
        if (jv_is_number(rmin)) r->min_threshold = (float)jv_number(rmin);
        if (jv_is_number(rmax)) r->max_threshold = (float)jv_number(rmax);
        r->range_mode = jv_string_eq(jv_get(cond, "mode"), "outside") ? 1 : 0;
    } else if (jv_string_eq(ctype, "timer_on")) {
        r->condition_type = COND_TYPE_TIMER_ON;
        JVal dms = jv_get(cond, "delay_ms");
        JVal dsec = jv_get(cond, "delay_sec");
        if (jv_is_number(dms)) r->delay_ms = (uint32_t)jv_int(dms);
        else if (jv_is_number(dsec)) r->delay_ms = (uint32_t)(jv_number(dsec) * 1000.0);
    } else if (jv_string_eq(ctype, "pulse_timer")) {
        r->condition_type = COND_TYPE_PULSE_TIMER;
        JVal pms = jv_get(cond, "pulse_ms");
        JVal psec = jv_get(cond, "pulse_sec");
        if (jv_is_number(pms)) r->delay_ms = (uint32_t)jv_int(pms);
        else if (jv_is_number(psec)) r->delay_ms = (uint32_t)(jv_number(psec) * 1000.0);
    } else if (jv_string_eq(ctype, "sr_latch")) {
        r->condition_type = COND_TYPE_SR_LATCH;
        r->sr_priority = jv_string_eq(jv_get(cond, "priority"), "set") ? 1 : 0;
        /* Check nested set_condition & reset_condition objects if present */
        JVal set_c = jv_get(cond, "set_condition");
        if (jv_is_object(set_c)) {
            JVal s_op = jv_get(set_c, "operator");
            JVal s_th = jv_get(set_c, "threshold");
            Copy_Id(jv_get(set_c, "input_id"), r->input_id, sizeof(r->input_id));
            if (jv_is_string(s_op)) jv_string(s_op, r->operator, sizeof(r->operator));
            if (jv_is_number(s_th)) r->threshold = (float)jv_number(s_th);
        }
        JVal rst_c = jv_get(cond, "reset_condition");
        if (jv_is_object(rst_c)) {
            Copy_Id(jv_get(rst_c, "input_id"), r->input_b_id, sizeof(r->input_b_id));
        }
    } else if (jv_string_eq(ctype, "and")) {
        r->condition_type = COND_TYPE_AND;
    } else if (jv_string_eq(ctype, "or")) {
        r->condition_type = COND_TYPE_OR;
    } else if (jv_string_eq(ctype, "not")) {
        r->condition_type = COND_TYPE_NOT;
    }
}

const char *RulesParser_Parse(const char *body, size_t len, RuleConfig_t *out, uint32_t auto_tick) {
    JVal root = jv_parse(body, len);
    if (!JV_PRESENT(root)) return "Invalid JSON syntax";

    /* Accept both wrapped {"version_id","timestamp","rules":[...]} and bare array [...] */
    JVal version_item = { NULL, NULL };
    JVal timestamp_item = { NULL, NULL };
    JVal rules_arr;

    if (jv_is_array(root)) {
        rules_arr = root;
    } else {
        version_item = jv_get(root, "version_id");
        timestamp_item = jv_get(root, "timestamp");
        rules_arr = jv_get(root, "rules");
    }

    if (!jv_is_array(rules_arr)) return "Missing rules array";

    int rules_count = jv_array_size(rules_arr);
    if (rules_count > MAX_RULES) return "Rule count exceeds MAX_RULES";

    memset(out, 0, sizeof(*out));
    out->magic = RULES_MAGIC_CURRENT;
    if (jv_is_string(version_item))
        jv_string(version_item, out->version_id, sizeof(out->version_id));
    else
        snprintf(out->version_id, sizeof(out->version_id), "auto_%lu", (unsigned long)auto_tick);
    if (jv_is_string(timestamp_item))
        jv_string(timestamp_item, out->timestamp, sizeof(out->timestamp));
    else
        strncpy(out->timestamp, "auto-generated", sizeof(out->timestamp) - 1);
    out->rule_count = 0;
    out->rules_valid = 0; // Starts unvalidated

    for (int i = 0; i < rules_count; i++) {
        JVal rule_obj = jv_array_item(rules_arr, i);
        if (!jv_is_object(rule_obj)) return "Rule item must be an object";
        Rule_t *r = &out->rules[i];

        JVal r_id   = jv_get(rule_obj, "rule_id");
        JVal in_id  = jv_get(rule_obj, "input_id");
        JVal op     = jv_get(rule_obj, "operator");
        JVal thresh = jv_get(rule_obj, "threshold");
        JVal out_id = jv_get(rule_obj, "output_id");
        JVal act    = jv_get(rule_obj, "action");
        uint8_t op_default = 0;

        /* If rule uses nested "condition" object, walk the tree to extract flat fields.
           Searches condition → child → set_condition → children[0] recursively for input_id/operator/threshold. */
        JVal cond = jv_get(rule_obj, "condition");
        if (jv_is_object(cond)) {
            JVal walk = cond;
            for (int depth = 0; depth < 5 && JV_PRESENT(walk); depth++) {
                if (!JV_PRESENT(in_id))  in_id  = jv_get(walk, "input_id");
                if (!JV_PRESENT(op))     op     = jv_get(walk, "operator");
                if (!JV_PRESENT(thresh)) thresh = jv_get(walk, "threshold");
                if (JV_PRESENT(in_id) && JV_PRESENT(op) && JV_PRESENT(thresh)) break;
                /* Descend into "set_condition", "child", or first element of "children" */
                JVal set_c = jv_get(walk, "set_condition");
                if (jv_is_object(set_c)) { walk = set_c; continue; }
                JVal child = jv_get(walk, "child");
                if (jv_is_object(child)) { walk = child; continue; }
                JVal children = jv_get(walk, "children");
                if (jv_is_array(children) && jv_array_size(children) > 0) {
                    walk = jv_array_item(children, 0);
                    continue;
                }
                break;
            }
            /* For sensor_state / AND / OR without operator, default to != -999 (always true when sensor online) */
            if (JV_PRESENT(in_id) && !JV_PRESENT(op)) op_default = 1;
        }

        if (!jv_is_string(r_id) ||
            (!jv_is_string(in_id) && !jv_is_number(in_id)) ||
            (!op_default && !jv_is_string(op)) ||
            (!jv_is_string(out_id) && !jv_is_number(out_id)) ||
            !jv_is_string(act)) {
            return "Rule field missing or invalid type";
        }

        /* threshold is optional for condition-based rules (default 0) */

        jv_string(r_id, r->rule_id, sizeof(r->rule_id));
        Copy_Id(in_id, r->input_id, sizeof(r->input_id));
        if (op_default) strncpy(r->operator, "!=", sizeof(r->operator) - 1);
        else jv_string(op, r->operator, sizeof(r->operator));
        r->threshold = jv_is_number(thresh) ? (float)jv_number(thresh) : -999.0f;
        Copy_Id(out_id, r->output_id, sizeof(r->output_id));
        jv_string(act, r->action, sizeof(r->action));
        r->active = 1;

        /* Sensor-to-sensor comparison: compare_mode "input" with input_b_id */
        r->compare_mode = 0;
        memset(r->input_b_id, 0, sizeof(r->input_b_id));
        if (jv_is_object(cond)) {
            JVal cm_walk = cond;
            for (int d = 0; d < 5 && JV_PRESENT(cm_walk); d++) {
                if (jv_string_eq(jv_get(cm_walk, "compare_mode"), "input")) {
                    r->compare_mode = 1;
                    Copy_Id(jv_get(cm_walk, "input_b_id"), r->input_b_id, sizeof(r->input_b_id));
                    break;
                }
                JVal ch = jv_get(cm_walk, "child");
                if (jv_is_object(ch)) { cm_walk = ch; continue; }
                JVal chs = jv_get(cm_walk, "children");
                if (jv_is_array(chs) && jv_array_size(chs) > 0) {
                    cm_walk = jv_array_item(chs, 0); continue;
                }
                break;
            }
        }

        /* --- Parse condition_type and advanced parameters --- */
        r->condition_type = COND_TYPE_COMPARE;
        if (jv_is_object(cond)) Parse_Rule_Condition_Fields(cond, r);

        /* --- Parse actuator type and specialized action values --- */
        JVal atype   = jv_get(rule_obj, "actuator_type");
        JVal aval    = jv_get(rule_obj, "value");
        JVal aparams = jv_get(rule_obj, "params");
        JVal asteps  = jv_get(rule_obj, "steps");
        JVal afreq   = jv_get(rule_obj, "frequency");
        JVal adir    = jv_get(rule_obj, "direction");

        r->actuator_type = jv_is_number(atype) ? (uint8_t)jv_int(atype) : 0;
        r->action_value = jv_is_number(aval) ? (float)jv_number(aval) : 0.0f;

        if (jv_is_number(asteps)) r->action_steps = (int32_t)jv_int(asteps);
        if (jv_is_number(afreq)) r->action_frequency = (uint32_t)jv_int(afreq);
        if (jv_is_string(adir)) {
            r->action_direction = jv_string_eq(adir, "CCW") ? 1 : 0;
        }

        /* params.* fields override top-level value/frequency when present */
        if (jv_is_object(aparams)) {
            JVal pduty = jv_get(aparams, "duty");
            JVal pfreq = jv_get(aparams, "frequency");
            JVal pma   = jv_get(aparams, "current_ma");
            JVal pv    = jv_get(aparams, "voltage_v");
            JVal pstep = jv_get(aparams, "steps");

            if (jv_is_number(pduty)) r->action_value = (float)jv_number(pduty);
            else if (jv_is_number(pma)) r->action_value = (float)jv_number(pma);
            else if (jv_is_number(pv))  r->action_value = (float)jv_number(pv);
            if (jv_is_number(pfreq)) r->action_frequency = (uint32_t)jv_int(pfreq);
            if (jv_is_number(pstep)) r->action_steps = (int32_t)jv_int(pstep);
        }

        /* auto_reverse: automatically undo action when condition becomes false */
        r->auto_reverse = jv_is_true(jv_get(rule_obj, "auto_reverse")) ? 1 : 0;

        /* Parse rule priority (default 10, range 1..16) */
        JVal prio_item = jv_get(rule_obj, "priority");
        r->priority = jv_is_number(prio_item) ? (uint8_t)jv_int(prio_item) : 10;

        /* Parse condition tree if present */
        r->root_node = -1;
        if (jv_is_object(cond)) {
            r->root_node = Parse_Condition_Tree(cond, out);
        }

        /* Parse action sequence if present */
        r->sequence_idx = -1;
        JVal seq_obj = jv_get(rule_obj, "sequence");
        if (jv_is_object(seq_obj)) {
            r->sequence_idx = Parse_Action_Sequence(seq_obj, out);
        }

        out->rule_count++;
    }

    return NULL;
}
