"""проверки консольной программы по её наблюдаемым событиям (Python 3)"""
import os
import random
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

BINARY = str(Path(sys.argv.pop(1)).resolve())
CHECK = "--check" in sys.argv
if CHECK:
    sys.argv.remove("--check")


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def execute(*args, check=None):
    if check is None:
        check = CHECK
    command = [BINARY]
    if check:
        command.append("--check")
    command.extend(map(str, args))
    return subprocess.run(command, text=True, capture_output=True, timeout=15)


class SimulationTests(unittest.TestCase):
    def completed(self, *args, check=None):
        run = execute(*args, check=check)
        self.assertEqual(run.returncode, 0, run.stderr + run.stdout[-2000:])
        self.assertEqual(run.stderr, "")
        config = fields(next(s for s in run.stdout.splitlines() if s.startswith("CONFIG ")))
        totals = fields(next(s for s in run.stdout.splitlines() if s.startswith("ИТОГ ")))
        self.assertEqual(totals["status"], "completed")
        n = int(config["parcels"])
        self.assertEqual(int(totals["formed"]), n)
        self.assertEqual(int(totals["delivered"]) + int(totals["rejected"]), n)
        self.assertEqual(int(totals["unfinished"]), 0)
        self.assertEqual(int(totals["not_generated"]), 0)

        # независимо восстанавливаем маршрут, занятость устройств и очередей
        # по выводу, а не доверяем только счётчикам самой программы
        states, directions, attempts, devices = {}, {}, {}, {}
        queues = {"input": [], "sort_queue": [], "manual": []}
        delivered = rejected = errors = retries = waits = 0

        def queue_change(name, delta, limit, parcel):
            if name not in queues:
                queues[name] = []
            if delta == 1:
                queues[name].append(parcel)
            else:
                self.assertTrue(queues[name], "Извлечение из пустой очереди")
                self.assertEqual(queues[name].pop(0), parcel, "Нарушен порядок FIFO")
            self.assertLessEqual(len(queues[name]), limit)

        def acquire(kind, slot, parcel, ready):
            key = kind, slot
            self.assertNotIn(key, devices)
            devices[key] = (parcel, ready)

        def release(kind, parcel, tick):
            matches = [key for key, value in devices.items()
                       if key[0] == kind and value[0] == parcel]
            self.assertEqual(len(matches), 1)
            self.assertGreaterEqual(tick, devices[matches[0]][1])
            del devices[matches[0]]

        cap, outcap = int(config["buffer_capacity"]), int(config["output_capacity"])
        previous_tick = 0
        for line in run.stdout.splitlines():
            if not line.startswith("[t="):
                continue
            f = fields(line)
            tick = int(f["t"].rstrip("]"))
            self.assertGreaterEqual(tick, previous_tick)
            previous_tick = tick
            parcel, event = int(f["parcel"]), f["event"]
            state = states.get(parcel)
            if event == "ARRIVE":
                self.assertIsNone(state)
                states[parcel] = "source"
            elif event == "INPUT":
                self.assertEqual(state, "source")
                states[parcel] = "input"
                queue_change("input", 1, cap, parcel)
            elif event == "SCAN_START":
                self.assertEqual(state, "input")
                states[parcel] = "scan"
                attempts[parcel] = 1
                queue_change("input", -1, cap, parcel)
                acquire("scanner", f["Сканер"], parcel, int(f["ready"]))
            elif event == "SCAN_END":
                self.assertEqual(state, "scan")
                states[parcel] = "scanned"
            elif event == "SCAN_ERROR":
                self.assertEqual(state, "scanned")
                states[parcel] = "error"
                errors += 1
            elif event == "RETRY":
                self.assertEqual(state, "error")
                attempts[parcel] += 1
                self.assertEqual(int(f["attempt"]), attempts[parcel])
                self.assertLessEqual(attempts[parcel], 1 + int(config["retries"]))
                keys = [k for k, v in devices.items() if k[0] == "scanner" and v[0] == parcel]
                self.assertEqual(len(keys), 1)
                self.assertGreaterEqual(tick, devices[keys[0]][1])
                devices[keys[0]] = (parcel, int(f["ready"]))
                states[parcel] = "scan"
                retries += 1
            elif event == "RECOGNIZED":
                self.assertEqual(state, "scanned")
                directions[parcel] = int(f["direction"])
                self.assertTrue(1 <= directions[parcel] <= int(config["directions"]))
                states[parcel] = "recognized"
            elif event == "SORT_QUEUE":
                self.assertEqual(state, "recognized")
                release("scanner", parcel, tick)
                states[parcel] = "sort_queue"
                queue_change("sort_queue", 1, cap, parcel)
            elif event == "REJECT":
                self.assertEqual(state, "error")
                self.assertEqual(attempts[parcel], 1 + int(config["retries"]))
                release("scanner", parcel, tick)
                states[parcel] = "manual"
                queue_change("manual", 1, cap, parcel)
                rejected += 1
            elif event == "SORT_START":
                self.assertEqual(state, "sort_queue")
                queue_change("sort_queue", -1, cap, parcel)
                states[parcel] = "sorting"
                acquire("sorter", f["Сортировщик"], parcel, int(f["ready"]))
            elif event == "SORT_END":
                self.assertEqual(state, "sorting")
                self.assertEqual(int(f["direction"]), directions[parcel])
                states[parcel] = "sorted"
            elif event == "CONVEYOR_QUEUE":
                self.assertEqual(state, "sorted")
                release("sorter", parcel, tick)
                states[parcel] = "conveyor_queue"
                queue_change(("conveyor", directions[parcel]), 1, cap, parcel)
            elif event == "TRANSPORT_START":
                self.assertEqual(state, "conveyor_queue")
                self.assertEqual(int(f["Конвейер"]), directions[parcel])
                queue_change(("conveyor", directions[parcel]), -1, cap, parcel)
                states[parcel] = "transport"
                acquire("conveyor", f["Конвейер"], parcel, int(f["ready"]))
            elif event == "DELIVER":
                self.assertEqual(state, "transport")
                self.assertEqual(int(f["direction"]), directions[parcel])
                release("conveyor", parcel, tick)
                states[parcel] = "output"
                queue_change(("output", directions[parcel]), 1, outcap, parcel)
                delivered += 1
            elif event == "UNLOAD":
                self.assertEqual(state, "output")
                queue_change(("output", directions[parcel]), -1, outcap, parcel)
                states[parcel] = "done"
            elif event == "MANUAL_UNLOAD":
                self.assertEqual(state, "manual")
                queue_change("manual", -1, cap, parcel)
                states[parcel] = "rejected"
            elif event == "WAIT":
                self.assertIsNotNone(state)
                waits += 1
            else:
                self.fail("Неизвестное событие: " + event)
        self.assertEqual(set(states), set(range(1, n + 1)))
        self.assertTrue(all(s in {"output", "done", "manual", "rejected"} for s in states.values()))
        self.assertFalse(devices)
        for key, value in [("delivered", delivered), ("rejected", rejected),
                           ("errors", errors), ("repeats", retries), ("waits", waits),
                           ("scan_attempts", sum(attempts.values()))]:
            self.assertEqual(int(totals[key]), value)
        return totals, run.stdout

    def test_defaults(self):
        self.completed()

    def test_empty(self):
        totals, _ = self.completed("--parcels", 0)
        self.assertEqual(int(totals["ticks"]), 0)

    def test_no_errors(self):
        totals, _ = self.completed("--parcels", 50, "--error-percent", 0)
        self.assertEqual(int(totals["delivered"]), 50)
        self.assertEqual(int(totals["scan_attempts"]), 50)

    def test_all_errors_and_exact_retry_limit(self):
        for repeats in (0, 1, 3):
            with self.subTest(repeats=repeats):
                totals, _ = self.completed("--parcels", 17, "--error-percent", 100,
                    "--retries", repeats, "--buffer-capacity", 1,
                    "--scanners", 4, "--unload-interval", 30)
                self.assertEqual(int(totals["rejected"]), 17)
                self.assertEqual(int(totals["scan_attempts"]), 17 * (repeats + 1))

    def test_congestion_and_output_unloading(self):
        totals, output = self.completed("--parcels", 40, "--directions", 1,
            "--lines", 8, "--scanners", 4, "--sorters", 3, "--buffer-capacity", 1,
            "--output-capacity", 1, "--unload-interval", 50, "--error-percent", 0)
        self.assertGreater(int(totals["waits"]), 0)
        self.assertIn("Выход заполнен", output)
        self.assertEqual(int(totals["delivered"]), 40)

    def test_exact_timing_and_ceil_transport_time(self):
        totals, output = self.completed("--parcels", 1, "--error-percent", 0,
                                       "--conveyor-length", 5, "--conveyor-speed", 2)
        self.assertEqual(int(totals["ticks"]), 10)
        self.assertIn("[t=7] parcel=1 event=TRANSPORT_START", output)
        self.assertIn("[t=10] parcel=1 event=DELIVER", output)

    def test_reproducibility_and_log(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / "events.log")
            _, first = self.completed("--seed", 0, "--log", path)
            self.assertEqual(Path(path).read_text(), first)
            _, second = self.completed("--seed", 0)
            self.assertEqual(first, second)
            _, third = self.completed("--seed", 1)
            self.assertNotEqual(first, third)

    def test_varied_configurations(self):
        rng = random.Random(34)
        for seed in range(35):
            with self.subTest(seed=seed):
                self.completed("--seed", seed, "--parcels", rng.randint(1, 60),
                    "--lines", rng.randint(1, 6), "--directions", rng.randint(1, 6),
                    "--scanners", rng.randint(1, 5), "--sorters", rng.randint(1, 5),
                    "--buffer-capacity", rng.randint(1, 4), "--output-capacity", rng.randint(1, 3),
                    "--error-percent", rng.randint(0, 100), "--retries", rng.randint(0, 5),
                    "--scan-time", rng.randint(1, 5), "--sort-time", rng.randint(1, 5),
                    "--conveyor-length", rng.randint(1, 10), "--conveyor-speed", rng.randint(1, 12),
                    "--unload-interval", rng.randint(1, 30))

    def test_queue_wraparound(self):
        for capacity in (1, 3, 7):
            for percent in (0, 100):
                with self.subTest(capacity=capacity, error_percent=percent):
                    self.completed("--parcels", 120, "--directions", 1,
                        "--lines", 6, "--scanners", 4, "--sorters", 2,
                        "--buffer-capacity", capacity, "--output-capacity", capacity,
                        "--unload-interval", 17, "--error-percent", percent)

    def test_check_mode_keeps_results(self):
        for count in (0, 80):
            with self.subTest(parcels=count):
                args = ("--parcels", count, "--seed", 34, "--buffer-capacity", 3,
                        "--output-capacity", 2, "--error-percent", 45, "--retries", 2)
                plain_totals, plain = self.completed(*args, check=False)
                check_totals, checked = self.completed(*args, check=True)
                plain_lines = [line for line in plain.splitlines() if not line.startswith("CONFIG ")]
                checked_lines = [line for line in checked.splitlines() if not line.startswith("CONFIG ")]
                self.assertEqual(plain_lines, checked_lines)
                self.assertEqual(plain_totals, check_totals)
                self.assertIn("check=0", plain)
                self.assertIn("check=1", checked)

    def test_invalid_arguments(self):
        cases = [("--parcels", "-1"), ("--scanners", "0"), ("--lines", "65"),
                 ("--directions", "0"), ("--buffer-capacity", "0"), ("--error-percent", "101"),
                 ("--conveyor-speed", "0"), ("--unload-interval", "0"), ("--delay-ms", "60001"),
                 ("--seed", "4294967296"), ("--retries", "101"), ("--parcels", "3.2"),
                 ("--parcels", ""), ("--parcels", " 1"), ("--parcels", "99999999999999999999999"),
                 ("--unknown", "1"), ("--parcels",), ("--log", "")]
        for args in cases:
            with self.subTest(args=args):
                run = execute(*args)
                self.assertEqual(run.returncode, 2)
                self.assertTrue(run.stderr)

    def test_help(self):
        self.assertEqual(execute("--help").returncode, 0)

    def test_cannot_open_log(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(execute("--log", folder).returncode, 1)

    def test_interrupt_and_save_partial_results(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=sig), tempfile.TemporaryDirectory() as folder:
                output, log = Path(folder) / "stdout", Path(folder) / "log"
                with output.open("w") as stream:
                    command = [BINARY]
                    if CHECK:
                        command.append("--check")
                    process = subprocess.Popen([*command, "--parcels", "100", "--delay-ms", "60000",
                                                "--log", str(log)], stdout=stream, stderr=subprocess.PIPE)
                    try:
                        deadline = time.monotonic() + 4
                        while "event=INPUT" not in output.read_text():
                            if time.monotonic() > deadline or process.poll() is not None:
                                self.fail("Программа не начала моделирование")
                            time.sleep(0.01)
                        os.kill(process.pid, sig)
                        _, stderr = process.communicate(timeout=3)
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait()
                self.assertEqual(process.returncode, 128 + sig)
                self.assertEqual(stderr, b"")
                text = output.read_text()
                self.assertEqual(log.read_text(), text)
                totals = fields(next(s for s in text.splitlines() if s.startswith("ИТОГ ")))
                self.assertEqual(totals["status"], "interrupted")
                self.assertEqual(sum(int(totals[k]) for k in
                                    ("delivered", "rejected", "unfinished", "not_generated")), 100)


if __name__ == "__main__":
    unittest.main(verbosity=2)
