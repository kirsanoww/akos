#!/usr/bin/env python3
import collections
import os
import pathlib
import random
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

BUILD = pathlib.Path(sys.argv.pop(1) if len(sys.argv) > 1 else 'build').resolve()

def values(line):
    return dict(word.split('=', 1) for word in line.split()[1:] if '=' in word)

class Run:
    def __init__(self, extra=(), workers=True, observers=1):
        self.temp = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self.temp.name)
        s = socket.socket()
        s.bind(('127.0.0.1', 0))
        self.port = s.getsockname()[1]
        s.close()
        self.processes = []
        self.files = []
        self.server = self.start('server', ['center_server', '127.0.0.1', str(self.port),
            '--parcels', '20', '--directions', '2', '--lines', '2', '--scanners', '2', '--sorters', '2',
            '--delay-ms', '1', '--startup-timeout', '10', '--check', *extra])
        self.wait_text('server', 'PLAN job=')
        self.plan = [values(s) for s in self.text('server').splitlines() if s.startswith('PLAN ')]
        ready = next(values(s) for s in self.text('server').splitlines() if s.startswith('READY '))
        until = time.monotonic() + 5
        while len(self.plan) != int(ready['jobs']):
            if time.monotonic() > until: raise AssertionError('incomplete plan')
            time.sleep(.01)
            self.plan = [values(s) for s in self.text('server').splitlines() if s.startswith('PLAN ')]
        self.observers = [self.observer('observer%d' % i) for i in range(observers)]
        self.workers = {}
        if workers:
            self.add_workers()

    def start(self, name, args):
        f = open(self.path / (name + '.txt'), 'w')
        self.files.append(f)
        p = subprocess.Popen([str(BUILD / args[0]), *args[1:]], stdout=f, stderr=f)
        self.processes.append(p)
        return p

    def worker(self, role, index, suffix=''):
        return self.start(role + str(index) + suffix, ['center_worker', '127.0.0.1', str(self.port), role, str(index), '--retry-seconds', '1'])

    def observer(self, name):
        p = self.start(name, ['center_observer', '127.0.0.1', str(self.port), '--retry-seconds', '1'])
        self.wait_text(name, 'END seq=')
        return p

    def add_workers(self, skip=()):
        for row in self.plan:
            key = (row['role'], int(row['index']))
            if key not in skip:
                self.workers[key] = self.worker(*key)

    def text(self, name='server'):
        return (self.path / (name + '.txt')).read_text()

    def wait_text(self, name, pattern, timeout=8):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            text = self.text(name)
            if pattern in text: return text
            if self.server.poll() is not None and name == 'server': break
            time.sleep(.01)
        raise AssertionError('missing %s in %s:\n%s' % (pattern, name, self.text(name)[-3000:]))

    def raw(self, hello=None):
        s = socket.create_connection(('127.0.0.1', self.port), timeout=3)
        s.settimeout(3)
        if hello: s.sendall(hello)
        return s

    def finish(self, timeout=30):
        code = self.server.wait(timeout=timeout)
        text = self.text()
        if code != 0: raise AssertionError('server %d:\n%s' % (code, text[-4000:]))
        for p in self.processes:
            if p is self.server or p.returncode is not None: continue
            if p.wait(timeout=5) != 0:
                f = self.files[self.processes.index(p)]
                raise AssertionError('client failed: %s\n%s' % (p.args, pathlib.Path(f.name).read_text()[-3000:]))
        return text

    def close(self):
        for p in self.processes:
            if p.poll() is None: p.terminate()
        for p in self.processes:
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
        for f in self.files: f.close()
        self.temp.cleanup()

class NetworkTests(unittest.TestCase):
    def run_case(self, extra=(), **kwargs):
        run = Run(extra, **kwargs)
        self.addCleanup(run.close)
        return run

    def validate(self, text, total, capacity=3, output=2, directions=2):
        # независимо восстанавливаем очереди, владельцев и стадии по журналу
        queues = collections.defaultdict(collections.deque)
        owners, locations, attempts = {}, {}, collections.Counter()
        final = set()
        seq = 0
        recognized = set()
        started = {}
        for line in text.splitlines():
            if not line.startswith('E '): continue
            e = values(line)
            self.assertEqual(int(e['seq']), seq + 1); seq += 1
            p, j, q = int(e['parcel']), int(e['job']), int(e['queue'])
            kind = e['event']
            if kind == 'TAKE':
                self.assertNotIn(j, owners)
                if q >= 0:
                    self.assertEqual(queues[q].popleft(), p)
                    self.assertEqual(locations[p], ('q', q))
                    self.assertEqual(len(queues[q]), int(e['count']))
                else: self.assertNotIn(p, locations)
                owners[j] = p; locations[p] = ('j', j)
            if kind == 'START':
                self.assertEqual(owners[j], p)
                if e['role'] == 'scanner': attempts[p] += 1
                started[j] = int(e['ms'])
            if kind == 'FINISH':
                self.assertEqual(owners[j], p)
                self.assertGreaterEqual(int(e['ms']), started[j])
                if e['role'] == 'scanner' and int(e['result']): recognized.add(p)
            if kind == 'PUT':
                self.assertEqual(owners.pop(j), p)
                queues[q].append(p); locations[p] = ('q', q)
                limit = output if 2 + directions <= q < 2 + 2 * directions else capacity
                self.assertLessEqual(len(queues[q]), limit)
                self.assertEqual(len(queues[q]), int(e['count']))
                if q != 0 and q != 2 + 2 * directions: self.assertIn(p, recognized)
                if 2 <= q < 2 + directions: self.assertEqual(q - 1, int(e['direction']))
                if 2 + directions <= q < 2 + 2 * directions: self.assertEqual(q - 1 - directions, int(e['direction']))
            if kind == 'FINAL':
                self.assertEqual(owners.pop(j), p)
                self.assertNotIn(p, final); final.add(p); locations[p] = ('final', 0)
                if int(e['stage']) == 10: self.assertIn(p, recognized)
        self.assertEqual(len(final), total)
        self.assertFalse(owners)
        self.assertTrue(all(not q for q in queues.values()))
        result = [values(s) for s in text.splitlines() if s.startswith('SUMMARY ')][-1]
        self.assertEqual(int(result['delivered']) + int(result['rejected']), total)
        self.assertEqual(int(result['pending']), 0)
        self.assertEqual(int(result['attempts']), sum(attempts.values()))
        self.assertIn('CLEANUP sockets_closed=1', text)
        self.assertNotIn('AddressSanitizer', text)
        self.assertNotIn('runtime error:', text)
        return result

    def test_success(self):
        r = self.run_case(['--error-percent', '0', '--parcels', '40'])
        result = self.validate(r.finish(), 40)
        self.assertEqual(int(result['delivered']), 40)
        self.assertEqual(int(result['attempts']), 40)

    def test_all_errors(self):
        r = self.run_case(['--error-percent', '100', '--parcels', '17', '--retries', '3'])
        result = self.validate(r.finish(), 17)
        self.assertEqual(int(result['rejected']), 17)
        self.assertEqual(int(result['attempts']), 68)

    def test_zero_retries(self):
        r = self.run_case(['--error-percent', '100', '--retries', '0'])
        result = self.validate(r.finish(), 20)
        self.assertEqual(int(result['attempts']), 20)

    def test_tiny_buffers(self):
        r = self.run_case(['--buffer-capacity', '1', '--output-capacity', '1', '--unload-interval', '30'])
        text = r.finish(); self.validate(text, 20, 1, 1)
        self.assertIn('event=WAIT', text)

    def test_random_cases(self):
        rng = random.Random(34)
        for _ in range(10):
            n, d, cap = rng.randint(1, 40), rng.randint(1, 4), rng.randint(1, 5)
            r = self.run_case(['--parcels', str(n), '--directions', str(d), '--buffer-capacity', str(cap),
                '--output-capacity', str(cap), '--error-percent', str(rng.randint(0,100)), '--retries', str(rng.randint(0,4)), '--delay-ms','0'])
            self.validate(r.finish(), n, cap, cap, d)

    def test_no_observer(self):
        r = self.run_case(observers=0)
        self.validate(r.finish(), 20)

    def test_multiple_observers_same_events(self):
        r = self.run_case(observers=3)
        text = r.finish(); self.validate(text,20)
        streams = [[s for s in r.text('observer%d'%i).splitlines() if s.startswith('E ') and 'parcel=0 ' not in s] for i in range(3)]
        self.assertEqual(streams[0],streams[1]); self.assertEqual(streams[1],streams[2])
        for i in range(3): self.assertIn('STOP complete', r.text('observer%d'%i))

    def test_observer_disconnect_reconnect_snapshot(self):
        r = self.run_case(['--delay-ms','20','--parcels','40'])
        r.wait_text('server','event=PUT')
        r.observers[0].terminate(); r.observers[0].wait(timeout=3)
        r.observer('observer_new')
        self.validate(r.finish(),40)
        text = r.text('observer_new')
        begin = next(values(s) for s in text.splitlines() if s.startswith('BEGIN '))
        end = next(values(s) for s in text.splitlines() if s.startswith('END '))
        self.assertEqual(begin['seq'],end['seq']); self.assertGreater(int(begin['seq']),0)
        self.assertEqual(sum(s.startswith('PARCEL ') for s in text.splitlines()),40)

    def test_reconnect_every_worker_role(self):
        for role in ['source','scanner','sorter','belt','unload']:
            r = self.run_case(['--delay-ms','30','--parcels','15','--error-percent','0'])
            name = role+'1'; r.wait_text(name, 'JOB ')
            r.workers[(role,1)].kill(); r.workers[(role,1)].wait()
            time.sleep(.1)
            r.workers[(role,1)] = r.worker(role,1,'_new')
            self.validate(r.finish(),15)
            self.assertIn('JOB ',r.text(name+'_new'))

    def test_missing_workers_timeout(self):
        r = self.run_case(['--startup-timeout','1'],workers=False)
        self.assertEqual(r.server.wait(timeout=5),1)
        self.assertIn('reason=idle_timeout',r.text())
        self.assertEqual(r.observers[0].wait(timeout=3),0)

    def test_shutdown_signals(self):
        for sig in [signal.SIGINT,signal.SIGTERM]:
            r = self.run_case(['--delay-ms','1000'])
            r.wait_text('source1','JOB ')
            r.server.send_signal(sig)
            self.assertEqual(r.server.wait(timeout=5),128+sig)
            for p in r.processes:
                if p is not r.server: self.assertEqual(p.wait(timeout=4),0)
            self.assertIn('STOP signal',r.text('observer0'))

    def test_partial_protocol_and_coalesced_lines(self):
        r = self.run_case(workers=False,observers=0)
        with r.raw() as s:
            for piece in [b'HE',b'LLO 1 ',b'observer\nPONG\n']:
                s.sendall(piece);time.sleep(.01)
            data = b''
            while b'END seq=' not in data: data += s.recv(4096)
            self.assertIn(b'WELCOME observer',data)
        r.add_workers(); self.validate(r.finish(),20)

    def test_bad_protocol_does_not_stop_server(self):
        r = self.run_case(workers=False,observers=0)
        for text in [b'HELLO 2 observer\n',b'x'*600,b'HELLO 1 worker scanner -1\n', b'HELLO 1 observer\x00\n',b'HELLO 1 worker scanner 99\n']:
            with r.raw(text) as s:
                try: self.assertEqual(s.recv(100),b'')
                except ConnectionResetError: pass
        r.add_workers();self.validate(r.finish(),20)

    def test_duplicate_role(self):
        r = self.run_case(workers=False)
        with r.raw(b'HELLO 1 worker scanner 1\n') as first:
            self.assertIn(b'WELCOME', first.recv(100))
            with r.raw(b'HELLO 1 worker scanner 1\n') as second:
                self.assertEqual(second.recv(100),b'')
        time.sleep(.05);r.add_workers();self.validate(r.finish(),20)

    def test_duplicate_completion(self):
        r = self.run_case(workers=False)
        r.add_workers(skip=[('source',1)])
        with r.raw(b'HELLO 1 worker source 1\n') as s:
            data=b''
            while b'JOB ' not in data: data += s.recv(4096)
            job=next(line for line in data.decode().splitlines() if line.startswith('JOB ')).split()
            time.sleep(int(job[3])/1000+.02)
            s.sendall(('DONE %s %s\n'%(job[1],job[4])).encode()*2)
            time.sleep(.05)
        r.worker('source',1,'_new');self.validate(r.finish(),20)

    def test_client_connect_before_server(self):
        r = self.run_case(workers=False,observers=0)
        # временная недоступность проверяется отдельным свободным портом
        with socket.socket() as s:
            s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
        p=r.start('early',['center_observer','127.0.0.1',str(port),'--retry-seconds','3'])
        time.sleep(.2)
        server=r.start('second',['center_server','127.0.0.1',str(port),'--parcels','1','--startup-timeout','1'])
        self.assertEqual(server.wait(timeout=5),1);self.assertEqual(p.wait(timeout=5),0)
        self.assertIn('WELCOME observer',r.text('early'))
        r.add_workers();r.finish()

    def test_server_crash_clients_exit(self):
        r = self.run_case(['--delay-ms','1000'])
        r.wait_text('source1','JOB ');r.server.kill();r.server.wait()
        for p in r.processes:
            if p is not r.server:self.assertEqual(p.wait(timeout=5),1)

    def test_seed_results(self):
        data=[]
        for _ in range(2):
            r=self.run_case(['--seed','1234','--parcels','40'])
            text=r.finish();self.validate(text,40)
            data.append(sorted((int(e['parcel']),int(e['direction']),int(e['attempts']),int(e['stage']))
                for e in [values(s) for s in text.splitlines() if s.startswith('E ') and 'event=FINAL ' in s]))
        self.assertEqual(data[0],data[1])

    def test_log_matches_console(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=pathlib.Path(tmp)/'events.txt'
            r=self.run_case(['--log',str(path)])
            text=r.finish();self.assertEqual(path.read_text(),text)

    def test_port_reuse(self):
        r=self.run_case();r.finish()
        p=r.start('reuse',['center_server','127.0.0.1',str(r.port),'--parcels','0'])
        self.assertEqual(p.wait(timeout=3),0)

    def test_zero_parcels(self):
        p=subprocess.run([str(BUILD/'center_server'),'127.0.0.1','9035','--parcels','0','--check'],capture_output=True,text=True,timeout=5)
        self.assertEqual(p.returncode,0,p.stderr);self.assertIn('pending=0',p.stdout)

    def test_invalid_arguments(self):
        for options in [['--parcels','-1'],['--seed','4294967296'],['--scanners','0'],['--delay-ms','60001'],['--unknown','1'],['--parcels','513'],['--log','']]:
            p=subprocess.run([str(BUILD/'center_server'),'127.0.0.1','9035',*options],capture_output=True,timeout=5)
            self.assertEqual(p.returncode,2)
        for args in [['center_server','127.0.0.1','0'],['center_worker','127.0.0.1','9035','bad','1'],['center_observer','127.0.0.1','abc']]:
            self.assertEqual(subprocess.run([str(BUILD/args[0]),*args[1:]],capture_output=True,timeout=5).returncode,2)

    def test_unwritable_log(self):
        p=subprocess.run([str(BUILD/'center_server'),'127.0.0.1','9035','--log','/nonexistent/path/log'],capture_output=True,timeout=5)
        self.assertEqual(p.returncode,1)

    def test_slow_observer_does_not_block(self):
        r=self.run_case(['--parcels','150','--error-percent','100','--retries','30','--delay-ms','0'],workers=False,observers=0)
        slow=socket.socket();slow.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
        slow.connect(('127.0.0.1',r.port));slow.sendall(b'HELLO 1 observer\n')
        self.addCleanup(slow.close)
        r.add_workers();self.validate(r.finish(),150)

    def test_maximum_batch_snapshot(self):
        r=self.run_case(['--parcels','512','--delay-ms','0','--error-percent','0'])
        text=r.finish();self.validate(text,512)
        self.assertEqual(sum(s.startswith('PARCEL ') for s in r.text('observer0').splitlines()),512)

    def test_processing_delay_and_overlap(self):
        r=self.run_case(['--delay-ms','20','--scan-time','5','--arrival-interval','1','--error-percent','0'])
        text=r.finish();self.validate(text,20)
        starts={};busy=set();overlap=False
        for line in text.splitlines():
            if not line.startswith('E '):continue
            e=values(line)
            if e['role']!='scanner':continue
            j=int(e['job'])
            if e['event']=='START':
                starts[j]=int(e['ms']);busy.add(j)
                if len(busy)>1:overlap=True
            if e['event']=='FINISH':
                self.assertGreaterEqual(int(e['ms'])-starts[j],100);busy.remove(j)
        self.assertTrue(overlap)

    def test_heartbeat_detects_silent_worker(self):
        r=self.run_case(workers=False)
        with r.raw(b'HELLO 1 worker scanner 1\n') as s:
            self.assertIn(b'WELCOME',s.recv(200))
            time.sleep(5.3)
            self.assertIn('event=DISCONNECT',r.text())
        r.add_workers();self.validate(r.finish(),20)

    def test_concurrent_servers(self):
        a=self.run_case(['--delay-ms','5']);b=self.run_case(['--delay-ms','5'])
        self.validate(a.finish(),20);self.validate(b.finish(),20)

    def test_duplicate_client_retry_is_bounded(self):
        r=self.run_case(workers=False)
        with r.raw(b'HELLO 1 worker scanner 1\n') as s:
            self.assertIn(b'WELCOME',s.recv(100))
            duplicate=r.worker('scanner',1,'_duplicate')
            self.assertEqual(duplicate.wait(timeout=4),1)
        time.sleep(.05);r.add_workers();self.validate(r.finish(),20)

    def test_manual_unloader_reconnect(self):
        r=self.run_case(['--error-percent','100','--retries','0','--delay-ms','20'])
        r.wait_text('unload3','JOB ')
        r.workers[('unload',3)].kill();r.workers[('unload',3)].wait()
        time.sleep(.1);r.worker('unload',3,'_new')
        result=self.validate(r.finish(),20)
        self.assertEqual(int(result['rejected']),20)

if __name__=='__main__':
    unittest.main(verbosity=2)
