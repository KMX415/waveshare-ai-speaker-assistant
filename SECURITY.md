# Security and credential handling

## What stays out of this repository

Real API keys, personal Wi-Fi names/passwords, per-device setup passwords, recordings,
flash dumps, attachments, build products, and local environments must never be committed.
The ignore rules exclude these common locations and file types. Review the staged diff
and run a secret scanner before every public push; ignore rules alone are not a guarantee.

The firmware and installation images contain no user-supplied credentials. Each board
generates its own setup password. Enter secrets locally through the setup page, never
through GitHub issues, pull requests, or chat.

## Storage boundary

- OpenAI API key: HMAC-derived encrypted NVS in a dedicated partition after provisioning.
- Wi-Fi credentials and ordinary preferences: **unencrypted NVS**.
- Setup network: WPA2 with an individual generated password; HTTP settings pages are
  restricted to this interface. HTTP itself is not end-to-end encrypted.
- OpenAI connection: TLS with certificate validation.
- Secure boot, whole-flash encryption, and debug/download lockout: **not enabled**.

Encrypted API-key storage protects copied credential flash. Someone who can replace
the firmware can potentially use the chip to derive the storage keys. This prototype
does not claim protection against that attacker.

## First-time storage provisioning

Run `scripts/provision_storage.py --port YOUR_PORT` from the installed Python environment,
with other serial applications closed. Read the warning and type the explicit confirmation.
Provisioning permanently writes an HMAC secret into **key slot 5** and read-protects it.
It refuses occupied slots not already owned by this application. The secret is generated
on the board and never printed or returned to the PC.

Ordinary boot, flashing, and web setup cannot initiate provisioning. The exact USB
provisioning command is `EENABLE-HARDWARE-KEY-STORAGE`; the helper requires human
confirmation before sending it. No provisioning command is part of the firmware installer.

Do not erase NVS on a provisioned board casually: it contains an ownership marker required
to reopen the vault. The normal release installer writes only the application-related
partitions and leaves NVS and the credential partition untouched. Factory-reset recovery
and migration from unrelated firmware require separate planning.

## Audio and billing

Wake detection happens locally. Active microphone audio is sent to OpenAI. The firmware
does not retain audio files or transcripts; cloud processing is governed by the API
provider's policies. The desktop client offers an explicit transcript-printing option.

Live sessions are billable. Hardware voice-check scripts also open billable sessions when
invoked. Do not run them in CI or against someone else's account.

## Reporting a vulnerability

Use the repository's private vulnerability reporting feature if enabled. Do not post
real credentials or exploit details containing private data in a public issue.
If a key was exposed, revoke/rotate it through its provider; deleting a file or commit
does not make an exposed key safe again.
