#!/usr/bin/env python3
"""Record real stock-Clang/Sela commands, then render their PTY output.

Run from any directory: bash /path/to/sela/scripts/record-hello-demo.sh
Requires the built publisher, SDK, Python 3, Pillow, DejaVu fonts, and file.
No compiler rebuild or source-tree build is performed. Outputs default to assets/.
The cast preserves actual timestamps, including intentional reading/typing pauses.
The GIF plays that same timeline at 10 fps; no compiler timing is accelerated.
"""

import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import pty
import re
import select
import shutil
import struct
import subprocess
import tempfile
import termios
import time

from PIL import Image, ImageDraw, ImageFont, __version__ as pillow_version


REPOSITORY = Path(__file__).resolve().parent.parent
WIDTH, HEIGHT, COLUMNS, ROWS = 960, 690, 88, 24
PROMPT_END = re.compile(r"\x1b\]133;D;(\d+)\x07\$ ")
OSC = re.compile(r"\x1b\][^\x07]*\x07")
CSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
COMMANDS = [
    ("C source", "cat main.c hello.c", 4.5),
    ("Publish with stock Clang", 'clang --config="$SELA_CONFIG" -O2 main.c hello.c -o hello.sela', 2.5),
    ("One standalone artifact", "file hello.sela", 2.0),
    ("Compile independently", "selac hello.sela -o hello", 2.5),
    ("Run the native executable", "env -u LD_LIBRARY_PATH -u LD_PRELOAD ./hello", 5.0),
]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def clean(text):
    """Remove the shell's semantic prompt markers, not application output."""
    text = OSC.sub("", text)
    text = CSI.sub("", text)
    if "\x1b" in text:
        raise RuntimeError("Unrecognized terminal escape; refusing a lossy recording")
    return text.replace("\r\n", "\n")


def record(directory):
    if shutil.which("file") is None:
        raise RuntimeError("The system file command is required for this recording")
    clang = Path(os.environ["SELA_LLVM_ROOT"]) / "bin/clang"
    config = REPOSITORY / "build/prealpha/sela.cfg"
    compiler = REPOSITORY / "build/prealpha/selac"
    for required in (clang, config, compiler):
        if not required.is_file():
            raise RuntimeError("Build the matching SDK and publisher before recording")
    for name in ("main.c", "hello.c", "hello.h"):
        shutil.copyfile(REPOSITORY / "examples/hello/hello" / name, directory / name)

    environment = {
        "PATH": str(compiler.parent) + os.pathsep + os.environ["PATH"],
        "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH", ""),
        "SELA_SDK_ROOT": os.environ["SELA_SDK_ROOT"],
        "SELA_CONFIG": str(config),
        "TERM": "dumb",
        "LC_ALL": "C",
        "PS1": "$ ",
        "PS2": "> ",
        "HISTFILE": "/dev/null",
        # Read the real previous-command status before printing the prompt.
        "PROMPT_COMMAND": "printf '\\033]133;D;%s\\007' \"$?\"",
    }
    started_at = int(time.time())
    started = time.monotonic()
    process, terminal = pty.fork()
    if process == 0:
        os.chdir(directory)
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLUMNS, 0, 0))
        os.execve("/bin/bash", ["bash", "--noprofile", "--norc", "-i"], environment)

    events = []
    output = ""
    stages = []

    def pump(timeout):
        nonlocal output
        if not select.select([terminal], [], [], timeout)[0]:
            return
        try:
            chunk = os.read(terminal, 65536)
        except OSError as error:
            if error.errno == errno.EIO:
                raise RuntimeError("Recording shell exited unexpectedly") from error
            raise
        if not chunk:
            raise RuntimeError("Recording shell closed unexpectedly")
        data = chunk.decode("utf-8", errors="strict")
        events.append([round(time.monotonic() - started, 6), "o", data])
        output += data

    def pause(seconds):
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            pump(min(0.02, remaining))

    def prompt_after(offset):
        deadline = time.monotonic() + 120
        while True:
            match = PROMPT_END.search(output, offset)
            if match:
                return int(match.group(1))
            if time.monotonic() > deadline:
                raise RuntimeError("Timed out waiting for a real shell command")
            pump(0.02)

    try:
        if prompt_after(0) != 0:
            raise RuntimeError("Recording shell initialization failed")
        pause(1.0)
        for title, command, hold in COMMANDS:
            stage = {"title": title, "command": command,
                     "started_at_seconds": round(time.monotonic() - started, 6)}
            for character in command:
                os.write(terminal, character.encode())
                pause(0.024)
            offset = len(output)
            executed = time.monotonic()
            os.write(terminal, b"\n")
            status = prompt_after(offset)
            stage.update(exit_status=status,
                         execution_seconds=round(time.monotonic() - executed, 6),
                         reading_pause_seconds=hold)
            stages.append(stage)
            if status != 0:
                raise RuntimeError("Real demo command failed:\n" + clean(output))
            pause(hold)
        duration = round(time.monotonic() - started, 6)
        # An empty output event preserves the final silent reading hold in cast
        # players that determine duration from the last event, not the header.
        events.append([duration, "o", ""])
    finally:
        # End the recording after the final reading pause, then close the shell.
        os.write(terminal, b"\x04")
        os.waitpid(process, 0)
        os.close(terminal)

    transcript = clean(output)
    if "\nHello world\n" not in transcript:
        raise RuntimeError("The real native output did not contain Hello world")
    for private_path in (str(REPOSITORY), str(Path.home()), os.environ["SELA_SDK_ROOT"]):
        if private_path in transcript:
            raise RuntimeError("Private path appeared in the terminal output")
    if "\r" in transcript or "\b" in transcript:
        raise RuntimeError("Unexpected terminal editing; refusing a lossy transcript")

    metadata = {
        "format_version": 1,
        "recorded_at_unix": started_at,
        "host": {"system": platform.system(), "machine": platform.machine(),
                 "distribution": platform.freedesktop_os_release().get("PRETTY_NAME")},
        "method": "Actual interactive Bash PTY output; no fabricated compiler output",
        "playback": {"speed": 1.0, "duration_seconds": duration, "gif_fps": 10,
                     "initial_pause_seconds": 1.0, "typing_interval_seconds": 0.024,
                     "reading_pauses": "Included in cast timestamps and GIF"},
        "prepared_environment": {
            "working_directory": "Fresh temporary copy of examples/hello/hello C sources",
            "SELA_CONFIG": "build/prealpha/sela.cfg (absolute path supplied privately)",
            "PATH": "Matching SDK tools and build/prealpha prepended",
            "application_environment": "LD_LIBRARY_PATH and LD_PRELOAD unset for execution",
        },
        "commands": stages,
        "source_sha256": {name: sha256(directory / name)
                          for name in ("main.c", "hello.c", "hello.h")},
        "tool_sha256": {"clang": sha256(clang), "selac": sha256(compiler),
                        "sela.cfg": sha256(config),
                        "libsela-clang.so": sha256(compiler.parent / "libsela-clang.so"),
                        "sela-ld": sha256(compiler.parent / "sela-ld")},
        "output_sha256": {"hello.sela": sha256(directory / "hello.sela"),
                          "hello": sha256(directory / "hello")},
        "clang_version": subprocess.check_output([str(clang), "--version"],
                                                 env=environment, text=True).splitlines()[0],
    }
    return events, transcript, metadata


def render(events, metadata, output, font_directory):
    fonts = {
        "terminal": ImageFont.truetype(str(font_directory / "DejaVuSansMono.ttf"), 17),
        "title": ImageFont.truetype(str(font_directory / "DejaVuSans-Bold.ttf"), 23),
        "small": ImageFont.truetype(str(font_directory / "DejaVuSans.ttf"), 14),
    }
    background, panel = "#0b1220", "#111c2e"
    foreground, muted, accent = "#e6edf6", "#93a5bd", "#38d9c5"
    frames = []
    text = ""
    event_index = 0
    duration = metadata["playback"]["duration_seconds"]
    frame_count = int(duration * 10 + 0.999)
    for frame_index in range(frame_count):
        position = frame_index / 10
        while event_index < len(events) and events[event_index][0] <= position:
            text += events[event_index][2]
            event_index += 1
        visible = clean(text).split("\n")[-ROWS:]
        stage = "Ready to compile"
        for item in metadata["commands"]:
            if item["started_at_seconds"] <= position:
                stage = item["title"]
        canvas = Image.new("RGB", (WIDTH, HEIGHT), background)
        draw = ImageDraw.Draw(canvas)
        draw.rounded_rectangle((14, 14, WIDTH - 15, HEIGHT - 15), radius=16,
                               fill=panel, outline="#24344b", width=1)
        draw.text((36, 30), "C → Sela → native", font=fonts["title"], fill=foreground)
        badge = "REAL TERMINAL RECORDING"
        badge_width = draw.textlength(badge, font=fonts["small"])
        draw.text((WIDTH - badge_width - 36, 37), badge, font=fonts["small"], fill=accent)
        draw.text((36, 70), stage, font=fonts["small"], fill=accent)
        draw.line((36, 98, WIDTH - 37, 98), fill="#24344b")
        for line_number, line in enumerate(visible):
            if len(line) > COLUMNS:
                raise RuntimeError("Terminal line exceeds the recorded width")
            color = accent if line.startswith("$ ") else foreground
            draw.text((36, 113 + line_number * 21), line, font=fonts["terminal"], fill=color)
        cursor_line = visible[-1] if visible else ""
        cursor_x = 36 + draw.textlength(cursor_line, font=fonts["terminal"])
        cursor_y = 116 + (len(visible) - 1) * 21
        draw.rectangle((cursor_x + 1, cursor_y, cursor_x + 9, cursor_y + 16), fill=accent)
        draw.line((36, HEIGHT - 58, WIDTH - 37, HEIGHT - 58), fill="#24344b")
        draw.text((36, HEIGHT - 44), "Actual commands + output · Real-time playback · Reading pauses included",
                  font=fonts["small"], fill=muted)
        frames.append(canvas.convert("P", palette=Image.Palette.ADAPTIVE, colors=128))
    frames[0].save(output, save_all=True, append_images=frames[1:], duration=100,
                   loop=0, optimize=True, disposal=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=REPOSITORY / "assets")
    parser.add_argument("--font-dir", type=Path,
                        default=Path("/usr/share/fonts/truetype/dejavu"))
    arguments = parser.parse_args()
    # selac prints an absolute output path. Keep that real message, using a
    # non-personal scratch path rather than redacting terminal output afterward.
    with tempfile.TemporaryDirectory(prefix="sela-demo-", dir="/tmp") as temporary:
        scratch = Path(temporary)
        events, transcript, metadata = record(scratch)
        metadata["renderer"] = {"script_sha256": sha256(Path(__file__)),
                                "python_version": platform.python_version(),
                                "pillow_version": pillow_version,
                                "canvas_pixels": [WIDTH, HEIGHT],
                                "font_sha256": {name: sha256(arguments.font_dir / name)
                                                for name in ("DejaVuSansMono.ttf", "DejaVuSans.ttf",
                                                             "DejaVuSans-Bold.ttf")}}
        cast = {"version": 2, "width": COLUMNS, "height": ROWS,
                "timestamp": metadata["recorded_at_unix"],
                "duration": metadata["playback"]["duration_seconds"],
                "title": "Sela: C source to artifact to native executable",
                "env": {"TERM": "dumb", "SHELL": "/bin/bash"}}
        (scratch / "hello-demo.cast").write_text(
            "\n".join(json.dumps(item) for item in [cast, *events]) + "\n", encoding="utf-8")
        (scratch / "hello-demo.txt").write_text(
            "Sela Hello demo — actual terminal transcript\n\n"
            "Reproduce: bash scripts/record-hello-demo.sh\n"
            "Prerequisites: built SDK/publisher, Python 3, Pillow, DejaVu fonts, file.\n"
            "The recorder copies the existing Hello C sources to fresh temporary storage.\n"
            "SELA_CONFIG points to build/prealpha/sela.cfg in the toolchain checkout.\n"
            "PATH selects the matching stock Clang and the independent selac program.\n"
            "The cast and GIF retain real elapsed time, including deliberate reading pauses.\n"
            "No compiler/source build cache is reused from the Hello example directory.\n\n"
            + transcript.rstrip() + "\n", encoding="utf-8")
        render(events, metadata, scratch / "hello-demo.gif", arguments.font_dir)
        metadata["assets_sha256"] = {name: sha256(scratch / name) for name in
                                     ("hello-demo.cast", "hello-demo.txt", "hello-demo.gif")}
        (scratch / "hello-demo.provenance.json").write_text(
            json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        arguments.output_dir.mkdir(parents=True, exist_ok=True)
        for name in ("hello-demo.gif", "hello-demo.cast", "hello-demo.txt", "hello-demo.provenance.json"):
            shutil.copyfile(scratch / name, arguments.output_dir / name)
        print("Recorded five successful commands; wrote GIF, cast, transcript, and provenance.")
        print("Real-time duration: %.2f seconds" % metadata["playback"]["duration_seconds"])


if __name__ == "__main__":
    main()
