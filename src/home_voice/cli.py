import argparse
import asyncio
import getpass
import logging
import os
import ssl

from .config import Settings


def parser():
    root = argparse.ArgumentParser(description="Home voice assistant: desktop host and ESP32 audio bridge")
    commands = root.add_subparsers(dest="command", required=True)
    server = commands.add_parser("serve", help="Run the voice bridge")
    server.add_argument("--mock", action="store_true", help="Local tone test; no OpenAI requests")
    server.add_argument("--ask-key", action="store_true", help="Read an API key without echoing or saving it")
    server.add_argument("--host", default="127.0.0.1")
    server.add_argument("--port", type=int, default=8765)
    server.add_argument("--max-seconds", type=float, default=600)
    server.add_argument("--idle-seconds", type=float, default=60)
    server.add_argument("--cert", help="TLS certificate PEM for LAN clients")
    server.add_argument("--key", help="TLS private key PEM for LAN clients")
    commands.add_parser("devices", help="List microphone and speaker devices")
    panel = commands.add_parser("setup", help="Open the local PC setup service for the USB board")
    panel.add_argument("--port", default="COM14")
    usb = commands.add_parser("board-check", help="Check ESP32 microphones and optionally play a tone over USB")
    usb.add_argument("--port", default="COM14")
    usb.add_argument("--tone", action="store_true")
    usb.add_argument("--seconds", type=float, default=8)
    level = commands.add_parser("level", help="Measure microphone levels locally")
    level.add_argument("--seconds", type=float, default=5)
    talk = commands.add_parser("talk", help="Use a desktop microphone and speaker")
    for command in (level, talk):
        command.add_argument("--input", type=int, required=True, help="Input device index from devices")
        command.add_argument("--input-channels", type=int, default=1)
        command.add_argument("--channel", type=int, default=0, help="Zero-based processed microphone channel")
    talk.add_argument("--output", type=int, required=True, help="Output device index from devices")
    talk.add_argument("--output-channels", type=int, default=2)
    talk.add_argument("--transcripts", action="store_true", help="Print transient transcript fragments")
    talk.add_argument("--ca", help="Trusted CA certificate PEM for a wss:// bridge")
    probe = commands.add_parser("probe", help="Protocol check with silence (billable if service is live)")
    for command in (talk, probe):
        command.add_argument("--url", default="ws://127.0.0.1:8765/audio")
    return root


def main():
    args = parser().parse_args()
    logging.basicConfig(level=logging.WARNING, format="%(levelname)s: %(message)s")
    try:
        if args.command == "serve":
            from .bridge import serve_bridge
            settings = Settings.from_env(host=args.host, port=args.port, mock=args.mock,
                                         max_seconds=args.max_seconds, idle_seconds=args.idle_seconds)
            if args.ask_key and not args.mock:
                settings.api_key = getpass.getpass("OpenAI API key (not saved): ").strip()
            if not settings.mock and not settings.api_key:
                raise ValueError("No API key. Use serve --ask-key or set OPENAI_API_KEY on this computer.")
            context = None
            if bool(args.cert) != bool(args.key):
                raise ValueError("Supply both --cert and --key")
            if args.cert:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(args.cert, args.key)
            asyncio.run(serve_bridge(settings, context))
        elif args.command == "setup":
            from .panel import serve_panel
            serve_panel(args.port)
        elif args.command == "board-check":
            from .usb import diagnose
            if not 0 < args.seconds <= 60:
                raise ValueError("--seconds must be between 0 and 60")
            diagnose(args.port, args.seconds, args.tone)
        else:
            from .client import devices, microphone_level, probe, talk
            if args.command == "devices":
                devices()
            elif args.command == "level":
                if not 0 < args.seconds <= 60:
                    raise ValueError("--seconds must be between 0 and 60")
                microphone_level(args)
            elif args.command == "probe":
                asyncio.run(probe(args.url, os.getenv("VOICE_DEVICE_TOKEN", "")))
            else:
                context = ssl.create_default_context(cafile=args.ca) if args.ca else None
                asyncio.run(talk(args, os.getenv("VOICE_DEVICE_TOKEN", ""), context))
    except KeyboardInterrupt:
        print("Stopped.")
    except Exception as exc:
        # Do not print arbitrary transport exception text: it can contain handshake headers.
        if isinstance(exc, (ValueError, RuntimeError)):
            print(f"Error: {exc}")
        else:
            print(f"Error: {type(exc).__name__}. Check the connection and selected audio devices. "
                  "For unsupported 16 kHz audio, try a Windows MME device entry.")
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
