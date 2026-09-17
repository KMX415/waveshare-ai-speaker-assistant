# Set up your assistant

The speaker runs from a USB power adapter and home Wi-Fi. A computer is only needed to install firmware. Live search and device voice controls run without a desktop bridge.

1. Hold BOOT for about three seconds, then release to open setup Wi-Fi.
2. Connect your phone to the speaker's saved HomeVoice setup network. Open **http://192.168.4.1**. Stay connected if the phone warns that this network has no internet.
3. In **Your assistant**, enter a town/city and country, choose Fahrenheit or Celsius, and leave **Live web search and weather** enabled. An existing postal code is a fallback; the assistant should ask when the location is ambiguous.
4. Write a short purpose or style, such as “A friendly kitchen assistant. Help with cooking, weather and everyday questions. Keep answers under three sentences unless I ask for more.”
5. Save while no conversation is active. Your settings remain after power loss and take effect at the next wake-up.
6. Say your wake phrase, wait for the ready tone, and ask a question.

The conversational name does not train a new wake word. Choose Jarvis or Computer in the separate wake-word section.

## Things to try

- “What's the weather today? Will it rain this afternoon?”
- “Look up the latest news about space exploration.”
- “Find the opening hours for the library in my town.”
- “How do I substitute buttermilk in pancakes?”
- “What is your volume?” or “Set your volume to 60.”
- “A little quieter, please.”
- “Save my location as Portland, Maine, USA.”
- “Use Celsius from now on.”
- “Call yourself Sage.”
- “Remember that I prefer vegetarian recipes.”
- “What do you remember about me?”
- “Forget my food preference.”

Weather uses live web search, not a separate weather subscription. Results depend on sources available to search; it should state when current conditions cannot be verified. Location is sent to OpenAI when relevant. Search and voice conversations incur API usage charges.

Speaker volume uses the existing device scale **0–80**. Voice changes are validated and saved to the board. Turning it to zero mutes spoken replies; use setup or USB controls to restore it. Voice controls can be disabled independently of web search. Name, location and units can also be changed by voice. Purpose and speaking style are edited through setup.

## Spoken microphone setup

Say **“Jarvis, calibrate your microphone.”** The assistant asks you to stay quiet for a room-noise sample, then speak normally from your usual kitchen position, then repeat the phrase to verify its trial adjustment. Follow the spoken prompts; there is no page to operate. Keep the distance and appliance noise representative of normal use.

The board measures raw mic headroom and echo-cancelled speech/noise levels, excluding its own playback. It requires enough detected speech, roughly a 10 dB speech-to-noise margin, and an unclipped verification sample. It changes hardware gain by at most 6 dB per attempt, bounded to supported 0–36 dB settings. These are conservative engineering thresholds, not a guarantee of speech recognition at every distance.

Only successful verification saves the new gain and enables bounded automatic gain. Unclear/noisy samples, cancellation and session closure restore the previous runtime setting; trial gain is not saved. Say **“Cancel microphone calibration”** or use BOOT to stop. If it cannot separate speech from an extractor fan or other noise, reposition the speaker or reduce that noise and retry; amplification cannot fix poor separation.

You can also say **“Turn automatic microphone gain on/off,” “What is your microphone gain?”** or **“Set microphone gain to 27 dB.”** Automatic adjustment uses local speech detection after echo cancellation, a slowly changing correction capped at approximately ±6 dB, and output limiting. It avoids boosting silence and the speaker's own playback. It is initially off and remains a saved preference across power loss. Calibration measurements are numerical only; no recording is saved.

USB **M** returns calibration phase, gain, automatic adjustment and the latest result; **M** plus byte 1 runs decision self-tests while idle. `scripts/check_microphone_controls.py` performs paid live tool checks while preserving the prior gain setting. A real room/voice calibration still needs a person to follow the spoken prompts.

## Memory

The speaker stores up to **12 short facts**, each with a short label and up to 160 UTF-8 bytes of text. An updated fact replaces its existing entry. A full store returns an error and the assistant should ask what to forget instead of silently discarding another memory. Name, location and units are separate preferences and do not use these slots.

**Remember useful facts automatically** is off by default. Enable it under Your assistant to let the model save useful, stable facts you mention directly during active conversations, without saying “remember.” It is instructed to ask before saving sensitive details and ignore background speech. These are model instructions, not speaker identification or a guarantee of perfect filtering; review Saved memories to correct mistakes. With automatic memory off, explicitly requested saves still work. Disabling voice controls disables memory tools as well; it does not delete existing memories.

Memories are shared by everyone using the speaker, survive power loss, and are included as context in future OpenAI sessions. They use ordinary **unencrypted** settings storage, unlike the API key. Do not store passwords or secrets. Idle audio is only processed locally for wake detection; no transcript archive is stored.

Ask it to forget a fact or use its **Forget** button in setup while idle. This removes it from future device context, not from earlier provider conversations or the already active conversation. To forget a location, ask it to clear your saved location (a separately saved postal code can still be used for weather).

Timers, alarms, reminders, music playback, smart-home control and Codex notifications are not implemented. The assistant is instructed to explain this rather than promise an action it cannot perform.

## Implementation

`assistant.c` constructs both the GPT-Live voice prompt and its Responses backend configuration from persisted preferences and memories. Alongside hosted `web_search`, device tools read settings, set volume/location/units/name, and remember/list/forget facts. API keys and Wi-Fi credentials are never part of tool output. Existing six-field settings are migrated with automatic memory disabled while preserving their values.

`delegation.c` collects complete function calls by delegation/response ID, ignores duplicate call events, and discards cancelled or failed responses. The voice session task executes completed calls, sends each function result, then explicitly continues the backend. Network writes remain on the existing upload task. Search counters and backend failure counts are exposed in USB upload diagnostics without recording queries or transcripts.

Physical USB diagnostic commands: **A** runs bounded parser/argument self-tests while idle; **Y** followed by UTF-8 text (up to 600 bytes) submits a test question to an already active paid session. These are development aids, not runtime requirements. They do not expose the API key.

**O** followed by one byte supports memory diagnostics: 0 reports counts and flags only, 1 enables automatic memory, 2 saves a reserved temporary test fact, 3 removes that exact test fact, and 4 checks invalid input rejection. Mutating operations require an idle session. `scripts/check_assistant_memory.py` exercises persistence across reboot and deletion; `--live` adds paid remember/forget tool checks. The script never prints personal memory contents.

Protocol reference: [GPT-Live delegation and tools](https://developers.openai.com/api/docs/guides/live-delegation).

## Change the speaking voice

Say "What voices can I choose?" or "Change your voice to Cedar." The assistant saves the choice on the speaker. Say "Goodbye Jarvis," then wake it again to hear the new voice. The current conversation keeps its original voice. The choice survives unplugging. Marin remains the default. The device setup page also has an OpenAI voice selector.

Supported built-in [OpenAI Live voices](https://developers.openai.com/api/reference/typescript/resources/live): Alloy, Ash, Ballad, Beacon, Bossa, Cedar, Cinder, Coral, Delta, Echo, Gleam, Marin, Meridian, Quartz, Ripple, Sage, Shimmer, Stone, Tempo, Verse, Vesper, Willow. Voice choice is separate from the assistant name and wake phrase. Voice controls must be enabled.
