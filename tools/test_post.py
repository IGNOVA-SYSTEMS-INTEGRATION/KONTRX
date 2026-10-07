import socket

s = socket.socket()
s.settimeout(5)
s.connect(('192.168.1.200', 80))
body = b'{"username":"admin","password":"adminkontrx"}'
req = (b'POST /api/auth/login HTTP/1.1\r\n'
       b'Host: 192.168.1.200\r\n'
       b'Content-Type: application/json\r\n'
       b'Content-Length: ' + str(len(body)).encode() + b'\r\n'
       b'Connection: close\r\n\r\n' + body)
s.sendall(req)
data = b''
while True:
    try:
        chunk = s.recv(1024)
        if not chunk:
            print("Server closed connection.")
            break
        data += chunk
    except Exception as e:
        print('recv err:', e)
        break
s.close()
print('Total Received:', len(data))
print(data.decode('latin1', 'ignore'))
