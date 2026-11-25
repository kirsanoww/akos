#!/usr/bin/env python3
import argparse
import pathlib
import signal
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('ip')
parser.add_argument('port')
parser.add_argument('--build-dir', default='build')
for name, default in [('lines',2),('scanners',2),('sorters',2),('directions',3)]:
    parser.add_argument('--'+name, type=int, default=default)
args = parser.parse_args()
for value in [args.lines,args.scanners,args.sorters,args.directions]:
    if not 1 <= value <= 16: parser.error('число устройств: 1..16')
program = str(pathlib.Path(args.build_dir).resolve()/'center_worker')
children=[]

def stop(sig, frame):
    for p in children:
        if p.poll() is None:p.send_signal(sig)

signal.signal(signal.SIGINT,stop)
signal.signal(signal.SIGTERM,stop)
code=0
try:
    for role,count in [('source',args.lines),('scanner',args.scanners),('sorter',args.sorters),('belt',args.directions),('unload',args.directions+1)]:
        for i in range(1,count+1):
            children.append(subprocess.Popen([program,args.ip,args.port,role,str(i)]))
    for p in children:
        if p.wait()!=0:code=1
finally:
    stop(signal.SIGTERM,None)
    for p in children:
        try:p.wait(timeout=3)
        except subprocess.TimeoutExpired:p.kill();p.wait()
sys.exit(code)
