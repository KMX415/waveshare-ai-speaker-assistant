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

Weather uses live web search, not a separate weather subscription. Results depend on sources available to search; it should state when current conditions cannot be verified. Location is sent to OpenAI when relevant. Search and voice conversations incur API usage charges.

Speaker volume uses the existing device scale **0–80**. Voice changes are validated and saved to the board. Turning it to zero mutes spoken replies; use setup or USB controls to restore it. Voice controls can be disabled independently of web search. Purpose, location and units are currently edited through setup, not by voice.

Timers, alarms, reminders, music playback, smart-home control and Codex notifications are not implemented. The assistant is instructed to explain this rather than promise an action it cannot perform.

## Implementation

`assistant.c` constructs both the GPT-Live voice prompt and its Responses backend configuration from persisted preferences. The backend offers OpenAI's hosted `web_search`, `get_device_settings`, and `set_volume`. API keys and Wi-Fi credentials are never part of tool output.

`delegation.c` collects complete function calls by delegation/response ID, ignores duplicate call events, and discards cancelled or failed responses. The voice session task executes completed calls, sends each function result, then explicitly continues the backend. Network writes remain on the existing upload task. Search counters and backend failure counts are exposed in USB upload diagnostics without recording queries or transcripts.

Physical USB diagnostic commands: **A** runs bounded parser/argument self-tests while idle; **Y** followed by UTF-8 text (up to 600 bytes) submits a test question to an already active paid session. These are development aids, not runtime requirements. They do not expose the API key.

Protocol reference: [GPT-Live delegation and tools](https://developers.openai.com/api/docs/guides/live-delegation).
