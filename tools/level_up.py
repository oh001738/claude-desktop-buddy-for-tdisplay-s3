#!/usr/bin/env python3
"""
Spoof token counts to force Buddy to level up.
Usage: python3 level_up.py [levels_to_add]
"""
import json, time, serial, glob, sys

def main():
    levels = 1
    if len(sys.argv) > 1:
        try:
            levels = int(sys.argv[1])
        except ValueError:
            print("Usage: python3 level_up.py [levels_to_add]")
            sys.exit(1)

    ports = glob.glob('/dev/cu.usbserial-*') + glob.glob('/dev/cu.usbmodem*')
    if not ports:
        sys.exit("No Buddy device found on USB.")
    
    port = ports[0]
    print(f"Connecting to {port}...")
    try:
        s = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        sys.exit(f"Failed to connect: {e}")

    # The firmware requires two packets to compute a delta if the bridge restarted.
    # We send tokens:0 to sync the baseline, then tokens:N to trigger the delta.
    
    print("Syncing baseline tokens...")
    sync_payload = {"tokens": 0}
    s.write((json.dumps(sync_payload) + "\n").encode())
    time.sleep(0.5)

    tokens_to_add = levels * 50000
    print(f"Sending {tokens_to_add} tokens to trigger {levels} level(s) up...")
    
    level_payload = {"tokens": tokens_to_add}
    s.write((json.dumps(level_payload) + "\n").encode())
    time.sleep(1.0)
    
    print("Done! Check your Buddy device.")

if __name__ == "__main__":
    main()
