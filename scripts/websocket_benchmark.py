#!/usr/bin/env python3
"""Run Phase 2 and Phase 3 against the same quota-limited loopback tape."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from latency_replay import summarize


def stop(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_case(args: argparse.Namespace, binary: Path, name: str, quota: int) -> dict:
    server_log = args.output / f"{name}-server.log"
    bot_log = args.output / f"{name}-bot.log"
    command = [str(args.server), *map(str, (
        args.rate, args.warmup, args.seconds, quota, args.interval_ms,
    ))]
    with server_log.open("w") as server_stderr, bot_log.open("w") as bot_stderr:
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=server_stderr,
                              text=True) as server:
            bot = None
            try:
                ready = server.stdout.readline().split()
                if len(ready) != 2 or ready[0] != "READY":
                    raise RuntimeError(f"server did not start; see {server_log}")
                env = dict(os.environ, EXCHANGE_URL=f"ws://127.0.0.1:{int(ready[1])}",
                           TEAM_ID="local_benchmark", ARENA_TOKEN="")
                bot = subprocess.Popen([str(binary)], env=env, stdin=subprocess.DEVNULL,
                                       stdout=bot_stderr, stderr=subprocess.STDOUT)
                output, _ = server.communicate(timeout=args.warmup + args.seconds + 25)
                if server.returncode:
                    if output.strip():
                        (args.output / f"{name}-failed.json").write_text(output)
                    raise RuntimeError(f"server failed; see {server_log} and {bot_log}")
                stop(bot)
                if bot.returncode:
                    raise RuntimeError(f"bot exited with status {bot.returncode}; see {bot_log}")
            finally:
                stop(server)
                if bot is not None:
                    stop(bot)

    result = json.loads(output)
    samples = result["latencies_us"]
    pipeline = re.search(r"^\[pipeline\] processed=(\d+) errors=(\d+) dropped_latency=(\d+)",
                         bot_log.read_text(), re.MULTILINE)
    if pipeline:
        processed, errors, dropped = map(int, pipeline.groups())
        # SESSION_OPEN plus the complete tape, ACKs and rejection messages.
        expected = 1 + result["snapshots"] + result["warmup_snapshots"] + result["feedback_sent"]
        result.update(pipeline_processed=processed, pipeline_expected=expected,
                      pipeline_errors=errors, pipeline_dropped_lat=dropped)
    result.update(quota=quota, interval_ms=args.interval_ms, target_rate=args.rate,
                  binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  latency_summary=summarize(samples) if samples else None)
    (args.output / f"{name}.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def format_row(name: str, result: dict) -> str:
    summary = result["latency_summary"]
    percentiles = (" | ".join(f"{summary[key]:,.2f}" for key in ("p50", "p90", "p99", "p999"))
                   if summary else "— | — | — | —")
    return (f"| {name} | {result['quota']} | {result['send_rate']:,.0f} | "
            f"{len(result['latencies_us']):,} | {percentiles} | "
            f"{result['accepted_places']:,} | {result['accepted_cancels']:,} | "
            f"{result['saturated_intervals']:,} | {result['quota_rejections']:,} |")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("server", "phase2", "phase3"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory for logs and results")
    parser.add_argument("--rate", type=int, default=5000, help="offered book snapshots/s")
    parser.add_argument("--warmup", type=int, default=5, help="warmup seconds")
    parser.add_argument("--seconds", type=int, default=30, help="measurement seconds")
    parser.add_argument("--interval-ms", type=int, default=1, help="benchmark quota reset period")
    parser.add_argument("--normal-quota", type=int, default=30)
    parser.add_argument("--tight-quota", type=int, default=6)
    args = parser.parse_args()
    for option in ("server", "phase2", "phase3"):
        path = getattr(args, option).resolve()
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f"--{option} must point to an executable: {path}")
        setattr(args, option, path)
    if not (0 < args.tight_quota < args.normal_quota):
        parser.error("require 0 < tight quota < normal quota")
    if min(args.rate, args.seconds, args.interval_ms) <= 0 or args.warmup < 0:
        parser.error("rate, seconds and interval must be positive, and warmup nonnegative")
    if (args.warmup + args.seconds) * args.rate > 1_000_000:
        parser.error("at most 1,000,000 snapshots per run")
    if args.output.exists():
        parser.error("--output must be a new directory")

    args.output.mkdir(parents=True)
    report = [
        f"Target {args.rate:,} snapshots/s, {args.warmup} s warmup, {args.seconds} s measured, "
        f"{args.interval_ms} ms quota interval.",
        "Latency in µs from loopback snapshot send to the first matching limit order's arrival "
        "at the server. Samples include attempts later rejected by the quota.",
        "",
        "| Run | Quota | Books sent/s | Samples | p50 | p90 | p99 | p99.9 | "
        "Accepted places | Accepted cancels | Saturated intervals | Quota rejections |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    notes = []
    try:
        for label, binary in (("phase2", args.phase2), ("phase3", args.phase3)):
            for setting, quota in (("normal", args.normal_quota), ("tight", args.tight_quota)):
                name = f"{label}-{setting}"
                print(f"Running {name} (quota {quota})", file=sys.stderr, flush=True)
                result = run_case(args, binary, name, quota)
                report.append(format_row(name, result))
                if len(result["latencies_us"]) < 10_000:
                    notes.append(f"- {name} has fewer than 10,000 samples. Interpret p99.9 cautiously.")
                if setting == "tight" and not result["saturated_intervals"]:
                    notes.append(f"- {name} did not reach the quota.")
                if ("pipeline_processed" in result and
                        result["pipeline_processed"] != result["pipeline_expected"]):
                    notes.append(f"- {name} processed {result['pipeline_processed']:,} pipeline events "
                                 f"of {result['pipeline_expected']:,} sent, including feedback.")
                if result["unmatched_limits"] or result["duplicate_limits"]:
                    notes.append(f"- {name} excluded {result['unmatched_limits']:,} unmatched and "
                                 f"{result['duplicate_limits']:,} duplicate limit attempts.")
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"websocket_benchmark: {error}", file=sys.stderr)
        return 1
    report.extend(["", "Per-run JSON includes send scheduling delays and messages received "
                   "during the 500 ms drain window. Books sent/s measures the server's offered load."])
    if notes:
        report.extend(["", *notes])
    summary = "\n".join(report) + "\n"
    (args.output / "summary.md").write_text(summary)
    print(summary, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
