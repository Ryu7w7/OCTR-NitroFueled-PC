"""Lab: drive the game without a player (bat 16 reads logs/pad.txt as held buttons, BB_PAD_FILE).

    python tools/drive.py start                 launch bat 16 (the native renderer alone)
    python tools/drive.py press cross [0.15]    hold buttons for a while, then release
    python tools/drive.py hold "cross lx=40" 5  hold for seconds (sticks: lx= ly= rx= ry= 0..255)
    python tools/drive.py wait 3                sleep
    python tools/drive.py shot out.png          screenshot of the game window (else the screen)
    python tools/drive.py stop                  close the game
    python tools/drive.py log [pattern]         last lines of the newest log (matching pattern)

Several steps can follow one another on one command line, separated by ";".
"""
import ctypes
import glob
import os
import subprocess
import sys
import time

PKG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dist", "vulkan-lab",
                   "CTR-NitroFueled-PC-portable")
PAD = os.path.join(PKG, "logs", "pad.txt")
BAT = os.path.join(PKG, "16 - Vulkan nativo SOLO (pruebas automaticas).bat")


def write_pad(text):
    with open(PAD + ".tmp", "w") as f:
        f.write(text + "\n")
    os.replace(PAD + ".tmp", PAD)


def window_rect():
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def each(hwnd, _):
        n = user32.GetWindowTextLengthW(hwnd)
        if n and user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if "Crash" in buf.value or "SacredFire" in buf.value or "sacredfire" in buf.value:
                found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    if not found:
        return None
    rect = ctypes.wintypes.RECT() if hasattr(ctypes, "wintypes") else None
    import ctypes.wintypes as wt
    rect = wt.RECT()
    user32.GetWindowRect(found[0], ctypes.byref(rect))
    return (rect.left, rect.top, rect.right, rect.bottom)


def step(words):
    cmd, args = words[0], words[1:]
    if cmd == "start":
        # start [16|17] [NAME=value ...]: 16 the native renderer alone, 17 stock shadPS4
        write_pad("")
        bat = BAT
        env = dict(os.environ)
        for a in args:
            if "=" in a:
                k, v = a.split("=", 1)
                env[k] = v
            else:
                bat = glob.glob(os.path.join(PKG, a + " - *.bat"))[0]
        subprocess.Popen(["cmd", "/c", bat], cwd=PKG, env=env, creationflags=subprocess.CREATE_NEW_CONSOLE)
        print("started", os.path.basename(bat), [a for a in args if "=" in a])
    elif cmd == "route":
        # route <track index in Time Trial's list, 0 = Crash Cove>: from the title screen to the
        # starting grid (Time Trial, the save's character); the timings fit this PC
        track = int(args[0]) if args else 3
        for words in (["press", "cross", "0.3"], ["wait", "6"], ["press", "cross", "0.3"], ["wait", "2"],
                      ["press", "down"], ["wait", "1"], ["press", "cross", "0.3"], ["wait", "4"]):
            step(words)
        for _ in range(3):
            step(["press", "down"]); step(["wait", "0.5"])
        step(["press", "cross", "0.3"]); step(["wait", "5"])
        for _ in range(track):
            step(["press", "down"]); step(["wait", "0.5"])
        step(["press", "cross", "0.3"]); step(["wait", "3"])
        step(["press", "cross", "0.3"]); step(["wait", "12"])
        step(["press", "cross", "0.5"]); step(["wait", "3"])
        step(["press", "cross", "0.5"]); step(["wait", "25"])
        print("on the grid (if every screen came when expected)")
    elif cmd == "press":
        write_pad(args[0])
        time.sleep(float(args[1]) if len(args) > 1 else 0.15)
        write_pad("")
        time.sleep(0.1)
    elif cmd == "hold":
        write_pad(args[0])
        time.sleep(float(args[1]))
        write_pad("")
    elif cmd == "wait":
        time.sleep(float(args[0]))
    elif cmd == "shot":
        from PIL import ImageGrab
        rect = window_rect()
        img = ImageGrab.grab(bbox=rect) if rect else ImageGrab.grab()
        img.thumbnail((1280, 1280))
        img.save(args[0])
        print("shot", args[0], rect)
    elif cmd == "stop":
        write_pad("")
        subprocess.run(["taskkill", "/IM", "sacredfire.exe", "/F"], capture_output=True)
        print("stopped")
    elif cmd == "log":
        logs = sorted(glob.glob(os.path.join(PKG, "logs", "*.log")), key=os.path.getmtime)
        lines = open(logs[-1], encoding="utf-8", errors="replace").read().splitlines()
        if args:
            lines = [l for l in lines if args[0] in l]
        print(logs[-1])
        print("\n".join(l[:300] for l in lines[-int(args[1]) if len(args) > 1 else -15:]))
    else:
        raise SystemExit("unknown step " + cmd)


def main(argv):
    import shlex
    for part in " ".join(argv).split(";"):
        # posix=False keeps Windows paths' backslashes; quotes group words ("cross lx=40")
        words = [w[1:-1] if len(w) > 1 and w[0] == w[-1] and w[0] in "\"'" else w
                 for w in shlex.split(part, posix=False)]
        if words:
            step(words)


if __name__ == "__main__":
    main(sys.argv[1:])
