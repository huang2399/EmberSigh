"""串口监视器 — 不触碰 DTR/RTS，仅被动读取"""
import sys, time, io
import serial

PORT = 'COM3'
BAUD = 115200
DUR  = int(sys.argv[1]) if len(sys.argv) > 1 else 25
OUT  = sys.argv[2] if len(sys.argv) > 2 else None

# dsrdtr=False + 打开后立刻清除 DTR/RTS，避免 USB-CDC 复位
ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.open()
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

buf = io.StringIO()
t0 = time.time()
while time.time() - t0 < DUR:
    try:
        data = ser.read(4096)
    except Exception as e:
        print('[read error]', e, file=sys.stderr)
        break
    if data:
        txt = data.decode('utf-8', errors='replace')
        buf.write(txt)
        sys.stdout.write(txt)
        sys.stdout.flush()

ser.close()

if OUT:
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(buf.getvalue())
    print(f'\n--- {len(buf.getvalue())} chars saved ---', file=sys.stderr)
