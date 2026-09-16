"""Configuration kept on the host, never supplied by an audio device."""

from dataclasses import dataclass, field
import os

SAMPLE_RATE = 16000
FRAME_SAMPLES = 320
FRAME_BYTES = FRAME_SAMPLES * 2
FORMAT = {"encoding": "pcm_s16le", "sample_rate": SAMPLE_RATE, "channels": 1}
LIVE_URL = "wss://api.openai.com/v1/live/sessions"

VOICE_PROMPT = """You are a friendly home voice assistant. Speak naturally and clearly.
Keep everyday replies brief, and go into detail when asked.
Backchannel policy: Use occasional, unobtrusive acknowledgments.
Interruption policy: Yield when the user interrupts and listen to their correction.
Delegation policy:
Backend tools: General knowledge and careful reasoning. No web search, home controls,
computer access, persistent memory, reminders, or task execution are connected yet.
Delegate to the backend when a question needs knowledge or reasoning beyond this conversation.
Do not delegate to the backend when greeting, clarifying, or using an answer already given.
Wait for backend results before presenting them. Explain unavailable capabilities honestly.
"""


@dataclass
class Settings:
    api_key: str = field(default="", repr=False)
    device_token: str = field(default="", repr=False)
    host: str = "127.0.0.1"
    port: int = 8765
    voice: str = "marin"
    backend_model: str = "gpt-5.6-luna"
    max_seconds: float = 600
    idle_seconds: float = 60
    startup_timeout: float = 20
    close_timeout: float = 15
    mock: bool = False

    @classmethod
    def from_env(cls, **overrides):
        values = dict(api_key=os.getenv("OPENAI_API_KEY", ""),
                      device_token=os.getenv("VOICE_DEVICE_TOKEN", ""),
                      voice=os.getenv("VOICE_NAME", "marin"),
                      backend_model=os.getenv("VOICE_BACKEND_MODEL", "gpt-5.6-luna"))
        return cls(**(values | overrides))

    def session(self):
        return {
            "model": "gpt-live-1",
            "instructions": VOICE_PROMPT,
            "audio": {"format": {"type": "audio/pcm", "rate": SAMPLE_RATE},
                      "output": {"voice": self.voice}},
            "delegation": {"type": "responses", "responses": {
                "model": self.backend_model,
                "instructions": "Answer general questions accurately and concisely for a spoken conversation. "
                                "You have no tools or access to current information. State uncertainty. "
                                "Do not claim to have controlled devices, saved memories, or performed actions.",
            }},
        }
