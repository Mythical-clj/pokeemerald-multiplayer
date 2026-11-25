#!/usr/bin/env python3
"""One-window launcher for the Emerald multiplayer link.

Pick a room and a player number, press Start. It runs the sync project's
client (adapter <-> relay) in a child process and shows its log.
The same executable is the child: `owl_launcher.exe --run-client <args>`.
"""
import json
import os
import queue
import subprocess
import sys
import threading
import webbrowser

import owl_text

DEFAULT_RELAY = "wss://gbcatrade.wired-ariel.it/ws"
SETTINGS = os.path.join(os.path.expanduser("~"), ".owl_launcher.json")
ZADIG = "https://zadig.akeo.ie/"


def run_client(args):
    """Child mode: behave exactly like `python client.py <args>`."""
    import client
    sys.argv = ["client.py"] + args
    client.main()


def load_settings():
    try:
        with open(SETTINGS) as f:
            return json.load(f)
    except Exception:
        return {}


def save_settings(d):
    try:
        with open(SETTINGS, "w") as f:
            json.dump(d, f)
    except Exception:
        pass


def hide_console():
    try:
        import ctypes
        hwnd = ctypes.windll.kernel32.GetConsoleWindow()
        if hwnd:
            ctypes.windll.user32.ShowWindow(hwnd, 0)
    except Exception:
        pass


def gui():
    import tkinter as tk
    from tkinter import scrolledtext

    hide_console()
    st = load_settings()
    root = tk.Tk()
    root.title("Emerald Multiplayer Link")
    root.geometry("680x560")

    fields = {}
    form = tk.Frame(root)
    form.pack(fill="x", padx=10, pady=8)
    for row, (key, label, default) in enumerate((
            ("room", "Room (same for everyone)", "1"),
            ("peer", "Player number (1-4, different for each)", "1"),
            ("relay", "Relay", DEFAULT_RELAY))):
        tk.Label(form, text=label, anchor="w", font=("Segoe UI", 10, "bold")).grid(row=row, column=0, sticky="w")
        var = tk.StringVar(value=str(st.get(key, default)))
        tk.Entry(form, textvariable=var, width=48).grid(row=row, column=1, padx=6, pady=2)
        fields[key] = var

    rawvar = tk.BooleanVar(value=False)
    health = owl_text.Health()
    summary = tk.StringVar(value="\n".join(health.summary()))
    tk.Label(root, textvariable=summary, justify="left", anchor="w",
             font=("Consolas", 10), relief="groove").pack(fill="x", padx=10, pady=4)

    log = scrolledtext.ScrolledText(root, height=14, state="disabled")
    log.pack(fill="both", expand=True, padx=10, pady=4)
    status = tk.StringVar(value="Stopped")
    tk.Label(root, textvariable=status, anchor="w").pack(fill="x", padx=10)

    q = queue.Queue()
    proc = {"p": None}

    def append(text):
        log.configure(state="normal")
        log.insert("end", text)
        log.see("end")
        log.configure(state="disabled")

    def reader(p):
        for line in iter(p.stdout.readline, ""):
            q.put(line)
        q.put("\n[client stopped]\n")
        q.put(None)

    def start():
        if proc["p"]:
            return
        try:
            room = int(fields["room"].get())
            peer = int(fields["peer"].get())
        except ValueError:
            append("Room and player number must be numbers.\n")
            return
        relay = fields["relay"].get().strip() or DEFAULT_RELAY
        save_settings({"room": room, "peer": peer, "relay": relay})
        args = ["--transport", "usb", "--room", str(room), "--peer-id", str(peer),
                "--usb-timing", "7400", "--relay", relay]
        cmd = ([sys.executable, "--run-client"] + args if getattr(sys, "frozen", False)
               else [sys.executable, os.path.abspath(__file__), "--run-client"] + args)
        env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8")
        flags = 0x08000000 if os.name == "nt" else 0   # CREATE_NO_WINDOW
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding="utf-8", errors="replace",
                             env=env, creationflags=flags)
        proc["p"] = p
        health.__init__()
        threading.Thread(target=reader, args=(p,), daemon=True).start()
        status.set("Running: room %d, player %d" % (room, peer))
        btn.configure(text="Stop", command=stop)

    def stop():
        p = proc["p"]
        if p:
            p.terminate()

    def poll():
        try:
            while True:
                item = q.get_nowait()
                if item is None:
                    proc["p"] = None
                    status.set("Stopped")
                    btn.configure(text="Start", command=start)
                else:
                    health.feed(item)
                    summary.set("\n".join(health.summary()))
                    append(owl_text.translate(item, rawvar.get()))
        except queue.Empty:
            pass
        root.after(100, poll)

    bar = tk.Frame(root)
    bar.pack(fill="x", padx=10, pady=8)
    btn = tk.Button(bar, text="Start", width=12, command=start)
    btn.pack(side="left")
    tk.Button(bar, text="Adapter driver help (Zadig)",
              command=lambda: webbrowser.open(ZADIG)).pack(side="left", padx=8)

    def copy_log():
        root.clipboard_clear()
        root.clipboard_append(log.get("1.0", "end"))
        status.set("Log copied to the clipboard")
    tk.Button(bar, text="Copy log", command=copy_log).pack(side="left")
    tk.Checkbutton(bar, text="Show raw details (Italian)", variable=rawvar).pack(side="left", padx=8)

    def on_close():
        stop()
        root.destroy()
    root.protocol("WM_DELETE_WINDOW", on_close)
    poll()
    root.mainloop()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--run-client":
        run_client(sys.argv[2:])
    else:
        gui()
