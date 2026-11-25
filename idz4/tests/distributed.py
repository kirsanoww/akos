#!/usr/bin/env python3
"""сервер, наблюдатель и рабочие клиенты в трёх сетевых пространствах Linux"""
import json
import os
import subprocess
import time

prefix='akos4-check-'+str(os.getpid())
network=prefix+'-net'
names=[]
def docker(*args):
    return subprocess.check_output(['docker',*args],text=True).strip()
try:
    docker('network','create','--internal',network)
    server=prefix+'-server';names.append(server)
    docker('run','-d','--init','--name',server,'--network',network,'akos-idz4',
        './build/center_server','0.0.0.0','9034','--parcels','30','--error-percent','0','--delay-ms','5','--check')
    ip=json.loads(docker('inspect',server))[0]['NetworkSettings']['Networks'][network]['IPAddress']
    until=time.monotonic()+10
    while 'READY ' not in docker('logs',server):
        if time.monotonic()>until:raise RuntimeError('server not ready')
        time.sleep(.1)
    observer=prefix+'-observer';names.append(observer)
    docker('run','-d','--init','--name',observer,'--network',network,'akos-idz4','./build/center_observer',ip,'9034')
    until=time.monotonic()+10
    while 'END seq=' not in docker('logs',observer):
        if time.monotonic()>until:raise RuntimeError('observer not ready')
        time.sleep(.1)
    workers=prefix+'-workers';names.append(workers)
    docker('run','-d','--init','--name',workers,'--network',network,'akos-idz4','python3','scripts/run_workers.py',ip,'9034')
    for name in [server,observer,workers]:
        code=subprocess.check_output(['docker','wait',name],text=True,timeout=45).strip()
        assert code=='0',(name,code,docker('logs',name)[-2000:])
    text=docker('logs',server)
    assert 'delivered=30 rejected=0 pending=0' in text
    assert 'STOP complete' in docker('logs',observer)
    print('PASS: 3 Linux containers, separate network namespaces, TCP, 30 deliveries, all exit codes 0')
    print(next(line for line in text.splitlines() if line.startswith('SUMMARY ')))
finally:
    for name in names:
        subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run(['docker','network','rm',network],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
