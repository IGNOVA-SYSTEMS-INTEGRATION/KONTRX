#!/usr/bin/env python3
import sys

def patch():
    with open('web/index.html', 'r', encoding='utf-8') as f:
        text = f.read()

    # 1. Patch HTML in page-rules
    p1 = text.find("id='page-rules'>")
    p2 = text.find("id='page-configs'")
    if p1 == -1 or p2 == -1:
        print("ERROR: page-rules markers not found")
        sys.exit(1)

    new_html = (
        "id='page-rules'>"
        "<div id='rules-pending-banner' class='card' style='display:none;background:rgba(245,158,11,0.08);border:1.5px solid var(--yellow);margin-bottom:16px'><div class='card-title' style='color:var(--yellow);display:flex;align-items:center;gap:8px'>Pending Ruleset</div><div style='padding:8px 0'>Ruleset pending (<strong id='pending-rules-version'>--</strong>):</div><div style='margin-top:12px;display:flex;gap:10px'><button class='btn btn-sm btn-primary' onclick='acceptPendingRules()'>Accept & Apply</button><button class='btn btn-sm btn-danger' onclick='rejectPendingRules()'>Reject</button></div></div>"
        "<div id='rules-testmode-banner' class='alert alert-warn' style='display:none;margin-bottom:16px;background:rgba(245,158,11,0.12);border:1px solid #f59e0b;padding:10px 14px;border-radius:6px;color:#d97706;font-size:0.85rem'>⚠️ <strong>Test Mode Active:</strong> Automated rule evaluations are currently paused to allow manual dashboard testing. Turn off Test Mode in the top navigation bar to resume automated rules.</div>"
        "<div class='card' style='margin-bottom:16px'><div class='card-title'>Active Ruleset Summary</div><div style='padding:8px 0'><strong>Active Version:</strong> <span id='rules-version'>--</span><br/><strong>Updated At:</strong> <span id='rules-timestamp'>--</span><br/><strong>Status:</strong> <span id='rules-status'>--</span></div><div style='margin-top:12px;display:flex;align-items:center;gap:10px;border-top:1px solid var(--border);padding-top:12px;flex-wrap:wrap'><span>Bypass Sensor Validation:</span><label class='switch'><input type='checkbox' id='rules-bypass-validation' onchange='toggleBypassValidation(this.checked)'><span class='slider'></span></label><span style='font-size:0.72rem;color:var(--muted)'>(Simulates sensors on test bench)</span></div><div style='margin-top:12px;display:flex;align-items:center;gap:10px;border-top:1px solid var(--border);padding-top:12px;flex-wrap:wrap'><span>Rollback to Version:</span><select id='rules-rollback-select' style='padding:5px 10px;font-weight:600;border-radius:4px;background:var(--card2);color:var(--text);border:1px solid var(--border)'></select><button class='btn btn-sm' style='padding:5px 14px;background:rgba(245,158,11,0.2);border:1px solid #f59e0b;color:#f59e0b;font-weight:700;border-radius:4px;cursor:pointer' onclick='rollbackSelectedVersion()'>Rollback</button><button class='btn btn-sm btn-danger' style='margin-left:auto' onclick='clearRules()'>Clear All Rules</button></div></div>"
        "<div class='card' style='margin-bottom:16px'><div class='card-title'>Logic Flow Diagram</div><div id='rules-diagram-container' style='padding:10px 0'></div></div>"
        "<div class='form-section'><h3>Running Rules</h3><div style='overflow-x:auto;margin-top:10px'><table class='table'><thead><tr><th>Rule ID</th><th>Trigger Condition</th><th>Action (Output)</th><th>Active State</th></tr></thead><tbody id='rules-table-body'><tr><td colspan='4' style='color:var(--muted)'>No active rules loaded.</td></tr></tbody></table></div></div>"
        "<div class='form-section' style='margin-top:24px'><h3>Ruleset History & Archives</h3><div style='overflow-x:auto;margin-top:10px'><table class='table'><thead><tr><th>Version ID</th><th>Uploaded Timestamp</th><th>Rules Count</th><th>Actions</th></tr></thead><tbody id='rules-history-table-body'><tr><td colspan='4' style='color:var(--muted)'>No log.</td></tr></tbody></table></div></div></div><div class='page' "
    )
    text = text[:p1] + new_html + text[p2:]

    # 2. Patch JS functions
    idx1 = text.find('function getFriendlyInputName')
    idx2 = text.find('function toggleRuleActive')
    if idx1 == -1 or idx2 == -1:
        print("ERROR: JS markers not found")
        sys.exit(1)

    replacement = (
        "function getFriendlyInputName(e){if(e==null||e===undefined||e==='')return 'None';"
        "var t=parseInt(e);"
        "if(!isNaN(t)&&Array.isArray(sensors)){var a=sensors.find(function(s){return s.id===t;});if(a)return(sensorTypeNames[a.type]||a.type)+' (ID '+a.id+')';}"
        "return isNaN(t)?String(e):'Sensor (ID '+t+')';}"
        "function getFriendlyOutputName(e){if(e==null||e===undefined||e==='')return 'None';"
        "var t=parseInt(e);"
        "if(!isNaN(t)&&Array.isArray(relays)){"
        "var byId=relays.find(function(r){return r.id===t;});if(byId)return(byId.name||'Actuator '+(t+1))+' (ID '+t+')';"
        "if(t>=0&&t<relays.length)return(relays[t].name||'Actuator '+(t+1))+' (ID '+t+')';}"
        "return isNaN(t)?String(e):'Actuator (ID '+t+')';}"
        "function formatRuleCondition(e){var c=e&&e.condition?e.condition:e;"
        "if(!c){var inA=getFriendlyInputName(e.input_id);var op=e.operator||'==';var th=(e.compare_mode===1||e.compare_mode==='input')?getFriendlyInputName(e.input_b_id):(e.threshold!=null?e.threshold:0);"
        "return 'If <strong>'+inA+'</strong> '+op+' '+th;}"
        "var ctype=c.type||'compare';"
        "if(ctype==='not'){var ch=c.child?formatRuleCondition(c.child):'Condition';if(ch.indexOf('If ')===0)ch=ch.substring(3);return 'If <strong>NOT</strong> ('+ch+')';}"
        "if(ctype==='and'||ctype==='or'){var opWord=ctype.toUpperCase();if(Array.isArray(c.children)&&c.children.length>0){var parts=c.children.map(function(k){var s=formatRuleCondition(k);return s.indexOf('If ')===0?s.substring(3):s;});return 'If ('+parts.join(' <strong>'+opWord+'</strong> ')+')';}return 'If <strong>'+opWord+' Gate</strong>';}"
        "if(ctype==='sensor_state'){return '<strong>'+getFriendlyInputName(c.input_id)+'</strong> Active';}"
        "if(ctype==='hysteresis'){var inH=getFriendlyInputName(c.input_id||e.input_id);return 'If <strong>'+inH+'</strong> Hysteresis ['+c.high_threshold+' / '+c.low_threshold+']';}"
        "if(ctype==='range'){var inR=getFriendlyInputName(c.input_id||e.input_id);return 'If <strong>'+inR+'</strong> in Range ['+c.min+' .. '+c.max+'] ('+(c.mode||'inside')+')';}"
        "if(ctype==='timer_on'){var chTxt=c.child?formatRuleCondition(c.child):('<strong>'+getFriendlyInputName(e.input_id)+'</strong>');if(chTxt.indexOf('If ')===0)chTxt=chTxt.substring(3);var ds=(c.delay_sec!=null?c.delay_sec:(c.delay_ms!=null?c.delay_ms/1000:5));return 'If ('+chTxt+') held for '+ds+'s (TON Timer)';}"
        "if(ctype==='pulse_timer'){var chTxt=c.child?formatRuleCondition(c.child):('<strong>'+getFriendlyInputName(e.input_id)+'</strong>');if(chTxt.indexOf('If ')===0)chTxt=chTxt.substring(3);var ps=(c.pulse_sec!=null?c.pulse_sec:(c.pulse_ms!=null?c.pulse_ms/1000:3));return 'If ('+chTxt+') Trigger Pulse '+ps+'s (TP Timer)';}"
        "if(ctype==='sr_latch'){var sc=c.set_condition;var rc=c.reset_condition;var sTxt=sc?formatRuleCondition(sc):'Set';var rTxt=rc?formatRuleCondition(rc):'Reset';if(sTxt.indexOf('If ')===0)sTxt=sTxt.substring(3);if(rTxt.indexOf('If ')===0)rTxt=rTxt.substring(3);return 'SR Latch: Set('+sTxt+') Reset('+rTxt+') ['+(c.priority||'reset').toUpperCase()+'-prio]';}"
        "var inC=getFriendlyInputName(c.input_id!=null?c.input_id:e.input_id);var opC=c.operator||e.operator||'==';"
        "var thC=(c.compare_mode==='input'||c.compare_mode===1||e.compare_mode===1||e.compare_mode==='input')?getFriendlyInputName(c.input_b_id!=null?c.input_b_id:e.input_b_id):(c.threshold!=null?c.threshold:(e.threshold!=null?e.threshold:0));"
        "return 'If <strong>'+inC+'</strong> '+opC+' '+thC;}"
        "function formatRuleAction(e){var outName=getFriendlyOutputName(e.output_id);var act=e.action||'ON';"
        "if(e.actuator_type===3||act==='SET_DUTY'){var d=(e.params&&e.params.duty!=null)?e.params.duty:(e.value!=null?e.value:0);"
        "var f=(e.params&&e.params.frequency!=null)?e.params.frequency:'';"
        "return 'Set <strong>'+outName+'</strong> Duty '+d+'%' +(f?' @ '+f+'Hz':'');}"
        "if(e.actuator_type===5||act==='SET_MA'){var ma=(e.params&&e.params.current_ma!=null)?e.params.current_ma:(e.value!=null?e.value:4);"
        "return 'Set <strong>'+outName+'</strong> to '+ma+' mA';}"
        "if(e.actuator_type===6||act==='SET_V'){var v=(e.params&&e.params.voltage_v!=null)?e.params.voltage_v:(e.value!=null?e.value:0);"
        "return 'Set <strong>'+outName+'</strong> to '+v+' V';}"
        "if(e.actuator_type===4||act==='MOVE'){var st=(e.params&&e.params.steps!=null)?e.params.steps:(e.steps!=null?e.steps:0);"
        "return 'Move <strong>'+outName+'</strong> '+st+' steps';}"
        "return 'Set <strong>'+outName+'</strong> to '+act;}"
        "function rollbackToVersion(ver){if(!ver)return;if(!confirm('Are you sure you want to rollback controller rules to version '+ver+'?'))return;"
        "fetch('/api/rules/rollback?version='+encodeURIComponent(ver),{method:'POST'}).then(function(r){return r.json()}).then(function(d){if(d.ok){showToast('Successfully rolled back to version '+ver);loadRulesTab()}else{showToast('Rollback failed: '+(d.error||'Unknown error'),!0)}}).catch(function(e){showToast('Error sending rollback request',!0)});}"
        "function rollbackSelectedVersion(){var sel=$('rules-rollback-select');if(sel&&sel.value)rollbackToVersion(sel.value);}"
        "function loadRulesTab(){fetch('/api/rules?t='+Date.now()).then(function(e){return e.json()}).then(function(e){"
        "window.loadedRules=e.rules||[],window.lastStatusData&&generateQR(window.lastStatusData);"
        "var t=$('rules-pending-banner');t&&(t.style.display=e.has_pending?'block':'none',e.has_pending&&($('pending-rules-version').innerText=e.pending_version||''));"
        "var tb=$('rules-testmode-banner');if(tb){var isTm=window.lastStatusData&&window.lastStatusData.test_mode;tb.style.display=isTm?'block':'none';}"
        "var a=!e.rules||0===e.rules.length;var curVer=a?'None':(e.version_id||'None');"
        "$('rules-version').innerText=curVer,$('rules-timestamp').innerText=a?'N/A':(e.timestamp||'N/A'),$('rules-status').innerText=a?'Inactive (No Rules)':(e.rules_valid?' Stable (Verified)':' Testing / Unverified');"
        "var n=$('rules-bypass-validation');n&&(n.checked=!!e.bypass_validation);"
        "var s,o=$('rules-diagram-container');"
        "o&&(o.innerHTML='',a?o.innerHTML=\"<div style='text-align:center;color:var(--muted);font-size:0.85rem;padding:20px 0'>No active rules.</div>\":(e.rules.forEach(function(r){"
        "if(r.active){var d=document.createElement('div');d.className='rule-flow-card',d.style='display:flex;align-items:center;background:var(--card2);border:1px solid var(--border);border-radius:6px;padding:6px 10px;margin-bottom:6px',"
        "d.innerHTML='<div style=\"background:rgba(76,165,133,.15);padding:4px 8px;border-radius:4px;color:var(--accent);font-weight:600;flex:1;text-align:center\">'+formatRuleCondition(r)+'</div><div style=\"padding:0 8px;font-size:.8rem;color:var(--muted)\">➔</div><div style=\"background:rgba(58,84,75,.15);padding:4px 8px;border-radius:4px;color:var(--accent2);font-weight:600;flex:1;text-align:center\">'+formatRuleAction(r)+'</div>',o.appendChild(d)}}),"
        "0===o.children.length&&(o.innerHTML=\"<div style='text-align:center;color:var(--muted);font-size:0.85rem;padding:20px 0'>All loaded rules are currently deactivated.</div>\")));"
        "(s=$('rules-table-body')).innerHTML='',a?s.innerHTML=\"<tr><td colspan='4' style='color:var(--muted)'>No active rules loaded.</td></tr>\":e.rules.forEach(function(r){"
        "var tr=document.createElement('tr');var condHtml=formatRuleCondition(r);var actHtml=formatRuleAction(r);"
        "tr.innerHTML='<td><code>'+r.rule_id+'</code></td><td>'+condHtml+'</td><td>'+actHtml+'</td><td><label class=\"switch\"><input type=\"checkbox\" '+(r.active?'checked':'')+' onchange=\"toggleRuleActive(\\''+r.rule_id+'\\',this.checked)\"><span class=\"slider\"></span></label></td>',s.appendChild(tr)});"
        "var rSel=$('rules-rollback-select');if(rSel){rSel.innerHTML='';}"
        "(s=$('rules-history-table-body')).innerHTML='';var hist=e.history||[];"
        "if(hist&&hist.length>0){hist.forEach(function(h){"
        "var isAct=(h.is_active||h.version_id===curVer);"
        "if(rSel&&!isAct){var opt=document.createElement('option');opt.value=h.version_id;opt.textContent=h.version_id+' ('+(h.rules_count||0)+' rules)';rSel.appendChild(opt);}"
        "var tr=document.createElement('tr');"
        "var actBtn=isAct?'<span class=\"badge\" style=\"background:#10b981;color:#fff;padding:2px 8px;border-radius:4px;font-size:0.75rem\">ACTIVE</span>':'<button class=\"btn btn-sm\" style=\"padding:2px 10px;font-size:0.75rem;cursor:pointer;background:rgba(245,158,11,0.15);border:1px solid #f59e0b;color:#f59e0b;border-radius:4px\" onclick=\"rollbackToVersion(\\''+h.version_id+'\\')\">Rollback</button>';"
        "tr.innerHTML='<td><code>'+h.version_id+'</code></td><td>'+(h.timestamp||'N/A')+'</td><td>'+(h.rules_count||0)+'</td><td>'+actBtn+'</td>',s.appendChild(tr)});"
        "if(rSel&&rSel.options.length===0){var opt=document.createElement('option');opt.value='';opt.textContent='No older versions';rSel.appendChild(opt);}}"
        "else{s.innerHTML=\"<tr><td colspan='4' style='color:var(--muted)'>No archived versions found.</td></tr>\";if(rSel){rSel.innerHTML=\"<option value=''>No versions</option>\";}}"
        "}).catch(function(e){})}"
    )

    new_text = text[:idx1] + replacement + text[idx2:]
    with open('web/index.html', 'w', encoding='utf-8') as f:
        f.write(new_text)
    print(f"Patched successfully! New text length: {len(new_text)}")

if __name__ == '__main__':
    patch()
