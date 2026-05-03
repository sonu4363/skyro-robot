from pathlib import Path
from datetime import datetime
import ast
import hashlib
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

BASE_DIR = Path(__file__).resolve().parent
DEPS_DIR = BASE_DIR / "py_deps"
try:
    from flask import Flask, request, send_file
    import speech_recognition as sr
    from werkzeug.exceptions import BadRequest
except Exception:
    if DEPS_DIR.exists() and str(DEPS_DIR) not in sys.path:
        sys.path.insert(0, str(DEPS_DIR))
    from flask import Flask, request, send_file
    import speech_recognition as sr
    from werkzeug.exceptions import BadRequest

try:
    from groq import Groq
except Exception:
    Groq = None


USER_AGENT = "ESP32AssistantRobotFinalYear/1.0 (Groq AI student project)"
SERVER_VERSION = "skyro-server-groq-v3-2026-04-29"
STARTUP_GREETING = "Hi, I am Skyro. How can I help you?"
STT_LANGUAGES = ("en-IN", "hi-IN")
FALLBACK_SAY = "Main aapki help ke liye ready hoon."

SAFE_MATH_BINOPS = {
    ast.Add: lambda a, b: a + b,
    ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b,
    ast.Div: lambda a, b: a / b,
    ast.FloorDiv: lambda a, b: a // b,
    ast.Mod: lambda a, b: a % b,
    ast.Pow: lambda a, b: a**b,
}
SAFE_MATH_UNARYOPS = {
    ast.UAdd: lambda a: +a,
    ast.USub: lambda a: -a,
}

GROQ_API_KEY = os.getenv("GROQ_API_KEY", "YOUR_GROQ_API_KEY_HERE")
client = Groq(api_key=GROQ_API_KEY) if (Groq and GROQ_API_KEY) else None

app = Flask(__name__)
recognizer = sr.Recognizer()
recognizer.operation_timeout = 20

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def safe_print(text):
    msg = str(text)
    try:
        print(msg, flush=True)
    except Exception:
        try:
            print(msg.encode("ascii", errors="replace").decode("ascii"), flush=True)
        except Exception:
            pass


def ascii_safe_text(text):
    msg = str(text).replace("\r", " ").replace("\n", " ")
    msg = msg.encode("ascii", errors="replace").decode("ascii")
    msg = re.sub(r"\s+", " ", msg).strip()
    if msg and re.search(r"[A-Za-z0-9]", msg):
        return msg
    return FALLBACK_SAY


def ascii_safe_block(text):
    normalized = str(text).replace("\r\n", "\n").replace("\r", "\n")
    lines = [ascii_safe_text(line) if line.strip() else "" for line in normalized.split("\n")]
    return "\n".join(lines).strip()


def normalize_text(text):
    msg = str(text).lower().strip()
    msg = re.sub(r"[^a-z0-9 ]+", " ", msg)
    return re.sub(r"\s+", " ", msg).strip()


def contains_any(text, phrases):
    return any(phrase in text for phrase in phrases)


def detect_robot_command(text):
    q = normalize_text(text)
    command_words = {
        "forward": ["forward", "forword", "froward", "go ahead", "move forward", "aage", "aage jao"],
        "backward": ["backward", "back word", "back", "move back", "piche", "peeche"],
        "left": ["left", "lift", "turn left", "baye", "baaye"],
        "right": ["right", "rite", "write", "turn right", "daye", "daaye"],
        "stop": ["stop", "ruk", "ruko", "halt"],
    }
    for command, phrases in command_words.items():
        if contains_any(q, phrases):
            return command
    return None


def is_greeting(text):
    q = normalize_text(text)
    greetings = ["hi", "hii", "hiii", "hello", "helo", "hey", "hy", "namaste", "namaskar"]
    return any(greeting == q or q.startswith(greeting + " ") for greeting in greetings)


def is_how_are_you(text):
    q = normalize_text(text)
    phrases = ["how are you", "how r u", "kaise ho", "kaisa hai", "kaisi ho", "aap kaise ho", "aap kaise hain"]
    return contains_any(q, phrases)


def extract_math_expression(question):
    normalized = str(question).lower().strip()
    normalized = normalized.replace("what's", "what is").replace("what s", "what is")

    replacements = [
        ("to the power of", "**"),
        ("power", "**"),
        ("divided by", "/"),
        ("divide by", "/"),
        ("upon", "/"),
        ("over", "/"),
        ("into", "*"),
        ("multiplied by", "*"),
        ("times", "*"),
        ("plus", "+"),
        ("minus", "-"),
        ("mod", "%"),
    ]
    for src, dst in replacements:
        normalized = re.sub(rf"\b{re.escape(src)}\b", f" {dst} ", normalized)

    normalized = re.sub(r"[^0-9\.\+\-\*\/%\(\) ]+", " ", normalized)
    normalized = re.sub(r"\s+", " ", normalized).strip()

    if not normalized or not re.search(r"\d", normalized) or not re.search(r"[\+\-\*\/%]", normalized):
        return None
    return normalized


def safe_eval_math(node):
    if isinstance(node, ast.Expression):
        return safe_eval_math(node.body)
    if isinstance(node, (ast.Constant, ast.Num)):
        return node.value if hasattr(node, "value") else node.n
    if isinstance(node, ast.BinOp) and type(node.op) in SAFE_MATH_BINOPS:
        return SAFE_MATH_BINOPS[type(node.op)](safe_eval_math(node.left), safe_eval_math(node.right))
    if isinstance(node, ast.UnaryOp) and type(node.op) in SAFE_MATH_UNARYOPS:
        return SAFE_MATH_UNARYOPS[type(node.op)](safe_eval_math(node.operand))
    raise ValueError("unsupported math expression")


def answer_math_question(question):
    expression = extract_math_expression(question)
    if not expression:
        return None
    try:
        parsed = ast.parse(expression, mode="eval")
        value = safe_eval_math(parsed)
        if isinstance(value, float):
            value = int(value) if value.is_integer() else round(value, 4)
        return f"{expression} ka answer {value} hai."
    except Exception:
        return None


def get_ai_response(user_input):
    if client is None:
        return "AI client not ready. Please install groq package and set GROQ_API_KEY."
    try:
        chat_completion = client.chat.completions.create(
            messages=[
                {
                    "role": "system",
                    "content": (
                        "You are Skyro, a helpful and concise assistant. "
                        "Always answer in Roman Hindi (Hinglish) or English only. "
                        "Do not use Devanagari script. Keep responses short and natural."
                    ),
                },
                {"role": "user", "content": user_input},
            ],
            model="llama-3.3-70b-versatile",
            temperature=0.4,
            max_tokens=256,
        )
        return chat_completion.choices[0].message.content.strip()
    except Exception as exc:
        return f"AI service error: {exc}"


def build_assistant_response(question):
    question = str(question).strip()
    if not question:
        return "Question empty hai."

    safe_print("=" * 60)
    safe_print(f"QUESTION RECEIVED : {ascii_safe_text(question)}")
    safe_print("-" * 60)

    command = detect_robot_command(question)
    if command:
        answers = {
            "forward": "Main aage ja raha hoon.",
            "backward": "Main peeche ja raha hoon.",
            "left": "Main left turn kar raha hoon.",
            "right": "Main right turn kar raha hoon.",
            "stop": "Main stop ho gaya.",
        }
        answer = answers[command]
        safe_answer = ascii_safe_text(answer)
        safe_print(f"DETECTED TYPE   : ROBOT COMMAND -> {command.upper()}")
        safe_print(f"FINAL ANSWER    : {safe_answer}")
        safe_print("=" * 60)
        return f"COMMAND: {command}\nAnswer: {safe_answer}\nSAY: {safe_answer}"

    if is_greeting(question):
        answer = "Hi, I am Skyro. Nice to meet you."
        safe_answer = ascii_safe_text(answer)
        safe_print("DETECTED TYPE   : GREETING")
        safe_print(f"FINAL ANSWER    : {safe_answer}")
        safe_print("=" * 60)
        return f"Answer: {safe_answer}\nSAY: {safe_answer}"

    if is_how_are_you(question):
        answer = "Main theek hu, aap kaise hain?"
        safe_answer = ascii_safe_text(answer)
        safe_print("DETECTED TYPE   : HOW-ARE-YOU")
        safe_print(f"FINAL ANSWER    : {safe_answer}")
        safe_print("=" * 60)
        return f"Answer: {safe_answer}\nSAY: {safe_answer}"

    math_answer = answer_math_question(question)
    if math_answer:
        safe_answer = ascii_safe_text(math_answer)
        safe_print("DETECTED TYPE   : MATH QUESTION")
        safe_print(f"FINAL ANSWER    : {safe_answer}")
        safe_print("=" * 60)
        return f"Answer: {safe_answer}\nSAY: {safe_answer}"

    safe_print("DETECTED TYPE   : GENERAL QUESTION -> Groq AI")
    ai_response = get_ai_response(question)
    safe_answer = ascii_safe_text(ai_response)
    safe_print(f"FINAL ANSWER    : {safe_answer}")
    safe_print("=" * 60)
    return f"Answer: {safe_answer}\nSAY: {safe_answer}"


def create_tone_wav(path: Path, duration_sec=0.30, frequency_hz=680, sample_rate=16000, volume=0.30):
    frames = int(sample_rate * duration_sec)
    with wave.open(str(path), "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        samples = bytearray()
        for i in range(frames):
            value = int(volume * 32767 * math.sin(2 * math.pi * frequency_hz * i / sample_rate))
            samples.extend(struct.pack("<h", value))
        wav_file.writeframes(samples)


def run_powershell_tts(ps_exe, script_text, text, wav_path):
    temp_script = None
    try:
        with tempfile.NamedTemporaryFile("w", suffix=".ps1", delete=False, encoding="utf-8", dir=str(BASE_DIR)) as script_file:
            script_file.write(script_text)
            temp_script = Path(script_file.name)

        subprocess.run(
            [ps_exe, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(temp_script), "-Text", text, "-Output", str(wav_path)],
            check=True,
            timeout=40,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    finally:
        if temp_script and temp_script.exists():
            temp_script.unlink(missing_ok=True)


def create_tts_wav(text):
    speak_text = ascii_safe_text(text)
    if not speak_text:
        speak_text = STARTUP_GREETING
    if len(speak_text) > 320:
        speak_text = speak_text[:320].rsplit(" ", 1)[0]

    out_dir = BASE_DIR / "tts_cache"
    out_dir.mkdir(exist_ok=True)
    key = hashlib.sha1(speak_text.encode("utf-8")).hexdigest()
    wav_path = out_dir / f"{key}.wav"
    mp3_path = out_dir / f"{key}.mp3"

    if wav_path.exists() and wav_path.stat().st_size > 1000:
        return wav_path

    try:
        from gtts import gTTS
        import miniaudio

        gTTS(text=speak_text, lang="en", tld="co.in", slow=False).save(str(mp3_path))
        sound = miniaudio.mp3_read_file_s16(str(mp3_path))
        miniaudio.wav_write_file(str(wav_path), sound)
        if wav_path.exists() and wav_path.stat().st_size > 1000:
            return wav_path
    except Exception as exc:
        safe_print(f"gTTS/miniaudio failed: {ascii_safe_text(exc)}")

    sapi_com_script = r"""
param([string]$Text, [string]$Output)
$voice = New-Object -ComObject SAPI.SpVoice
$stream = New-Object -ComObject SAPI.SpFileStream
$format = New-Object -ComObject SAPI.SpAudioFormat
$format.Type = 22
$stream.Format = $format
$stream.Open($Output, 3, $false)
$voice.AudioOutputStream = $stream
[void]$voice.Speak($Text)
$stream.Close()
"""

    system_speech_script = r"""
param([string]$Text, [string]$Output)
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.Rate = 0
$synth.Volume = 100
$synth.SetOutputToWaveFile($Output)
$synth.Speak($Text)
$synth.Dispose()
"""

    ps_candidates = [
        r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
        shutil.which("powershell"),
        shutil.which("pwsh"),
    ]
    scripts = [sapi_com_script, system_speech_script]

    last_error = None
    for ps in ps_candidates:
        if not ps:
            continue
        for script_text in scripts:
            try:
                run_powershell_tts(ps, script_text, speak_text, wav_path)
                if wav_path.exists() and wav_path.stat().st_size > 1000:
                    return wav_path
            except Exception as exc:
                last_error = exc
                safe_print(f"PowerShell TTS failed via {ps}: {ascii_safe_text(exc)}")

    safe_print(f"TTS fallback tone used. Last TTS error: {ascii_safe_text(last_error) if last_error else 'unknown'}")
    create_tone_wav(wav_path)
    return wav_path


@app.get("/")
def home():
    return f"{SERVER_VERSION}\nESP32 assistant server is running. Test /ask?q=hi"


@app.get("/tts")
@app.get("/tts/")
def tts():
    text = request.args.get("text", STARTUP_GREETING).strip()
    try:
        wav_path = create_tts_wav(text)
        safe_print(f"TTS: {ascii_safe_text(text)}")
        return send_file(wav_path, mimetype="audio/wav", as_attachment=False)
    except Exception as exc:
        msg = f"TTS error: {ascii_safe_text(exc)}"
        safe_print(msg)
        return msg, 500


@app.get("/ask")
def ask_text():
    question = request.args.get("q", "").strip()
    if not question:
        return "Use /ask?q=your question here", 400
    response = ascii_safe_block(build_assistant_response(question))
    return response


@app.post("/transcribe")
def transcribe():
    try:
        audio_bytes = request.get_data()
        safe_print(f"\n/transcribe content_length={request.content_length} received={len(audio_bytes)}")
        if not audio_bytes:
            return "No audio received from ESP32", 400

        out_dir = BASE_DIR / "recordings"
        out_dir.mkdir(exist_ok=True)
        wav_path = out_dir / f"esp32_{datetime.now().strftime('%Y%m%d_%H%M%S')}.wav"
        wav_path.write_bytes(audio_bytes)

        with sr.AudioFile(str(wav_path)) as source:
            audio = recognizer.record(source)

        candidates = []
        for lang in STT_LANGUAGES:
            try:
                text = recognizer.recognize_google(audio, language=lang).strip()
                if text:
                    candidates.append((lang, text))
            except sr.UnknownValueError:
                continue
            except sr.RequestError as exc:
                safe_print(f"STT request error ({lang}): {ascii_safe_text(exc)}")

        if not candidates:
            msg = "Could not understand audio. Speak closer to mic and try again."
            safe_print(msg)
            return msg

        question = max(candidates, key=lambda item: len(item[1]))[1]
        safe_print(f"Question text: {ascii_safe_text(question)}")

        response = ascii_safe_block(build_assistant_response(question))
        return response

    except BadRequest:
        msg = "Bad/incomplete audio upload from ESP32. Try shorter recording."
        safe_print(msg)
        return msg, 400
    except sr.UnknownValueError:
        msg = "Could not understand audio. Speak closer to mic and try again."
        safe_print(msg)
        return msg
    except sr.RequestError as exc:
        msg = f"Speech-to-text service error: {ascii_safe_text(exc)}"
        safe_print(msg)
        return msg, 500
    except Exception as exc:
        msg = f"Server error: {ascii_safe_text(exc)}"
        safe_print(msg)
        return msg, 500


if __name__ == "__main__":
    safe_print(SERVER_VERSION)
    safe_print("Running on http://0.0.0.0:5000")
    app.run(host="0.0.0.0", port=5000, debug=False)
