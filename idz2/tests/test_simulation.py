"""проверки маршрутов, очередей и остановки многопоточной программы"""
import collections
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
FAULT_BINARY = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else None


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s\]]+)", line))


def execute(*args, **kwargs):
    return subprocess.run([BINARY, *map(str, args)], text=True,
                          capture_output=True, timeout=20, **kwargs)


class SimulationTests(unittest.TestCase):
    def validate(self, text, complete=True):
        lines = text.splitlines()
        config = fields(next(line for line in lines if line.startswith("CONFIG ")))
        totals = fields(next(line for line in lines if line.startswith("ИТОГ ")))
        n = int(config["parcels"])
        states, directions, attempts = {}, {}, {}
        devices, sources, owner = {}, {}, {}
        queues = collections.defaultdict(collections.deque)
        scan_count = errors = retries = waits = delivered = rejected = 0
        max_scanners = 0
        active_scanners = set()
        number = 0
        previous_time = 0

        def queue_in(key, parcel, f, capacity):
            queues[key].append(parcel)
            self.assertLessEqual(len(queues[key]), capacity)
            self.assertEqual(int(f["count"]), len(queues[key]))
            self.assertEqual(int(f["capacity"]), capacity)

        def queue_out(key, parcel):
            self.assertTrue(queues[key], key)
            self.assertEqual(queues[key].popleft(), parcel, "Нарушен FIFO")

        def acquire(parcel, f, tick, role):
            device = int(f["device"])
            first = 1 if role == "scanner" else int(config["scanners"]) + 1
            count = int(config["scanners"] if role == "scanner" else config["sorters"])
            if role == "conveyor":
                first = int(config["scanners"]) + int(config["sorters"]) + 1
                count = int(config["directions"])
                self.assertEqual(device - first + 1, directions[parcel])
            self.assertTrue(first <= device < first + count)
            self.assertNotIn(device, devices, "Устройство занято двумя посылками")
            devices[device] = (parcel, tick + int(f["duration_ms"]), role)
            owner[parcel] = device

        def finish(parcel, tick):
            device = owner[parcel]
            self.assertEqual(devices[device][0], parcel)
            self.assertGreaterEqual(tick, devices[device][1])

        def release(parcel):
            device = owner.pop(parcel)
            self.assertEqual(devices.pop(device)[0], parcel)

        for line in lines:
            if not line.startswith("[seq="):
                continue
            f = fields(line)
            number += 1
            self.assertEqual(int(f["seq"]), number)
            tick = int(f["time_ms"])
            self.assertGreaterEqual(tick, previous_time)
            previous_time = tick
            parcel = int(f["parcel"])
            self.assertTrue(1 <= parcel <= n)
            event = f["event"]
            state = states.get(parcel)
            cap, outcap = int(config["buffer_capacity"]), int(config["output_capacity"])
            if event == "ARRIVE":
                self.assertIsNone(state)
                source = int(f["source"])
                self.assertTrue(1 <= source <= int(config["lines"]))
                self.assertNotIn(source, sources)
                sources[source] = parcel
                states[parcel] = "source"
            elif event == "INPUT":
                self.assertEqual(state, "source")
                source = next(k for k, v in sources.items() if v == parcel)
                del sources[source]
                queue_in("input", parcel, f, cap)
                states[parcel] = "input"
            elif event == "SCAN_START":
                self.assertIn(state, ("input", "retry"))
                if state == "input":
                    queue_out("input", parcel)
                    acquire(parcel, f, tick, "scanner")
                else:
                    device = owner[parcel]
                    self.assertEqual(device, int(f["device"]))
                    devices[device] = (parcel, tick + int(f["duration_ms"]), "scanner")
                attempts[parcel] = attempts.get(parcel, 0) + 1
                self.assertEqual(int(f["attempt"]), attempts[parcel])
                self.assertLessEqual(attempts[parcel], int(config["retries"]) + 1)
                states[parcel] = "scan"
                active_scanners.add(owner[parcel])
                max_scanners = max(max_scanners, len(active_scanners))
                scan_count += 1
            elif event == "SCAN_END":
                self.assertEqual(state, "scan")
                finish(parcel, tick)
                active_scanners.remove(owner[parcel])
                states[parcel] = "scanned"
            elif event == "SCAN_ERROR":
                self.assertEqual(state, "scanned")
                states[parcel] = "error"
                errors += 1
            elif event == "RETRY":
                self.assertEqual(state, "error")
                self.assertLessEqual(attempts[parcel], int(config["retries"]))
                states[parcel] = "retry"
                retries += 1
            elif event == "RECOGNIZED":
                self.assertEqual(state, "scanned")
                directions[parcel] = int(f["direction"])
                self.assertTrue(1 <= directions[parcel] <= int(config["directions"]))
                states[parcel] = "recognized"
            elif event == "SORT_QUEUE":
                self.assertEqual(state, "recognized")
                release(parcel)
                queue_in("sort", parcel, f, cap)
                states[parcel] = "sort_queue"
            elif event == "REJECT":
                self.assertEqual(state, "error")
                self.assertEqual(attempts[parcel], int(config["retries"]) + 1)
                release(parcel)
                queue_in("manual", parcel, f, cap)
                states[parcel] = "manual"
                rejected += 1
            elif event == "SORT_START":
                self.assertEqual(state, "sort_queue")
                queue_out("sort", parcel)
                acquire(parcel, f, tick, "sorter")
                states[parcel] = "sort"
            elif event == "SORT_END":
                self.assertEqual(state, "sort")
                finish(parcel, tick)
                states[parcel] = "sorted"
            elif event == "CONVEYOR_QUEUE":
                self.assertEqual(state, "sorted")
                self.assertEqual(int(f["direction"]), directions[parcel])
                release(parcel)
                queue_in(("belt", directions[parcel]), parcel, f, cap)
                states[parcel] = "belt_queue"
            elif event == "TRANSPORT_START":
                self.assertEqual(state, "belt_queue")
                queue_out(("belt", directions[parcel]), parcel)
                acquire(parcel, f, tick, "conveyor")
                travel = (int(config["conveyor_length"]) + int(config["conveyor_speed"]) - 1) // int(config["conveyor_speed"])
                self.assertEqual(int(f["duration_ms"]), travel * int(config["delay_ms"]))
                states[parcel] = "belt"
            elif event == "TRANSPORT_END":
                self.assertEqual(state, "belt")
                finish(parcel, tick)
                states[parcel] = "transported"
            elif event == "DELIVER":
                self.assertEqual(state, "transported")
                self.assertEqual(int(f["direction"]), directions[parcel])
                release(parcel)
                queue_in(("output", directions[parcel]), parcel, f, outcap)
                states[parcel] = "output"
                delivered += 1
            elif event == "UNLOAD":
                self.assertEqual(state, "output")
                queue_out(("output", directions[parcel]), parcel)
                states[parcel] = "done"
            elif event == "MANUAL_UNLOAD":
                self.assertEqual(state, "manual")
                queue_out("manual", parcel)
                states[parcel] = "rejected"
            elif event == "WAIT":
                self.assertIsNotNone(state)
                key = f["queue"]
                if key in ("output", "belt"):
                    key = (key, int(f["direction"]))
                self.assertEqual(len(queues[key]), outcap if f["queue"] == "output" else cap)
                waits += 1
            else:
                self.fail("Неизвестное событие: " + event)
        for key, value in [("delivered", delivered), ("rejected", rejected),
                           ("scan_attempts", scan_count), ("errors", errors),
                           ("repeats", retries), ("waits", waits), ("formed", len(states))]:
            self.assertEqual(int(totals[key]), value)
        self.assertEqual(sum(int(totals[key]) for key in
                             ("delivered", "rejected", "unfinished", "not_generated")), n)
        if complete:
            self.assertEqual(totals["status"], "completed")
            self.assertEqual(set(states), set(range(1, n + 1)))
            self.assertTrue(all(s in ("output", "done", "manual", "rejected") for s in states.values()))
            self.assertFalse(devices)
            self.assertFalse(sources)
            self.assertEqual(int(totals["unfinished"]), 0)
            self.assertEqual(int(totals["not_generated"]), 0)
            expected = n + int(config["lines"]) + int(config["scanners"]) + int(config["sorters"]) + 2 * int(config["directions"]) + 2
            self.assertEqual(int(totals["threads"]), expected)
        return totals, attempts, directions, max_scanners

    def completed(self, *args):
        run = execute(*args)
        self.assertEqual(run.returncode, 0, run.stderr + run.stdout[-2000:])
        self.assertEqual(run.stderr, "")
        return self.validate(run.stdout), run.stdout

    def test_defaults_and_empty(self):
        self.completed()
        self.completed("--parcels", 0, "--check")

    def test_no_errors(self):
        (totals, _, _, _), _ = self.completed("--parcels", 40, "--error-percent", 0, "--check")
        self.assertEqual(int(totals["delivered"]), 40)
        self.assertEqual(int(totals["scan_attempts"]), 40)

    def test_exact_retry_limit(self):
        for retries in (0, 1, 3):
            with self.subTest(retries=retries):
                (totals, _, _, _), _ = self.completed("--parcels", 17, "--error-percent", 100,
                    "--retries", retries, "--buffer-capacity", 1, "--check")
                self.assertEqual(int(totals["rejected"]), 17)
                self.assertEqual(int(totals["scan_attempts"]), 17 * (retries + 1))

    def test_congestion(self):
        (totals, _, _, _), text = self.completed("--parcels", 20, "--directions", 1,
            "--lines", 6, "--scanners", 4, "--buffer-capacity", 1, "--output-capacity", 1,
            "--unload-interval", 25, "--error-percent", 0, "--check")
        self.assertGreater(int(totals["waits"]), 0)
        self.assertIn("event=WAIT queue=output", text)
        self.assertIn("event=UNLOAD", text)

    def test_actual_overlap_and_timing(self):
        (_, _, _, overlap), _ = self.completed("--parcels", 12, "--lines", 4,
            "--scanners", 4, "--scan-time", 20, "--delay-ms", 2,
            "--conveyor-length", 5, "--conveyor-speed", 2, "--error-percent", 0, "--check")
        self.assertGreaterEqual(overlap, 2, "Сканеры должны работать одновременно")

    def test_seed_and_log(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / "events.log"
            first, text = self.completed("--parcels", 60, "--seed", 0, "--error-percent", 55, "--log", log)
            self.assertEqual(log.read_text(), text)
            second, _ = self.completed("--parcels", 60, "--seed", 0, "--error-percent", 55, "--check")
            # порядок строк зависит от планировщика, судьба каждой посылки - от seed
            self.assertEqual(first[1:3], second[1:3])
            for key in ("delivered", "rejected", "scan_attempts", "errors", "repeats"):
                self.assertEqual(first[0][key], second[0][key])
            third, _ = self.completed("--parcels", 60, "--seed", 1, "--error-percent", 55)
            self.assertNotEqual(first[1:3], third[1:3])

    def test_varied_configurations(self):
        rng = random.Random(34)
        for seed in range(25):
            with self.subTest(seed=seed):
                self.completed("--parcels", rng.randint(1, 70), "--lines", rng.randint(1, 6),
                    "--scanners", rng.randint(1, 5), "--sorters", rng.randint(1, 5),
                    "--directions", rng.randint(1, 5), "--buffer-capacity", rng.randint(1, 5),
                    "--output-capacity", rng.randint(1, 3), "--error-percent", rng.randint(0, 100),
                    "--retries", rng.randint(0, 4), "--scan-time", rng.randint(1, 5),
                    "--sort-time", rng.randint(1, 5), "--conveyor-length", rng.randint(1, 9),
                    "--conveyor-speed", rng.randint(1, 10), "--seed", seed, "--delay-ms", 0, "--check")

    def test_queue_wraparound(self):
        for capacity in (1, 3, 7):
            for errors in (0, 100):
                with self.subTest(capacity=capacity, errors=errors):
                    self.completed("--parcels", 100, "--directions", 1, "--lines", 5,
                        "--scanners", 3, "--sorters", 3, "--buffer-capacity", capacity,
                        "--output-capacity", capacity, "--error-percent", errors, "--delay-ms", 0, "--check")

    def test_repeated_runs(self):
        for _ in range(10):
            self.completed("--parcels", 30, "--delay-ms", 0, "--check")

    def test_invalid_arguments(self):
        cases = [("--parcels", -1), ("--parcels", 513), ("--scanners", 0), ("--directions", 65),
                 ("--buffer-capacity", 0), ("--error-percent", 101), ("--retries", 101),
                 ("--seed", 4294967296), ("--seed", "x"), ("--delay-ms", 60001),
                 ("--parcels", ""), ("--parcels", " 1"), ("--parcels", "3.2"),
                 ("--parcels", "999999999999999999999999"), ("--parcels",),
                 ("--unknown", 1), ("--log", "")]
        for args in cases:
            with self.subTest(args=args):
                self.assertEqual(execute(*args).returncode, 2)

    def test_help_and_log_error(self):
        self.assertEqual(execute("--help").returncode, 0)
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(execute("--log", folder).returncode, 1)

    def test_broken_output(self):
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            run = subprocess.run([BINARY], stdout=write_fd, stderr=subprocess.PIPE, timeout=3)
        finally:
            os.close(write_fd)
        self.assertEqual(run.returncode, 1)

    def test_interrupt_sleep_and_full_queue(self):
        cases = [(["--delay-ms", "60000"], "event=SCAN_START"),
                 (["--directions", "1", "--buffer-capacity", "1", "--output-capacity", "1",
                   "--unload-interval", "60000", "--error-percent", "0"], "event=WAIT queue=output")]
        for sig in (signal.SIGINT, signal.SIGTERM):
            for args, marker in cases:
                with self.subTest(signal=sig, marker=marker), tempfile.TemporaryDirectory() as folder:
                    out, log = Path(folder) / "stdout", Path(folder) / "log"
                    with out.open("w") as stream:
                        process = subprocess.Popen([BINARY, "--parcels", "40", "--check", "--log", str(log), *args],
                                                   stdout=stream, stderr=subprocess.PIPE)
                        try:
                            deadline = time.monotonic() + 5
                            while marker not in out.read_text():
                                if time.monotonic() > deadline or process.poll() is not None:
                                    self.fail("Не дождались события: " + marker)
                                time.sleep(0.005)
                            os.kill(process.pid, signal.SIGUSR1)
                            os.kill(process.pid, sig)
                            _, stderr = process.communicate(timeout=3)
                        finally:
                            if process.poll() is None:
                                process.kill()
                                process.wait()
                            process.stderr.close()
                    self.assertEqual(process.returncode, 128 + sig, stderr)
                    self.assertEqual(stderr, b"")
                    self.assertEqual(log.read_text(), out.read_text())
                    totals, _, _, _ = self.validate(out.read_text(), complete=False)
                    self.assertEqual(totals["status"], "interrupted")
                    self.assertEqual(int(totals["signal"]), sig)

    @unittest.skipUnless(FAULT_BINARY, "нет тестовой сборки с отказом pthread_create")
    def test_partial_thread_creation_failure(self):
        for after in (0, 1, 7, 21, 24, 29):
            with self.subTest(after=after):
                env = os.environ.copy()
                env["AKOS_FAIL_AFTER"] = str(after)
                run = subprocess.run([FAULT_BINARY, "--check"], env=env,
                                     capture_output=True, text=True, timeout=5)
                self.assertEqual(run.returncode, 1, run.stdout)
                self.assertIn("Не удалось создать", run.stderr)
                self.assertIn("status=error", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
