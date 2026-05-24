import struct
import sys


def upload_kernel(connection_path ,kernel_path):
    with open(kernel_path, "rb") as f:
        kernel_data = f.read()
        
    header = struct.pack('<II',
        0x544F4F42,           
        len(kernel_data),     
    )
    
    with open(connection_path, "wb", buffering=0) as tty:
        tty.write(header)
        tty.write(kernel_data)
    
    print(f"success, sent {len(kernel_data)} bytes")

if __name__ == "__main__":
    connection_path = sys.argv[1]
    upload_kernel(connection_path, "./kernel/build/kernel.bin")
