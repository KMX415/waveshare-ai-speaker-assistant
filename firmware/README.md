# Firmware build guide

The normal first-time-owner path is the [prebuilt firmware setup guide](../docs/getting-started.md).
This page is for building from source.

## Target and prerequisites

- Waveshare ESP32-S3-AUDIO-Board: 16 MB flash, 8 MB octal PSRAM.
- **ESP-IDF v5.5.2**, installed following [Espressif's instructions](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/).
- A USB data cable and the board's verified serial port.
- Internet access during the first build to download managed dependencies.

Use a checkout path without spaces, such as `C:\src\waveshare-ai-speaker-assistant`.
Open the ESP-IDF terminal after installing the toolchain:

```powershell
cd C:\src\waveshare-ai-speaker-assistant\firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM14 flash
```

Replace COM14 with your port. Close the Python setup server and serial monitors before flashing.

The checked-in defaults enable octal PSRAM, the correct flash size and partition layout,
Jarvis/Computer WakeNet models, separate WebSocket transmit locking, and encrypted NVS
support. The dependency lock pins managed components. The build includes the model image
in its flash arguments.

**Do not erase flash as a routine update step.** NVS includes settings and the storage
ownership marker; credentials occupy their own partition. Flashing does not provision eFuses.

## Optional Windows build helper

`build.ps1` is a convenience wrapper for an existing SDK installation at
`%USERPROFILE%\.cache\home-voice\esp-idf`, with tools in the adjacent `idf-tools`
directory and Python under `idf-tools\python_env\idf5.5_py3.12_env`.
It does not install ESP-IDF. Use the standard build above unless you have that layout.

```powershell
.\firmware\build.ps1
.\firmware\build.ps1 -Flash -Port COM14
```

It copies the source into a path without spaces. Edit this checkout, not the staged copy.

## Wiring used by this firmware

| Function | GPIO / device |
|:--|:--|
| I2C | SDA 11 / SCL 10 |
| I2S | MCLK 12 / BCLK 13 / LRCLK 14 / RX 15 / TX 16 |
| Capture | ES7210: reference, microphone 1, unused, microphone 2 |
| Playback | ES8311, duplicated 32-bit stereo slots |
| Amplifier enable | TCA9555 expander pin 8 |
| LEDs | Seven addressable LEDs on GPIO38; tested revision uses RGB order |
| BOOT | GPIO0 |

Audio is 16 kHz mono PCM16 internally. Echo cancellation uses microphone 1 and the
hardware playback reference. Microphone 2 is available for diagnosis; beamforming
is not implemented. WakeNet runs separately from capture.

## Partitions

| Partition | Offset | Size |
|:--|:--|:--|
| Ordinary NVS | 0x9000 | 0x6000 |
| PHY data | 0xf000 | 0x1000 |
| Application | 0x10000 | 0x400000 |
| Wake models | 0x410000 | 0x600000 |
| Encrypted credentials | 0xe00000 | 0x6000 |

See [SECURITY.md](../SECURITY.md) before provisioning storage or considering recovery.
Never use or publish another person's full-flash backup. No factory backup is distributed here.

Hardware reference: [Waveshare resources](https://docs.waveshare.com/ESP32-S3-AUDIO-Board/Resources-And-Documents).
