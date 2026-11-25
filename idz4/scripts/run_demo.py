#!/usr/bin/env python3
"""запускаем отдельные C-программы, данные передаются только по TCP"""
import argparse
import pathlib
import signal
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', default='build')
parser.add_argument('--ip', default='127.0.0.1')
parser.add_argument('--port', type=int, default=9034)
parser.add_argument('--observers', type=int, default=2)
args, options = parser.parse_known_args()
if args.observers < 0 or args.observers > 32:
    parser.error('observers: 0..32')
folder = pathlib.Path(args.build_dir).resolve()
logs = pathlib.Path('logs')
logs.mkdir(exist_ok=True)
server = subprocess.Popen([str(folder / 'center_server'), args.ip, str(args.port), *options],
                          stdout=subprocess.PIPE, text=True)
children = []
files = []

def stop(sig, frame):
    if server.poll() is None:
        server.send_signal(sig)

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)
try:
    # PLAN задаёт точный набор ролей для выбранной конфигурации
    count = None
    total = None
    plan = []
    for line in server.stdout:
        print(line, end='', flush=True)
        if line.startswith('READY '):
            ready = dict(x.split('=', 1) for x in line.split()[1:])
            count = int(ready['jobs'])
            total = int(ready['total'])
        if line.startswith('PLAN '):
            plan.append(dict(x.split('=', 1) for x in line.split()[1:]))
        if count is not None and len(plan) == count:
            break
    if count is not None and total != 0:
        for i in range(args.observers):
            f = open(logs / ('observer_%d.txt' % (i + 1)), 'w')
            files.append(f)
            children.append(subprocess.Popen([str(folder / 'center_observer'), args.ip, str(args.port)], stdout=f, stderr=f))
        for role in plan:
            f = open(logs / ('%s_%s.txt' % (role['role'], role['index'])), 'w')
            files.append(f)
            children.append(subprocess.Popen([str(folder / 'center_worker'), args.ip, str(args.port), role['role'], role['index']], stdout=f, stderr=f))
    for line in server.stdout:
        print(line, end='', flush=True)
    code = server.wait()
    for process in children:
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=3)
            code = 1
        if code == 0 and process.returncode != 0:
            code = 1
finally:
    if server.poll() is None:
        server.terminate()
        server.wait(timeout=5)
    for process in children:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
    for f in files:
        f.close()
sys.exit(code)
