#!/usr/bin/env python3
"""
Kontrx Controller Relay Toggle Script
------------------------------------
This script continuously toggles the relays of the Kontrx Edge Gateway on and off.
By default:
  - Turns ON for 2 seconds.
  - Turns OFF for 1 second.
  - Repeated continuously.

Usage:
  python control_relays.py --ip 192.168.1.200 --relay all --on-time 2 --off-time 1

Requirements:
  - Python 3 (Uses only standard libraries, no extra installations needed)
"""

import sys
import time
import argparse
import urllib.request
import urllib.error

def set_relay_state(ip, relay, state):
    """
    Sends a POST request to the controller to change relay state.
    relay: 'all' or an integer string representing relay ID (0-9)
    state: 1 for ON, 0 for OFF
    """
    if relay == 'all':
        url = f"http://{ip}/api/relay/all?state={state}"
    else:
        url = f"http://{ip}/api/relay?id={relay}&state={state}"
        
    req = urllib.request.Request(url, method="POST")
    
    try:
        with urllib.request.urlopen(req, timeout=3) as response:
            if response.status == 200:
                return True, response.read().decode('utf-8', errors='ignore')
            else:
                return False, f"HTTP Status {response.status}"
    except urllib.error.URLError as e:
        return False, str(e.reason)
    except Exception as e:
        return False, str(e)

def main():
    parser = argparse.ArgumentParser(
        description="تلقائي: تشغيل وإطفاء الريليهات بشكل مستمر بفواصل زمنية."
    )
    parser.add_argument("--ip", type=str, default="192.168.1.200",
                        help="IP address of the Kontrx controller (Default: 192.168.1.200)")
    parser.add_argument("--relay", type=str, default="all",
                        help="Relay to control: 'all' or index 0-9 (Default: all)")
    parser.add_argument("--on-time", type=float, default=2.0,
                        help="Duration in seconds to keep relays ON (Default: 2.0)")
    parser.add_argument("--off-time", type=float, default=1.0,
                        help="Duration in seconds to keep relays OFF (Default: 1.0)")
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("           Kontrx Relay Toggle Controller")
    print("=" * 60)
    print(f"[*] Target IP:      {args.ip}")
    print(f"[*] Relay Choice:   {args.relay}")
    print(f"[*] ON Duration:    {args.on_time} seconds")
    print(f"[*] OFF Duration:   {args.off_time} seconds")
    print("[*] Press Ctrl+C to stop the script safely.")
    print("=" * 60)

    try:
        iteration = 1
        while True:
            # 1. Turn ON
            print(f"[{time.strftime('%H:%M:%S')}] #{iteration} | Sending ON Command (state=1)...", end="", flush=True)
            success, response = set_relay_state(args.ip, args.relay, 1)
            if success:
                print(" [OK]")
            else:
                print(f" [FAILED] ({response})")
            
            time.sleep(args.on_time)
            
            # 2. Turn OFF
            print(f"[{time.strftime('%H:%M:%S')}] #{iteration} | Sending OFF Command (state=0)...", end="", flush=True)
            success, response = set_relay_state(args.ip, args.relay, 0)
            if success:
                print(" [OK]")
            else:
                print(f" [FAILED] ({response})")
                
            time.sleep(args.off_time)
            iteration += 1
            
    except KeyboardInterrupt:
        print("\n\n[*] Script stopped by user (Ctrl+C).")
        # Optional: Attempt to turn everything OFF before exit for safety
        print("[*] Turning all relays OFF for safety...", end="", flush=True)
        success, response = set_relay_state(args.ip, args.relay, 0)
        if success:
            print(" [OK]")
        else:
            print(f" [FAILED] ({response})")
        print("[*] Exiting. Good bye!")
        sys.exit(0)

if __name__ == "__main__":
    main()
