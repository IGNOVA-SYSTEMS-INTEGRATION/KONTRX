#!/usr/bin/env python3
"""
Patch web/index.html to include actuator type and pinouts in the QR code JSON payload,
and add a 'Copy QR JSON' button to the QR modal.
"""
import re
import sys

def patch():
    with open('web/index.html', 'r', encoding='utf-8') as f:
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
        # Insert right after closeQRModal
        html = html.replace("function closeQRModal(){var e=$(\"qr-modal\");e&&e.classList.add(\"hidden\")}",
                            "function closeQRModal(){var e=$(\"qr-modal\");e&&e.classList.add(\"hidden\")}" + copy_fn, 1)
        print("Added copyQRPayload function.")

    # 3. Replace generateQR function
    old_gen_pattern = re.compile(r'function generateQR\(e\)\{if\(e\)\{window\.lastStatusData=e;var t=\$\("qr-canvas"\);.*?drawQRFallback\(cb2,"QR Data Error"\)\}\}\}\}\}')
    m = old_gen_pattern.search(html)
    if not m:
        print("ERROR: could not locate generateQR function in web/index.html!")
        sys.exit(1)

    new_gen = (
        'function generateQR(e){'
        'if(e){'
        'window.lastStatusData=e;'
        'var t=$("qr-canvas");'
        'if(t){'
        'for(var a=[],n=[],s=[],o=window.sensors&&window.sensors.length?window.sensors:e.sensors||[],r=0;r<o.length;r++){'
        'var i=o[r],l={t:i.type,id:i.id},d=void 0!==sensorValues&&sensorValues[i.type+":"+i.id]?sensorValues[i.type+":"+i.id]:i;'
        'd&&d.valid&&null!=d.value&&d.value>-900&&(l.v=Math.round(100*d.value)/100,null!=d.temp&&d.temp>-50&&(l.tmp=Math.round(10*d.temp)/10)),a.push(l)'
        '}'
        'for(var c=window.relays&&window.relays.length?window.relays:e.relays||[],p=0;p<c.length;p++){'
        'var u=c[p],v={n:u.name||"A"+(p+1)};'
        'var at=void 0!==u.type?u.type:0,ch=u.channel||u.slave_id||1,pin="";'
        'var tName=tn[at]||(0==at?"Relay":7==at?"Digital Output":"Actuator");'
        'if(0==at){'
        'tName="Relay";'
        'pin=u.pin||(RELAY_PINS[p]||"PE2");'
        'v.s=u.state?1:0;'
        'u.nc&&(v.nc=1);'
        '}else if(7==at){'
        'tName="DO";'
        'pin=u.pin||(DO_PINS[p]||"PB0");'
        'v.s=u.state?1:0;'
        'u.nc&&(v.nc=1);'
        '}else if(3==at){'
        'tName="PWM";'
        'pin=1==ch?"PD12":(2==ch?"PD13":"CH"+ch);'
        'v.duty=void 0!==u.duty?u.duty:0;'
        '(u.freq||u.reg_addr)&&(v.freq=u.freq||u.reg_addr);'
        '}else if(4==at){'
        'tName="PTO";'
        'var ptoPins=["PUL:PE9,DIR:PE8","PUL:PE5,DIR:PE3","PUL:PC8,DIR:PC9","PUL:PA3,DIR:PA5"];'
        'pin=ptoPins[ch-1]||("Axis "+ch);'
        'v.pos=void 0!==u.position?u.position:0;'
        '(u.speed||u.reg_addr)&&(v.speed=u.speed||u.reg_addr);'
        '}else if(5==at){'
        'tName="4-20mA";'
        'pin=1==ch?"PA4":(2==ch?"PA5":"CH"+ch);'
        'v.current_ma=void 0!==u.current_ma?u.current_ma:4;'
        '}else if(6==at){'
        'tName="0-10V";'
        'pin=1==ch?"PD14":(2==ch?"PD15":"CH"+ch);'
        'v.voltage_v=void 0!==u.voltage_v?u.voltage_v:0;'
        '}else if(1==at){'
        'tName="Modbus TCP";'
        'pin=(u.ip||u.pin||"192.168.1.50")+":"+(u.port||502);'
        'v.slave_id=u.slave_id||1;'
        'u.reg_addr&&(v.reg_addr=u.reg_addr);'
        'v.s=u.state?1:0;'
        '}else if(2==at){'
        'tName="OPC UA";'
        'pin=(u.ip||u.pin||"192.168.1.60")+":"+(u.port||4840);'
        'u.opc_node_id&&(v.node_id=u.opc_node_id);'
        'v.s=u.state?1:0;'
        '}'
        'v.type=tName;'
        'v.pinout=pin;'
        'v.pinouts=pin;'
        'v.pin=pin;'
        'v.p=pin;'
        'n.push(v);'
        '}'
        'if(window.loadedRules&&window.loadedRules.length)'
        'for(var g=0;g<window.loadedRules.length;g++){'
        'var f=window.loadedRules[g];'
        'f.active&&s.push({in:(f.input_id||"")+(f.operator||"")+(f.threshold||0),out:(f.output_id||"")+"="+(f.action||"")})'
        '}'
        'var m={id:e.serial||"KX-0000001",ip:e.ip||"",fw:e.fw||"2.2.0",test:e.test_mode?1:0,s:a,a:n};'
        's.length&&(m.r=s);'
        'try{'
        'var y=JSON.stringify(m);'
        'window.lastQRPayload=y;'
        'if(y.length>1800)throw new Error("QR payload length limit exceeded");'
        'var h=nanoQR.generate(y);'
        'drawQRCanvas(t,h);'
        'var b=$("qr-modal-canvas");'
        'b&&drawQRCanvas(b,h)'
        '}catch(err){'
        'console.warn("Full QR generation failed, using compact fallback:",err);'
        'try{'
        'var cm={id:e.serial||"KX-0000001",ip:e.ip||"",fw:e.fw||"2.2.0",test:e.test_mode?1:0,sc:a.length,ac:n.length,rc:s.length};'
        'var cy=JSON.stringify(cm);'
        'window.lastQRPayload=cy;'
        'var ch=nanoQR.generate(cy);'
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

    html = html[:m.start()] + new_gen + html[m.end():]
    print("Replaced generateQR function successfully.")

    with open('web/index.html', 'w', encoding='utf-8') as f:
        f.write(html)
    print(f"Saved new index.html ({len(html)} bytes).")

if __name__ == '__main__':
    patch()
