#!/usr/bin/env python3
"""
Patch web/index.html so that multi_us sensors are rendered in buildSensorCards()
on the dashboard, including the 8-channel ultrasonic transducer breakdown.
"""
import re
import sys

def patch():
    with open('web/index.html', 'r', encoding='utf-8') as f:
        html = f.read()

    print(f"Original index.html size: {len(html)} bytes")

    old_fn = (
        'function buildSensorCards(){var e=$("sensor-cards");if(e){for(var t=document.querySelectorAll("#sensor-cards .dyn-card"),a=0;a<t.length;a++)t[a].parentNode.removeChild(t[a]);for(a=0;a<sensors.length;a++){var n=sensors[a];if("multi_us"!==n.type){var s=sensorValues[n.type+":"+n.id],o=s&&s.valid?s.value:null,r=s&&s.valid?s.temp:null,i=sensorTypeUnits[n.type]||"",l=sensorTypeNames[n.type]||n.type,d=s&&s.valid?"":\'<span style="font-size:.65rem;padding:1px 6px;border-radius:4px;background:rgba(231,76,60,0.15);color:#e74c3c;font-weight:600;margin-left:6px">Offline</span>\',c=document.createElement("div");c.className="card dyn-card",c.innerHTML=\'<div class="card-title">\'+l+" #"+n.id+d+\'</div><div><span class="card-val">\'+(null!=o?fmtF(o):"--")+\'</span><span class="card-unit">\'+i+\'</span></div><div class="card-sub">Temp:\'+(null!=r?fmtF(r):"--")+" C</div>",e.appendChild(c)}}}}'
    )

    new_fn = (
        'function buildSensorCards(){'
        'var e=$("sensor-cards");'
        'if(e){'
        'for(var t=document.querySelectorAll("#sensor-cards .dyn-card"),a=0;a<t.length;a++)t[a].parentNode.removeChild(t[a]);'
        'for(a=0;a<sensors.length;a++){'
        'var n=sensors[a];'
        'var s=sensorValues[n.type+":"+n.id],o=s&&s.valid?s.value:null,r=s&&s.valid?s.temp:null,i=sensorTypeUnits[n.type]||"",l=sensorTypeNames[n.type]||n.type,d=s&&s.valid?"":\'<span style="font-size:.65rem;padding:1px 6px;border-radius:4px;background:rgba(231,76,60,0.15);color:#e74c3c;font-weight:600;margin-left:6px">Offline</span>\',c=document.createElement("div");'
        'c.className="card dyn-card";'
        'if("multi_us"===n.type){'
        'var mus=lastStatusData&&lastStatusData.multi_us?lastStatusData.multi_us:[];'
        'var mu=mus.find?mus.find(function(m){return m.id===n.id}):null;'
        'if(!mu){for(var mi=0;mi<mus.length;mi++){if(mus[mi].id===n.id){mu=mus[mi];break;}}}'
        'var distH="";'
        'if(mu&&mu.dist&&mu.dist.length){'
        'distH=\'<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:4px;margin:8px 0;font-size:.7rem;background:var(--card2);padding:6px;border-radius:6px;border:1px solid var(--border)">\';'
        'for(var k=0;k<8;k++){'
        'var dk=mu.dist[k]>-900?fmtI(mu.dist[k]):"--";'
        'distH+=\'<div style="text-align:center"><span style="color:var(--muted);font-size:.62rem">U\'+(k+1)+\':</span> <strong>\'+dk+\'</strong></div>\';'
        '}'
        'distH+=\'</div>\';'
        '}'
        'var valStr=null!=o?fmtF(o):(mu&&mu.avg>-900?fmtI(mu.avg):"--");'
        'var tmpStr=null!=r?fmtF(r):(mu&&mu.temp>-900?fmtF(mu.temp):"--");'
        'c.innerHTML=\'<div class="card-title">🔊 \'+l+" #"+n.id+d+\'</div><div><span class="card-val">\'+valStr+\'</span><span class="card-unit">\'+i+\'</span></div>\'+distH+\'<div class="card-sub">Avg: \'+(mu&&mu.avg>-900?fmtI(mu.avg):valStr)+\' mm &nbsp; Temp: \'+tmpStr+\' °C</div>\';'
        '}else{'
        'c.innerHTML=\'<div class="card-title">\'+l+" #"+n.id+d+\'</div><div><span class="card-val">\'+(null!=o?fmtF(o):"--")+\'</span><span class="card-unit">\'+i+\'</span></div><div class="card-sub">Temp:\'+(null!=r?fmtF(r):"--")+" °C</div>";'
        '}'
        'e.appendChild(c);'
        '}'
        '}'
        '}'
    )

    if old_fn not in html:
        # Match using regex if encoding of C differs
        pattern = re.compile(r'function buildSensorCards\(\)\{var e=\$\("sensor-cards"\);if\(e\)\{for\(var t=document\.querySelectorAll\("#sensor-cards \.dyn-card"\),a=0;a<t\.length;a\+\+\)t\[a\]\.parentNode\.removeChild\(t\[a\]\);for\(a=0;a<sensors\.length;a\+\+\)\{var n=sensors\[a\];if\("multi_us"!==n\.type\)\{.*?e\.appendChild\(c\)\}\}\}\}')
        m = pattern.search(html)
        if not m:
            print("ERROR: could not find buildSensorCards in web/index.html")
            sys.exit(1)
        html = html[:m.start()] + new_fn + html[m.end():]
        print("Replaced buildSensorCards via regex pattern.")
    else:
        html = html.replace(old_fn, new_fn, 1)
        print("Replaced buildSensorCards via exact match.")

    with open('web/index.html', 'w', encoding='utf-8') as f:
        f.write(html)
    print(f"Saved new index.html ({len(html)} bytes).")

if __name__ == '__main__':
    patch()
