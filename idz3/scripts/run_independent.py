#!/usr/bin/env python3
"""запускает отдельные C-программы, не передавая им память или IPC-дескрипторы"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--build-dir", default="build")
options, args = parser.parse_known_args()
build = Path(options.build_dir).resolve()
controller = build / "center_session"
worker = build / "center_worker"
session = "i" + str(os.getpid()) + "_" + str(time.monotonic_ns() % 1000000000)
if "--session" in args:
    index = args.index("--session")
    if index + 1 < len(args):
        session = args[index + 1]
else:
    args = ["--session", session, *args]

received = 0

def on_signal(number, frame):
    global received
    received = number

signal.signal(signal.SIGINT, on_signal)
signal.signal(signal.SIGTERM, on_signal)
processes = []
server = None
result = 1
try:
    server = subprocess.Popen([str(controller), *args], start_new_session=True)
    plan = None
    deadline = time.monotonic() + 10
    while server.poll() is None and not received and time.monotonic() < deadline:
        probe = subprocess.run([str(worker), session, "plan"], capture_output=True,
                               text=True, timeout=6)
        if probe.returncode == 0:
            plan = probe.stdout.splitlines()
            break
        time.sleep(0.02)
    if plan is not None:
        for line in plan:
            if received or server.poll() is not None:
                break
            role, index = line.split()
            processes.append(subprocess.Popen([str(worker), session, role, index],
                                              start_new_session=True))
        failed = False
        sent_signal = False
        while server.poll() is None:
            if received and not sent_signal:
                server.send_signal(received)
                sent_signal = True
            for process in processes:
                code = process.poll()
                if code is not None and code != 0 and not failed:
                    failed = True
                    server.terminate()
            time.sleep(0.01)
        result = server.returncode
        if failed and not received:
            result = 1
    elif server.poll() is not None:
        result = server.returncode
    else:
        server.send_signal(received or signal.SIGTERM)
        result = server.wait(timeout=8)
        if not received:
            result = 1
except (OSError, subprocess.TimeoutExpired) as error:
    print("Ошибка запуска независимых процессов:", error, file=sys.stderr)
    result = 1
finally:
    # при ошибке запуска не оставляем уже созданных участников
    if server is not None and server.poll() is None:
        server.terminate()
    for process in processes:
        if process.poll() is None:
            process.terminate()
    all_processes = processes + ([server] if server is not None else [])
    deadline = time.monotonic() + 8
    for process in all_processes:
        try:
            code = process.wait(timeout=max(0.1, deadline - time.monotonic()))
            if code != 0 and result == 0:
                result = 1
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            result = 1
sys.exit(result)
