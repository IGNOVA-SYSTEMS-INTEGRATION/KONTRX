#!/usr/bin/env python3
"""
Patch web/index.html and tools/patch_web_qr.py to include firmware version in QR payload.
"""

def update_index_html():
    with open('web/index.html', 'r', encoding='utf-8') as f:
        c = f.read()

    target1 = 'var m={id:e.serial||"KX-0000001",ip:e.ip||"",test:e.test_mode?1:0,s:a,a:n};'
    repl1 = 'var m={id:e.serial||"KX-0000001",ip:e.ip||"",fw:e.fw||"2.2.0",test:e.test_mode?1:0,s:a,a:n};'

    target2 = 'var cm={id:e.serial||"KX-0000001",ip:e.ip||"",test:e.test_mode?1:0,sc:a.length,ac:n.length,rc:s.length};'
    repl2 = 'var cm={id:e.serial||"KX-0000001",ip:e.ip||"",fw:e.fw||"2.2.0",test:e.test_mode?1:0,sc:a.length,ac:n.length,rc:s.length};'

    if target1 in c and target2 in c:
        c = c.replace(target1, repl1).replace(target2, repl2)
        with open('web/index.html', 'w', encoding='utf-8') as f:
            f.write(c)
        print('[+] Successfully patched web/index.html with QR fw version.')
    else:
        print('[-] Targets not found in web/index.html (already patched?)')

def update_patch_web_qr():
    with open('tools/patch_web_qr.py', 'r', encoding='utf-8') as f:
        c = f.read()

    target1 = "'var m={id:e.serial||\"KX-0000001\",ip:e.ip||\"\",test:e.test_mode?1:0,s:a,a:n};'"
    repl1 = "'var m={id:e.serial||\"KX-0000001\",ip:e.ip||\"\",fw:e.fw||\"2.2.0\",test:e.test_mode?1:0,s:a,a:n};'"

    target2 = "'var cm={id:e.serial||\"KX-0000001\",ip:e.ip||\"\",test:e.test_mode?1:0,sc:a.length,ac:n.length,rc:s.length};'"
    repl2 = "'var cm={id:e.serial||\"KX-0000001\",ip:e.ip||\"\",fw:e.fw||\"2.2.0\",test:e.test_mode?1:0,sc:a.length,ac:n.length,rc:s.length};'"

    if target1 in c and target2 in c:
        c = c.replace(target1, repl1).replace(target2, repl2)
        with open('tools/patch_web_qr.py', 'w', encoding='utf-8') as f:
            f.write(c)
        print('[+] Successfully patched tools/patch_web_qr.py.')
    else:
        print('[-] Targets not found in tools/patch_web_qr.py')

if __name__ == '__main__':
    update_index_html()
    update_patch_web_qr()
