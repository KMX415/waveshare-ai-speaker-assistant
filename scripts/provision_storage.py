"""Explicit, interactive, one-time encrypted-storage provisioning for a new board."""
import argparse
import json
import time
from home_voice.usb import Decoder, encode_packet, open_board

CONFIRMATION = "PROVISION KEY SLOT 5"

def exchange(board, command, expected, timeout=8):
    decoder = Decoder()
    board.write(encode_packet(1, command))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for kind, payload in decoder.feed(board.read(2048)):
            if kind != 3:
                continue
            try:
                event = json.loads(payload)
            except (ValueError, UnicodeDecodeError):
                continue
            if event.get("type") in expected:
                return event
    raise RuntimeError("No expected response. Check the port and installed firmware.")

def provision(port, confirm=input):
    with open_board(port) as board:
        identity = exchange(board, b"I", {"device"})
        if not identity.get("firmware", "").startswith("home-voice-standalone-"):
            raise RuntimeError("This is not the expected standalone firmware.")
        state = exchange(board, b"N", {"device_state"})
        if state.get("active"):
            raise RuntimeError("Stop the active conversation first.")
        if state.get("secure_storage"):
            print("Encrypted storage is already available. No hardware changes made.")
            return False
        print("This permanently writes an HMAC secret into ESP32 eFuse key slot 5.")
        print("It cannot be undone. Normal firmware updates and API-key replacement remain available.")
        print("It protects copied API-key flash; it does not enable secure boot or encrypt Wi-Fi settings.")
        if confirm("Type " + CONFIRMATION + " to proceed: ").strip() != CONFIRMATION:
            print("Cancelled. No provisioning command was sent.")
            return False
        # Only this exact interactive confirmation authorizes the irreversible command.
        result = exchange(board, b"EENABLE-HARDWARE-KEY-STORAGE",
                          {"storage_provisioned", "storage_error"}, timeout=15)
        if result["type"] != "storage_provisioned":
            raise RuntimeError("Provisioning refused or failed. Do not retry with another key slot.")
    print("Provisioning acknowledged. Waiting for the board to restart...")
    time.sleep(3)
    with open_board(port) as board:
        state = exchange(board, b"N", {"device_state"}, timeout=15)
        if not state.get("secure_storage"):
            raise RuntimeError("Provisioning was acknowledged, but storage readiness was not confirmed. Do not re-provision.")
    print("Encrypted storage is ready. You can now save your key through the setup page.")
    return True

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="The verified USB serial port of your speaker")
    args = parser.parse_args()
    try:
        provision(args.port)
    except (KeyboardInterrupt, EOFError):
        print("Cancelled.")
        raise SystemExit(1)
    except Exception as exc:
        if isinstance(exc, RuntimeError):
            print(str(exc))
        else:
            print(type(exc).__name__ + ": check the serial connection and close other serial applications.")
        raise SystemExit(1)

if __name__ == "__main__":
    main()
