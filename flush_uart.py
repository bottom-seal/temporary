import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"

with serial.Serial(port, 115200, timeout=0.1) as ser:
    ser.reset_input_buffer()   # clear data coming from board to PC
    ser.reset_output_buffer()  # clear data waiting from PC to board
    time.sleep(0.1)
    print(f"Flushed host-side buffers on {port}")
