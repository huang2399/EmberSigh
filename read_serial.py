import serial, time
s = serial.Serial('COM3', 115200, timeout=2)
time.sleep(4)
buf = s.read(8192)
s.close()
lines = buf.decode('utf-8', errors='replace').split(chr(10))
for l in lines:
    print(l.strip())
