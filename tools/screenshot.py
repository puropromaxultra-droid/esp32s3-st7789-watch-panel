"""串口截图：从板子导出帧缓冲，存成 PNG。

用法：
    python tools/screenshot.py            # 截当前画面 → tools/screen.png
    python tools/screenshot.py z          # 让板子重画开机 logo 再截 → tools/bootlogo.png
    python tools/screenshot.py Z COM15    # 指定命令与端口

板子端命令（weather_clock.ino）：
    Z = 导出当前帧缓冲
    z = 先重画开机画面（Windows 98 风格）再导出

协议：先一行 "[fb] 240 240"，随后每行 240 个像素的 4 位 hex（RGB565），最后 "[fb] end"。
"""
import sys, time, io
import serial
import serial.tools.list_ports as lp
from PIL import Image


def pick_port():
    for p in lp.comports():
        if (p.vid or 0) in (0x303A, 0x1A86, 0x10C4, 0x0403):
            return p.device
    raise SystemExit('没找到板子串口（VID 303A/1A86/10C4/0403）')


def main():
    cmd = (sys.argv[1] if len(sys.argv) > 1 else 'Z')
    port = sys.argv[2] if len(sys.argv) > 2 else pick_port()
    out = 'tools/bootlogo.png' if cmd == 'z' else 'tools/screen.png'

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(cmd.encode())

    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < 40:
        d = s.read(65536)
        if d:
            buf.extend(d)
            if b'[fb] end' in buf:
                break
    s.close()

    txt = bytes(buf).decode('utf-8', 'replace')
    if '[fb]' not in txt:
        raise SystemExit('没收到帧缓冲数据：%r' % txt[-300:])

    body = txt.split('[fb]', 1)[1]
    lines = body.splitlines()
    # 第一行是尺寸，之后是像素行
    dims = lines[0].split()
    W, H = int(dims[0]), int(dims[1])
    rows = []
    for l in lines[1:]:
        l = l.strip()
        # ★ 播放/推流时会有日志插进来，一行可能被拼接成更长；只要前 W*4 个字符都是 hex 就收
        if len(l) >= W * 4 and all(c in '0123456789abcdefABCDEF' for c in l[:W * 4]):
            rows.append(l[:W * 4])
            if len(rows) == H:
                break
    if len(rows) != H:
        raise SystemExit('像素行不足：%d/%d' % (len(rows), H))

    img = Image.new('RGB', (W, H))
    px = img.load()
    for y, row in enumerate(rows):
        for x in range(W):
            v = int(row[x * 4:x * 4 + 4], 16)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            px[x, y] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)

    img.save(out)
    img.resize((W * 2, H * 2), Image.NEAREST).save(out.replace('.png', '_2x.png'))
    print('已保存 %s（%dx%d）' % (out, W, H))


if __name__ == '__main__':
    main()
