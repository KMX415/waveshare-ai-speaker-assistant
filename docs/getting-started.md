# First-time setup

This guide is for someone starting with a new **[Waveshare ESP32-S3-AUDIO-Board speaker](https://amzn.to/3Tecbhp)**. Set aside about 30–45 minutes. The steps below use Windows; other systems can build with ESP-IDF and run the Python utilities, but their first-time setup has not been validated here.

![The Waveshare speaker](https://m.media-amazon.com/images/I/61gfgTsyB6L.jpg)

## Before you begin

Have these ready:

- The ESP32-S3-AUDIO-Board **with its speaker**, not a different Waveshare display board.
- A USB-C **data cable**. Some charging cables supply power but cannot transfer data.
- A Windows computer and a USB port.
- A 2.4 GHz Wi-Fi network name and password. The ESP32-S3 does not connect to 5 GHz-only networks.
- An OpenAI API key with API billing and access to `gpt-live-1` and `gpt-5.6-luna`.
- [Python 3.12](https://www.python.org/downloads/) or another compatible Python 3.11+ installation. In the installer, enable **Add Python to PATH**.

**You do not need an SD card, Raspberry Pi, ReSpeaker microphone, or a permanently running PC.** The computer is for installation and optional configuration.

Factory firmware is replaced by this installation. Make your own private backup first if you want the option of restoring it. Never publish full-flash backups.

## 1. Download the project

1. Open [the repository](https://github.com/KMX415/waveshare-ai-speaker-assistant).
2. Select **Code → Download ZIP**.
3. Extract the ZIP. Open the extracted folder containing `setup.ps1`.
4. Right-click an empty area in that folder and choose **Open in Terminal**. Use PowerShell.
5. Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1
```

This creates a local Python environment and installs the setup tools. The execution-policy option applies to that process; it does not change your computer’s permanent policy.

**Expected result:** an installation success message. If `python` is not recognized, install Python with PATH enabled, close Terminal, and open it again.

## 2. Find the speaker’s USB port

1. Connect the speaker using the data cable.
2. Open **Device Manager → Ports (COM & LPT)**.
3. Look for the new USB serial device. Note its COM number.
4. If unsure, unplug the speaker, see which entry disappears, and reconnect it.

Examples below use **COM14**. Replace that with **your** port every time. Do not select a port without identifying the device.

**No new port?** Try a different data cable and a direct computer port. A light on the board only proves it has power.

## 3. Flash the prebuilt firmware

1. Open the project’s [Releases page](https://github.com/KMX415/waveshare-ai-speaker-assistant/releases).
2. Download **firmware-install.zip** from the latest release.
3. Extract it into a folder named **firmware-install** inside your project folder.
4. Confirm this file exists: `firmware-install\flash.ps1`.
5. Close any serial monitor or setup server using the board.
6. From the project Terminal, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\firmware-install\flash.ps1 -Port COM14
```

The installer uses the project’s Python environment, checks the image hashes, and flashes the bootloader, partition table, application, and wake-word models. Keep the cable connected until it finishes.

**Expected result:** flash verification messages followed by a reset. The initial firmware does not contain anyone’s Wi-Fi password or API key.

If the board will not enter download mode: hold **BOOT**, press and release **RESET**, release **BOOT**, then retry. Recheck the COM port if it changes.

This package targets **16 MB flash / 8 MB octal PSRAM**. Do not use it on another board. It is the initial release layout; do not use it to migrate an unrelated firmware installation whose settings you need to preserve.

Prefer compiling yourself? Use the [source build guide](../firmware/README.md).

## 4. Enable encrypted API-key storage once

**Read this before running the next command.** The speaker protects its saved API key with a hardware-generated HMAC secret. Provisioning permanently uses **eFuse key slot 5**; that cannot be undone. Ordinary firmware updates and replacing the API key remain possible. The script refuses to reuse a key slot owned by another application.

This protects copied API-key flash contents. It is not protection against modified firmware, and it does not encrypt the Wi-Fi password. See [the security details](../SECURITY.md).

With the setup server still closed, run:

```powershell
.\.venv\Scripts\python.exe .\scripts\provision_storage.py --port COM14
```

The tool identifies the firmware, checks that no conversation is active, explains the permanent action, and asks you to type **PROVISION KEY SLOT 5**. Nothing is provisioned if you cancel or enter a different answer.

**Expected result:** provisioning succeeds, the board restarts, and encrypted storage is available. If already provisioned by this project, the tool simply reports that and makes no hardware change.

**Provisioning refused?** Do not erase eFuses or try a different key slot at random. Stop and inspect the board’s previous firmware/security configuration.

## 5. Open the computer setup page

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\voice.ps1 setup --port COM14
```

Leave that Terminal running. Open **http://127.0.0.1:8766/** on **this computer**.

You should see the speaker status, microphone levels, volume, and wake-word settings. Try **Play speaker test**. You should hear a short tone.

The address `127.0.0.1` means “this computer.” It is not the address to open on your phone.

## 6. Connect the speaker to home Wi-Fi

1. In the computer setup page, click **Show device setup connection**.
2. It displays your speaker’s individual **HomeVoice-…** setup Wi-Fi name and generated password. Keep that password private.
3. On your phone, open Wi-Fi settings and join that exact network.
4. If the phone says **No internet**, choose to stay connected. This is normal during setup.
5. In the phone’s browser, type **http://192.168.4.1/**. Use HTTP.
6. Scan/select your **2.4 GHz home network**, enter its password, and save.
7. Check that the status reports a Wi-Fi connection. If necessary, reconnect your phone to the HomeVoice setup network to keep configuring.

**“Use the device setup Wi-Fi network”?** Your phone is reaching the page through the wrong interface. Rejoin the HomeVoice network, temporarily disable VPN/mobile-data switching if it interferes, and try the exact address again. The page is deliberately available only through the board’s setup network.

The page does not automatically pop up, and the speaker’s home-network IP is not its configuration address.

## 7. Save your API key and choose settings

On the device page or the computer setup page:

1. Enter your OpenAI API key in the password field.
2. Choose **Save key on device** (the wording may be shortened on the device page).
3. Wait for confirmation that the key was saved in **encrypted device storage**.
4. Adjust **speaker volume**.
5. Enable hands-free conversations, choose **Jarvis** or **Computer**, and save wake-word settings.
6. Start with wake sensitivity **50%**. Increase it if the speaker misses you; lower it if it wakes accidentally.

A blank key field after saving is normal: the page does not retrieve and display the stored secret. Wi-Fi, volume, wake preferences, and the encrypted key persist after power loss.

**Key save fails?** Check that step 4 completed successfully. Repeatedly re-entering the key will not fix unprovisioned storage. Never post your key in an issue or screenshot.

## 8. Have your first conversation

1. Say **“Jarvis”** (or the selected wake word).
2. Watch for the wake flash and acknowledgment tone.
3. Wait for the connection to finish and the **session-ready tone**.
4. Say “Hello” or ask a question.
5. Say **“Goodbye, Jarvis”** to hang up.
6. Wait for the dim idle light before waking it again.

“Goodbye, Jarvis” works even when your wake word is “Computer.” It depends on live transcription, so BOOT is the reliable physical fallback if the internet fails.

| Light | Meaning |
|:--|:--|
| Dim cyan dot | Waiting for the wake word locally |
| Green flash | Wake detected |
| Moving amber | Connecting or closing |
| Cyan ring | Connected and listening |
| Blue meter | Speaker audio is playing |
| Blinking red for 3 seconds | A conversation failed; then the ring returns to idle. Read the retained status message. |

If you only want to test wake detection, use **Test detection for 60 seconds**. It plays a local tone without starting paid conversations. End the test or wait for it to expire before trying live voice.

## 9. Move it to USB power

After a successful conversation:

1. Stop the conversation.
2. Close the PC setup server with **Ctrl+C**.
3. Unplug the speaker from the computer.
4. Connect it to a USB power supply.
5. Give it time to reconnect to Wi-Fi, then try the wake word.

No PC process is needed for everyday conversation. To change settings later, hold BOOT for at least **2.5 seconds** and release. Join the same private setup network and open **http://192.168.4.1/**. If you forgot its password, reconnect by USB and use the PC setup page to display it.

## Troubleshooting

| Symptom | What to do |
|:--|:--|
| Port busy / access denied | Close the setup Terminal, serial monitor, or other app holding that COM port. |
| No reply after a wake | Wait for the ready tone. Check Wi-Fi, API access, and the setup page’s error text. |
| Red flashing ring | Inspect status. Network/key/account failures and audio backlog are different problems; report the exact non-sensitive message. |
| Reply too quiet | Raise speaker volume in setup. Check the speaker connection with power disconnected if needed. |
| Wake word misses you | Use the local detection test; adjust sensitivity and placement. BOOT can start manually. |
| “Goodbye, Jarvis” not recognized | Say the full phrase during a connected conversation and pause briefly. Use BOOT if the connection failed. |
| Wi-Fi fails | Confirm 2.4 GHz support, password, and signal. Reopen the setup AP with BOOT. |
| Key save fails | Confirm encrypted-storage provisioning; do not share the key for troubleshooting. |
| LEDs show unexpected colors | Report your board revision. The tested revision uses RGB byte order. |
| Microphone backlog | Stop, retry, and confirm the latest firmware. Report recurrence; it should not be treated as normal. |

When reporting an issue, include board model/revision, firmware release, operating system, and the error message. **Remove API keys, network names/passwords, setup credentials, recordings, and flash dumps.**
