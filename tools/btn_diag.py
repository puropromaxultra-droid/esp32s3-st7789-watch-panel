import serial, time
s = serial.Serial(); s.port = 'COM15'; s.baudrate = 115200
s.timeout = 0.2; s.dtr = False; s.rts = False
s.open()
time.sleep(14)
s.reset_input_buffer()

def rd(d=0.8):
    b = bytearray(); t0 = time.time()
    while time.time() - t0 < d:
        x = s.read(4096)
        if x: b.extend(x)
    return bytes(b).decode('utf-8', 'replace').strip()

def ask(c, d=0.9):
    s.reset_input_buffer()
    s.write(c if isinstance(c, bytes) else c.encode())
    return rd(d)

print('--- 1. 清零（读的是启动以来的累计）---')
print(ask('e'))

print('\n--- 2. 静置 5 秒 ---')
t0 = time.time()
time.sleep(5)
print('实际 %.2fs' % (time.time() - t0))
print(ask('e'))

print('\n--- 3. 注入 3 次 B 应翻 3 页 ---')
print(ask('P'))
for i in range(3):
    s.write(b'B'); time.sleep(0.5)
print(ask('P'))

print('\n--- 4. 双键注入（回时钟）---')
s.write(b'H'); time.sleep(1.2)
print(ask('P'))
s.close()
