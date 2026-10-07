#!/usr/bin/env python3
"""
Patch web/index.html with missing JS functions, styles, and OTA workflow.
"""
import re
import sys

def patch_index_html():
    with open('web/index.html', 'r', encoding='utf-8') as f:
        html = f.read()

    print(f"Original length: {len(html)}")

    # 1. Add CSS styles before </style>
    css_to_add = (
        ".mode-toggle-btn{background:var(--card2);border:1px solid var(--border);color:var(--muted);"
        "border-radius:6px;padding:6px 12px;font-size:.78rem;font-weight:600;cursor:pointer;transition:all .2s ease}"
        ".mode-toggle-btn:hover{color:var(--text);border-color:var(--accent)}"
        ".mode-toggle-btn.active{background:var(--accent);border-color:var(--accent);color:#fff;box-shadow:0 2px 6px rgba(76,165,133,.3)}"
        ".palette-item{background:var(--card);border:1px solid var(--border);border-radius:6px;padding:4px 10px;"
        "font-size:.75rem;font-weight:600;cursor:grab;display:inline-flex;align-items:center;gap:6px;color:var(--text);"
        "user-select:none;transition:all .15s ease}.palette-item:hover{border-color:var(--accent);background:var(--card2);"
        "transform:translateY(-1px);box-shadow:0 2px 4px rgba(0,0,0,.06)}.palette-item .add-btn{color:var(--accent);"
        "font-weight:bold;cursor:pointer;font-size:.85rem}.field-pill{background:var(--card2);border:1px solid var(--border);"
        "color:var(--text);border-radius:6px;padding:3px 8px;font-size:.72rem;font-weight:600;cursor:pointer;"
        "transition:all .15s ease}.field-pill:hover{border-color:var(--accent);color:var(--accent)}"
    )
    if ".mode-toggle-btn{" not in html:
        html = html.replace("</style>", css_to_add + "</style>")

    # 2. Add functions in <script>
    js_funcs = """
function switchPayloadMode(mode){
    var isBuilder=("builder"===mode);
    var bPanel=$("builder-mode-panel"),jPanel=$("json-mode-panel");
    var bBtn=$("btn-mode-builder"),jBtn=$("btn-mode-json");
    if(bPanel)bPanel.style.display=isBuilder?"block":"none";
    if(jPanel)jPanel.style.display=isBuilder?"none":"block";
    if(bBtn){if(isBuilder)bBtn.classList.add("active");else bBtn.classList.remove("active")}
    if(jBtn){if(isBuilder)jBtn.classList.remove("active");else jBtn.classList.add("active")}
    updateLivePreview()}
function buildMqttPalette(){
    var pList=$("palette-list");if(!pList)return;
    pList.innerHTML="";
    var standardFields=[
        {id:"timestamp",label:"Timestamp",icon:"⏱"},
        {id:"device_id",label:"Device ID",icon:"🆔"},
        {id:"sensors",label:"All Sensors",icon:"🌡"},
        {id:"relays",label:"All Actuators",icon:"⚡"},
        {id:"sys_cpu",label:"CPU Usage",icon:"💻"},
        {id:"sys_heap",label:"Heap RAM",icon:"🧠"},
        {id:"pto",label:"PTO Motion",icon:"⚙"},
        {id:"pwm",label:"PWM Out",icon:"〰"},
        {id:"analog_v",label:"0-10V Out",icon:"🔌"},
        {id:"analog_ma",label:"4-20mA Out",icon:"🔋"}
    ];
    standardFields.forEach(function(item){
        var el=document.createElement("div");
        el.className="palette-item";el.draggable=true;
        el.ondragstart=function(ev){ev.dataTransfer.setData("text/plain",item.id)};
        el.onclick=function(){addField(item.id)};
        el.innerHTML="<span>"+item.icon+" "+item.label+"</span><span class='add-btn'>+</span>";
        pList.appendChild(el)});
    if(sensors&&sensors.length>0){
        sensors.forEach(function(s){
            var sName=sensorTypeNames[s.type]||s.type;
            var sid=s.type+"_"+s.id;
            var el=document.createElement("div");
            el.className="palette-item";el.draggable=true;
            el.ondragstart=function(ev){ev.dataTransfer.setData("text/plain",sid)};
            el.onclick=function(){addField(sid)};
            el.innerHTML="<span>📡 "+sName+" #"+s.id+"</span><span class='add-btn'>+</span>";
            pList.appendChild(el)})}}
function insertPlaceholder(tag){
    var t=$("mqtt-custom-json-template");if(!t)return;
    var start=t.selectionStart||t.value.length,end=t.selectionEnd||t.value.length;
    t.value=t.value.substring(0,start)+tag+t.value.substring(end);
    t.selectionStart=t.selectionEnd=start+tag.length;
    t.focus();updateLivePreview()}
function refreshSDExplorer(){browseSDFolder(window.currentSDPath||"/")}
function toggleMqttTx(enabled){
    fetch("/api/config/mqtt/toggle",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({enabled:enabled?1:0})}).then(function(r){return r.json()}).then(function(d){showToast("MQTT Telemetry "+(enabled?"enabled":"paused"));if(lastStatusData&&lastStatusData.mqtt)lastStatusData.mqtt.tx_enabled=enabled?1:0}).catch(function(){showToast("Error updating MQTT transmission",true)})}
function toggleMQTTSkipOffline(checked){
    fetch("/api/config/mqtt/skip_offline",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({skip_offline:checked?1:0})}).then(function(r){return r.json()}).then(function(d){showToast("Skip Offline Sensors "+(checked?"enabled":"disabled"));if(lastStatusData&&lastStatusData.mqtt)lastStatusData.mqtt.skip_offline=checked?1:0}).catch(function(){showToast("Error updating Skip Offline Sensors",true)})}
"""
    # Replace empty toggleMQTTSkipOffline(v){}
    html = re.sub(r'function toggleMQTTSkipOffline\(v\)\{\}', '', html)

    # Insert functions right after <script>
    html = html.replace("<script>", "<script>" + js_funcs)

    # 3. Update showTab to initialize MQTT when clicked
    html = html.replace('"settings"===t){', '"mqtt"===t&&(buildMqttPalette(),renderActiveLayout(),updateLivePreview()),"settings"===t){')

    # 4. Replace uploadFirmware with full OTA verification
    new_upload_firmware = """function uploadFirmware(){
    var fileInput=$("fw-file"),file=fileInput._file||fileInput.files[0];
    if(!file)return;
    var btn=$("btn-flash");btn.disabled=true;
    var bar=$("ota-bar"),pct=$("ota-pct");
    pct.textContent="Authenticating OTA...";
    fetch("/api/ota/verify",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({otp:"KontrxOTA2026"})}).then(function(res){
        if(!res.ok)throw new Error("OTP verification failed");
        pct.textContent="Preparing flash sectors...";
        return fetch("/api/ota/prepare",{method:"POST"})
    }).then(function(res){
        if(200!==res.status)throw new Error("Flash erase failed");
        pct.textContent="Erasing flash (waiting 3s)...";
        setTimeout(function(){
            var xhr=new XMLHttpRequest();
            xhr.open("POST","/update",true);
            xhr.setRequestHeader("Content-Type","application/octet-stream");
            if(authToken){xhr.setRequestHeader("X-Auth-Token",authToken)}
            xhr.upload.onprogress=function(ev){
                if(ev.lengthComputable){
                    var percent=Math.round(ev.loaded/ev.total*100);
                    bar.style.width=percent+"%";
                    pct.textContent="Uploading: "+percent+"% ("+Math.round(ev.loaded/1024)+" / "+Math.round(ev.total/1024)+" KB)"}};
            xhr.onreadystatechange=function(){
                if(4===xhr.readyState){
                    if(200===xhr.status){
                        bar.style.width="100%";
                        pct.textContent="Upload complete!";
                        showMsg("ota-msg"," Firmware received. Device rebooting into bootloader...","ok");
                        setTimeout(function(){
                            pct.textContent="Device rebooting...";
                            setTimeout(function(){window.location.reload()},6000)},1000)
                    }else{
                        var errDetail=xhr.responseText||xhr.statusText;
                        pct.textContent="Upload failed: "+xhr.status;
                        showMsg("ota-msg"," Upload failed: "+errDetail+" (HTTP "+xhr.status+")","err");
                        btn.disabled=false}}};
            xhr.onerror=function(){
                pct.textContent="Network error during upload!";
                showMsg("ota-msg"," Connection lost during upload.","err");
                btn.disabled=false};
            xhr.send(file)
        },3000)
    }).catch(function(err){
        pct.textContent="OTA initialization failed!";
        showMsg("ota-msg"," "+(err.message||"Failed to initialize OTA"),"err");
        btn.disabled=false})}"""

    # Replace old uploadFirmware
    m_uf = re.search(r'function uploadFirmware\(\)\{.*?btn-flash.*?\}(?=\s*function|\s*<\/script>)', html, re.DOTALL)
    if m_uf:
        html = html[:m_uf.start()] + new_upload_firmware + html[m_uf.end():]
        print("Replaced uploadFirmware function successfully.")
    else:
        print("Warning: regex for uploadFirmware didn't match cleanly, finding by index...")
        idx = html.find('function uploadFirmware(){')
        end_idx = html.find('function fetchSystemLogs()', idx)
        if idx != -1 and end_idx != -1:
            html = html[:idx] + new_upload_firmware + "\n" + html[end_idx:]
            print(f"Replaced uploadFirmware by index {idx} to {end_idx}.")

    with open('web/index.html', 'w', encoding='utf-8') as f:
        f.write(html)

    print(f"New length: {len(html)}")

if __name__ == "__main__":
    patch_index_html()
