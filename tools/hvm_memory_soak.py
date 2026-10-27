import zipfile
#!/usr/bin/env python3
import argparse
import csv
import json
import logging
import os
import platform
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any

try:
    import psutil
except ImportError:
    psutil = None

ROOT = Path(__file__).resolve().parent.parent

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("memory_soak")

class TelemetryAdapter:
    def __init__(self, pid: int, interval: float = 1.0):
        self.pid = pid
        self.interval = interval
        self._samples: List[Dict[str, Any]] = []
        self._stop_event = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._process = None

        if psutil:
            try:
                self._process = psutil.Process(self.pid)
            except psutil.NoSuchProcess:
                logger.warning(f"Process {pid} not found for telemetry.")

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop_event.set()
        if self._thread.is_alive():
            self._thread.join(timeout=2.0)

    def _run(self):
        while not self._stop_event.is_set():
            if self._process and self._process.is_running():
                try:
                    rss = self._process.memory_info().rss
                    vms = self._process.memory_info().vms
                    for child in self._process.children(recursive=True):
                        try:
                            c_info = child.memory_info()
                            rss += c_info.rss
                            vms += c_info.vms
                        except psutil.NoSuchProcess:
                            pass
                    sample = {
                        "timestamp": time.time(),
                        "rss_mb": rss / (1024 * 1024),
                        "vms_mb": vms / (1024 * 1024),
                        "stage": "running",
                    }
                    self._samples.append(sample)
                except psutil.NoSuchProcess:
                    break
                except Exception as e:
                    logger.debug(f"Telemetry error: {e}")
            time.sleep(self.interval)

    def get_samples(self) -> List[Dict[str, Any]]:
        return self._samples

class Scenario:
    def __init__(self, hvm: Path, app: Path, out_dir: Path, timeout: float):
        self.hvm = hvm
        self.app = app
        self.out_dir = out_dir
        self.timeout = timeout
        self.process = None
        self.telemetry = None
        self.stages_completed = []
        self.issues = []

    def setup(self):
        pass

    def execute_cycle(self, cycle: int) -> str:
        """Executes one cycle, returns status (e.g. 'ok', 'failed', 'unsupported')"""
        return "unsupported"

    def teardown(self):
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
        if self.telemetry:
            self.telemetry.stop()

class ClashRoyaleScenario(Scenario):
    def setup(self):
        # We need to start the offline server (clash_protocol_probe.py)
        self.session_dir = self.out_dir / "cr_session"
        self.session_dir.mkdir(parents=True, exist_ok=True)
        self.peer_log = (self.session_dir / "peer.log").open("w")
        self.cr_server = subprocess.Popen(
            [
                sys.executable,
                str(ROOT / "tools" / "clash_protocol_probe.py"),
                "--out", str(self.session_dir),
                "--login-ok",
                "--state", str(self.out_dir / "identity.json"),
                "--home-client", str(self.app),
                "--game-state", str(self.out_dir / "home-state.json"),
                "--seconds", str(self.timeout)
            ],
            stdout=self.peer_log, stderr=subprocess.STDOUT
        )

        # Wait for peer to initialize
        for _ in range(100):
            if self.cr_server.poll() is not None:
                raise RuntimeError("Clash Royale offline server failed to start.")
            peer_log_content = (self.session_dir / "peer.log").read_text()
            if '"event": "listening"' in peer_log_content:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Clash Royale offline server did not initialize.")

    def execute_cycle(self, cycle: int) -> str:
        self.stages_completed.append(f"cycle_{cycle}_start")

        container = self.out_dir / 'game/com.supercell.scroll'
        bundle = container / 'Clash Royale.app'

        if not container.exists():
            subprocess.run([str(self.hvm), 'import', str(self.app), str(self.out_dir / 'game')], check=True, stdout=subprocess.DEVNULL)

        cmd = [
            str(self.hvm), "run", str(bundle),
            "--home", str(container),
            "--loopback-network", "--loopback-alias", "game.clashroyaleapp.com",
            "--run-for", str(self.timeout / 10.0), # Run a fraction for each cycle? Actually, just run it for a bit.
            "--graceful",
            "--report", str(self.session_dir / f"report_cycle_{cycle}.json"),
        ]

        self.process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.telemetry = TelemetryAdapter(self.process.pid, 0.5)
        self.telemetry.start()

        # Wait for process or timeout
        try:
            stdout, _ = self.process.communicate(timeout=self.timeout / 5.0)
            self.stages_completed.append(f"cycle_{cycle}_launch")
        except subprocess.TimeoutExpired:
            self.process.terminate()
            stdout, _ = self.process.communicate()

        self.telemetry.stop()

        if "Loaded own home from server" in stdout:
            self.stages_completed.append(f"cycle_{cycle}_home")

        # Check protocol events for NPC request
        events_file = self.session_dir / "protocol-events.json"
        if events_file.exists():
            try:
                events = json.loads(events_file.read_text())['events']
                npc_requests = [e['npc'] for e in events if e['event'] == 'npc_requested']
                if npc_requests:
                    self.stages_completed.append(f"cycle_{cycle}_npc_request")
            except Exception:
                pass

        self.stages_completed.append(f"cycle_{cycle}_battle_start_unsupported")
        self.stages_completed.append(f"cycle_{cycle}_battle_end_unsupported")

        if self.process.returncode != 0:
            return "failed"

        return "unsupported" # Because full battle is unsupported

    def teardown(self):
        super().teardown()
        if self.cr_server:
            self.cr_server.terminate()
            try:
                self.cr_server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.cr_server.kill()
        if self.peer_log:
            self.peer_log.close()


def generate_reports(out_dir: Path, summary: Dict[str, Any], samples: List[Dict[str, Any]]):
    with (out_dir / "summary.json").open("w") as f:
        json.dump(summary, f, indent=2)

    if samples:
        with (out_dir / "samples.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=samples[0].keys())
            writer.writeheader()
            writer.writerows(samples)

    with (out_dir / "summary.md").open("w") as f:
        f.write(f"# Memory Soak Test Summary\n\n")
        f.write(f"**Run ID:** {summary['run_id']}\n")
        f.write(f"**Status:** {summary['status']}\n")
        f.write(f"**App:** {summary['app']}\n")
        f.write(f"**Completed Cycles:** {summary['completed_cycles']} / {summary['requested_cycles']}\n\n")
        f.write(f"## Memory (Host Process)\n")
        if samples:
            peak_rss = max(s['rss_mb'] for s in samples)
            f.write(f"**Peak RSS:** {peak_rss:.2f} MB\n")
        else:
            f.write("No telemetry samples available.\n")

        f.write(f"\n## Stages Reached\n")
        for stage in summary.get('stages', []):
            f.write(f"- {stage}\n")

        f.write(f"\n## Issues\n")
        for issue in summary.get('issues', []):
            f.write(f"- {issue}\n")


def main():
    parser = argparse.ArgumentParser(description="HydrogenVM Memory Soak and Regression Test Runner")
    parser.add_argument("--app", required=True, type=Path, help="Path to the IPA file")
    parser.add_argument("--scenario", type=str, default="clash_royale", help="Scenario to run")
    parser.add_argument("--battles", type=int, default=10, help="Number of battles/cycles to attempt")
    parser.add_argument("--sample-interval", type=float, default=1.0, help="Telemetry sampling interval (s)")
    parser.add_argument("--output", type=Path, default=Path("soak_report"), help="Output directory")
    parser.add_argument("--timeout", type=float, default=300.0, help="Total scenario timeout (s)")
    parser.add_argument("--memory-budget", type=float, help="Memory budget in MB (RSS peak)")
    parser.add_argument("--headless", action="store_true", help="Run in headless mode")
    parser.add_argument("--hvm", type=Path, default=ROOT / "build/hvm", help="Path to hvm binary")
    args = parser.parse_args()

    if not psutil:
        logger.warning("psutil is not installed. Host telemetry will not be collected. (pip install psutil)")

    args.output.mkdir(parents=True, exist_ok=True)
    run_id = f"soak-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:6]}"
    session_out = args.output / run_id
    session_out.mkdir()

    logger.info(f"Starting memory soak test: {run_id}")

    if args.scenario == "clash_royale":
        scenario = ClashRoyaleScenario(args.hvm, args.app, session_out, args.timeout)
    else:
        logger.error(f"Unknown scenario: {args.scenario}")
        return 1

    summary = {
        "run_id": run_id,
        "app": args.app.name,
        "scenario": args.scenario,
        "requested_cycles": args.battles,
        "completed_cycles": 0,
        "status": "INCOMPLETE",
        "stages": [],
        "issues": [],
        "platform": platform.system(),
    }

    all_samples = []

    try:
        scenario.setup()
        for cycle in range(1, args.battles + 1):
            logger.info(f"Starting cycle {cycle}/{args.battles}")
            status = scenario.execute_cycle(cycle)

            if scenario.telemetry:
                samples = scenario.telemetry.get_samples()
                for s in samples:
                    s['cycle'] = cycle
                all_samples.extend(samples)

            summary["stages"].extend(scenario.stages_completed)

            if status == "unsupported":
                logger.info("Cycle reached unsupported stage (e.g. full battle). Counting cycle as incomplete/unsupported but continuing.")
                summary["status"] = "UNSUPPORTED"
                summary["completed_cycles"] += 1
                continue
            elif status == "failed":
                logger.error("Cycle failed.")
                summary["status"] = "FAIL - GUEST"
                break

            summary["completed_cycles"] += 1

        if summary["completed_cycles"] == args.battles:
            if summary["status"] != "UNSUPPORTED":
                summary["status"] = "PASS"

    except Exception as e:
        logger.exception("Scenario execution failed.")
        summary["status"] = "FAIL - HOST"
        summary["issues"].append(str(e))
    finally:
        scenario.teardown()


    # Metadata extraction
    try:
        import plistlib
        with zipfile.ZipFile(args.app) as archive:
            apps = [n for n in archive.namelist() if n.startswith("Payload/") and n.endswith(".app/Info.plist")]
            if apps:
                plist_data = archive.read(apps[0])
                info = plistlib.loads(plist_data)
                summary["bundleId"] = info.get("CFBundleIdentifier")
                summary["appVersion"] = info.get("CFBundleShortVersionString")
                summary["appBuild"] = info.get("CFBundleVersion")
    except Exception:
        pass

    def git_commit():
        try:
            return subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        except:
            return None
    summary["emulatorCommit"] = git_commit()

    if all_samples:
        peak_rss = max(s["rss_mb"] for s in all_samples)
        summary["peak_rss_mb"] = peak_rss

        # Determine baseline growth (from cycle 1 to final cycle)
        c1_samples = [s for s in all_samples if s["cycle"] == 1]
        last_cycle = max(s["cycle"] for s in all_samples)
        cn_samples = [s for s in all_samples if s["cycle"] == last_cycle]

        if c1_samples and cn_samples:
            base_rss = min(s["rss_mb"] for s in c1_samples)
            final_rss = min(s["rss_mb"] for s in cn_samples)
            summary["residual_growth_mb"] = final_rss - base_rss

        if args.memory_budget and peak_rss > args.memory_budget:
            summary["status"] = "FAIL - MEMORY_LIMIT"
            summary["issues"].append(f"Peak RSS ({peak_rss:.2f} MB) exceeded budget ({args.memory_budget:.2f} MB)")

    generate_reports(session_out, summary, all_samples)
    logger.info(f"Report generated at {session_out}")

    if summary["status"] in ["PASS", "UNSUPPORTED"]:
        return 0
    return 1

if __name__ == "__main__":
    sys.exit(main())
