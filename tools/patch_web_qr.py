#!/usr/bin/env python3
"""
Patch web/index.html to ensure Board Identity QR code is completely static
with respect to dynamic sensor readings and actuator states.
The QR payload only contains identity and hardware IO configuration (types, pins, channels, order).
"""
import re
import sys
import os

INDEX_HTML = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web", "index.html")

def patch():
    with open(INDEX_HTML, 'r', encoding='utf-8') as f:
        html = f.read()

    print(f"Original index.html size: {len(html)} bytes")

    # 1. Update the QR Modal to add "Copy JSON" button
    old_modal = (
        "<canvas id='qr-modal-canvas' width='280' height='280' style='border-radius:10px;background:#fff;display:block;margin:0 auto'></canvas>"
        "<button class='btn btn-primary' style='margin-top:12px;width:100%' onclick='closeQRModal()'>Close</button>"
    )
    new_modal = (
        "<canvas id='qr-modal-canvas' width='280' height='280' style='border-radius:10px;background:#fff;display:block;margin:0 auto'></canvas>"
        "<div style='display:flex;gap:8px;margin-top:12px'>"
        "<button class='btn btn-secondary btn-sm' style='flex:1;justify-content:center' onclick='copyQRPayload()'>📋 Copy JSON</button>"
        "<button class='btn btn-primary btn-sm' style='flex:1;justify-content:center' onclick='closeQRModal()'>Close</button>"
        "</div>"
    )
    if old_modal in html:
        html = html.replace(old_modal, new_modal, 1)
        print("Updated QR modal markup with Copy JSON button.")
    else:
        print("Notice: old_modal markup not matched exactly, checking if already patched.")

    # 2. Add copyQRPayload function if not already present
    copy_fn = (
        "function copyQRPayload(){"
        "if(window.lastQRPayload){"
        "if(navigator.clipboard&&navigator.clipboard.writeText){"
        "navigator.clipboard.writeText(window.lastQRPayload).then(function(){alert('QR JSON copied to clipboard!')}).catch(function(){prompt('QR JSON:',window.lastQRPayload)})"
        "}else{prompt('QR JSON:',window.lastQRPayload)}"
        "}}"
    )
    if "function copyQRPayload" not in html:
        html = html.replace("function closeQRModal(){var e=$(\"qr-modal\");e&&e.classList.add(\"hidden\")}",
                            "function closeQRModal(){var e=$(\"qr-modal\");e&&e.classList.add(\"hidden\")}" + copy_fn, 1)
        print("Added copyQRPayload function.")

    # 3. Replace generateQR function
    gen_start = html.find("function generateQR(e){")
    if gen_start == -1:
        print("ERROR: could not locate generateQR function in web/index.html!")
        sys.exit(1)

    gen_end = html.find("function fileSelected(){", gen_start)
    if gen_end == -1:
        print("ERROR: could not locate end of generateQR function in web/index.html!")
        sys.exit(1)

    new_gen = (
        'function generateQR(e){'
        'if(e){'
        'window.lastStatusData=e;'
        'var t=$("qr-canvas");'
        'if(t){'
        'for(var a=[],n=[],s=[],o=window.sensors&&window.sensors.length?window.sensors:e.sensors||[],r=0;r<o.length;r++){'
        'var i=o[r],l={t:i.type,id:i.id,name:sensorTypeNames[i.type]||i.type,unit:sensorTypeUnits[i.type]||""};'
        'a.push(l)'
        '}'
        'for(var c=window.relays&&window.relays.length?window.relays:e.relays||[],p=0;p<c.length;p++){'
        'var u=c[p],at=void 0!==u.type?u.type:0,ch=u.channel||u.slave_id||1,pin="",tName=tn[at]||(0==at?"Relay":7==at?"DO":"Actuator"),v={n:u.name||"A"+(p+1)};'
        'if(0==at||7==at){'
        'tName=0==at?"Relay":"DO";'
        'pin=u.pin||(0==at?RELAY_PINS[p]||"PE2":DO_PINS[p]||"PE0");'
        'v.mode=u.nc?"NC":"NO";'
        '}else if(3==at){'
        'tName="PWM";'
        'pin=u.pin||(1==ch?"PD12":2==ch?"PD13":"PD12");'
        'v.ch=ch;v.channel=ch;'
        'v.freq=u.freq||u.reg_addr||1e3;'
        '}else if(4==at){'
        'tName="PTO";'
        'var ptoPuls=["PE9","PE5","PC8","PA3"],ptoDirs=["PE8","PE3","PC9","PA5"];'
        'var pulP=ptoPuls[ch-1]||"PE9",dirP=ptoDirs[ch-1]||"PE8";'
        'pin="PUL:"+pulP+",DIR:"+dirP;'
        'v.axis=ch;v.channel=ch;v.pul_pin=pulP;v.dir_pin=dirP;'
        'v.freq=u.speed||u.reg_addr||5e3;'
        '}else if(5==at){'
        'tName="4-20mA";'
        'pin=u.pin||(1==ch?"PA4":2==ch?"PA5":"PA4");'
        'v.ch=ch;v.channel=ch;'
        '}else if(6==at){'
        'tName="0-10V";'
        'pin=u.pin||(1==ch?"PD14":2==ch?"PD15":"PD14");'
        'v.ch=ch;v.channel=ch;'
        '}else if(1==at){'
        'tName="Modbus TCP";'
        'pin=(u.ip||u.pin||"192.168.1.50")+":"+(u.port||502);'
        'v.ip=u.ip||u.pin||"192.168.1.50";v.port=u.port||502;'
        'v.slave_id=u.slave_id||1;v.reg_addr=u.reg_addr||0;'
        '}else if(2==at){'
        'tName="OPC UA";'
        'pin=(u.ip||u.pin||"192.168.1.60")+":"+(u.port||4840);'
        'v.ip=u.ip||u.pin||"192.168.1.60";v.port=u.port||4840;'
        'v.node_id=u.opc_node_id||"";'
        '}'
        'v.type=tName;v.pin=pin;v.pinout=pin;v.pinouts=pin;v.p=pin;'
        'n.push(v);'
        '}'
        'if(window.loadedRules&&window.loadedRules.length)'
        'for(var g=0;g<window.loadedRules.length;g++){'
        'var f=window.loadedRules[g];'
        'f.active&&s.push({in:(f.input_id||"")+(f.operator||"")+(f.threshold||0),out:(f.output_id||"")+"="+(f.action||"")})'
        '}'
        'var m={id:e.serial||"KX-0000001",ip:e.ip||"",fw:e.fw||"2.2.0",test:e.test_mode?1:0,edgeNodeId:e.sparkplug_edge_node_id||"",deviceId:e.sparkplug_device_id||"",s:a,a:n};'
        's.length&&(m.r=s);'
        'try{'
        'var y=JSON.stringify(m);'
        'if(y===window.lastQRPayload&&window.lastGeneratedQR)return;'
        'window.lastQRPayload=y;'
        'if(y.length>2400)throw new Error("QR payload length limit exceeded");'
        'var h=nanoQR.generate(y);'
        'window.lastGeneratedQR=h;'
        'drawQRCanvas(t,h);'
        'var b=$("qr-modal-canvas");'
        'b&&drawQRCanvas(b,h)'
        '}catch(err){'
        'console.warn("Full QR generation failed, using compact fallback:",err);'
        'try{'
        'var cm={id:e.serial||"KX-0000001",ip:e.ip||"",fw:e.fw||"2.2.0",test:e.test_mode?1:0,edgeNodeId:e.sparkplug_edge_node_id||"",deviceId:e.sparkplug_device_id||"",sc:a.length,ac:n.length,rc:s.length};'
        'var cy=JSON.stringify(cm);'
        'if(cy===window.lastQRPayload&&window.lastGeneratedQR)return;'
        'window.lastQRPayload=cy;'
        'var ch=nanoQR.generate(cy);'
        'window.lastGeneratedQR=ch;'
        'drawQRCanvas(t,ch);'
        'var cb=$("qr-modal-canvas");'
        'cb&&drawQRCanvas(cb,ch)'
        '}catch(err2){'
        'console.error("QR generation failed completely:",err2);'
        'drawQRFallback(t,"QR Data Error");'
        'var cb2=$("qr-modal-canvas");'
        'cb2&&drawQRFallback(cb2,"QR Data Error")'
        '}'
        '}'
        '}'
        '}'
        '}'
    )

    html = html[:gen_start] + new_gen + html[gen_end:]
    print("Replaced generateQR function successfully.")

    with open(INDEX_HTML, 'w', encoding='utf-8') as f:
        f.write(html)
    print(f"Saved new index.html ({len(html)} bytes).")

if __name__ == '__main__':
    patch()
