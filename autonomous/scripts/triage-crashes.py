#!/usr/bin/env python3
"""
triage-crashes.py — Crash deduplication and triage for Dilithion fuzzing pipeline.

Scans fuzz_crashes/ for untriaged crash artifacts, generates stable signatures
via stack-trace normalization + SHA256, checks against known crashes in state.json,
and passes novel crashes to analyze-crash.py for LLM analysis.

Usage:  python3 autonomous/scripts/triage-crashes.py [--dry-run]
        --dry-run : don't call analyze-crash.py or file GitHub issues
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# ── Configuration ──────────────────────────────────────────────────────────

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
CRASH_DIR = PROJECT_ROOT / "fuzz_crashes"
STATE_FILE = Path(__file__).resolve().parent.parent / "state" / "state.json"
STATE_BAK = Path(str(STATE_FILE) + ".bak")
ANALYZE_SCRIPT = Path(__file__).resolve().parent / "analyze-crash.py"
DRY_RUN = "--dry-run" in sys.argv

CRASH_CAP_PER_FUZZER = 50

# Severity rules: source path prefix → severity (first match wins)
SEVERITY_RULES = [
    (r"src/consensus/", "critical"),
    (r"src/crypto/", "critical"),
    (r"src/primitives/", "high"),
    (r"src/node/utxo", "high"),
    (r"src/node/mempool", "high"),
    (r"src/wallet/", "high"),
    (r"src/script/", "high"),
    (r"src/node/", "medium"),
    (r"src/net/", "medium"),
    (r"src/rpc/", "medium"),
    (r"src/vdf/", "medium"),
    (r"src/digital_dna/", "medium"),
    (r"src/miner/", "medium"),
    (r"src/util/", "low"),
]

# Crash type → severity boost (overrides path-based if higher)
CRASH_TYPE_BOOST = {
    "heap-buffer-overflow": "critical",
    "stack-buffer-overflow": "critical",
    "heap-use-after-free": "critical",
    "double-free": "critical",
    "SIGSEGV": "high",
    "SIGABRT": "high",
    "SIGILL": "high",
}

SEVERITY_RANK = {"critical": 4, "high": 3, "medium": 2, "low": 1}

# Frames to skip when extracting project-relevant stack frames
SKIP_PATTERNS = [
    r"^LLVMFuzzer",
    r"^fuzzer::",
    r"^__asan::",
    r"^__ubsan::",
    r"^__sanitizer::",
    r"^__libc_",
    r"^std::",
    r"^std::__",
    r"^operator ",
    r"^main$",
    r"^_start",
    r"^__libc_start_main",
]


# ── State Management ───────────────────────────────────────────────────────


def load_state() -> dict:
    """Load crash state database, with automatic backup fallback."""
    for path in [STATE_FILE, STATE_BAK]:
        if path.exists():
            try:
                with open(path) as f:
                    return json.load(f)
            except (json.JSONDecodeError, IOError):
                continue
    # Both broken or missing — start fresh
    print("[triage] WARNING: state file and backup missing/corrupt. Starting fresh.")
    return {
        "version": 1,
        "crashes": {"known": {}},
        "runs": [],
        "settings": {
            "crash_cap_per_fuzzer": CRASH_CAP_PER_FUZZER,
            "signature_hash_bytes": 16,
        },
    }


def save_state(state: dict) -> None:
    """Atomically save crash state with backup."""
    # Backup existing
    if STATE_FILE.exists():
        shutil.copy2(STATE_FILE, STATE_BAK)
    # Write atomically
    tmp = STATE_FILE.with_suffix(".tmp")
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2, sort_keys=True)
    tmp.replace(STATE_FILE)


# ── Crash Reproduction ─────────────────────────────────────────────────────


def reproduce_crash(fuzzer_path: Path, crash_file: Path) -> str:
    """Re-run fuzzer against crash artifact, return first 80 lines of output."""
    try:
        result = subprocess.run(
            [str(fuzzer_path), str(crash_file)],
            capture_output=True,
            text=True,
            timeout=15,
            cwd=str(PROJECT_ROOT),
        )
        output = result.stderr if result.stderr else result.stdout
        lines = output.strip().split("\n")[:80]
        return "\n".join(lines)
    except subprocess.TimeoutExpired:
        return "[triage] ERROR: crash reproduction timed out"
    except Exception as e:
        return f"[triage] ERROR: crash reproduction failed: {e}"


# ── Signature Generation ───────────────────────────────────────────────────


def extract_crash_type(output: str) -> str:
    """Extract crash type from sanitizer output or signal name."""
    # ASan patterns
    m = re.search(r"ERROR:\s*(\S+(?:\s+\S+)*?)(?:\s+in\s|\s*$)", output)
    if m:
        return m.group(1).strip()
    # UBSan patterns
    m = re.search(r"runtime error:\s*(.+?)(?:\n|$)", output)
    if m:
        return m.group(1).strip()
    # Signal patterns
    m = re.search(r"signal\s+(\S+)", output, re.IGNORECASE)
    if m:
        return m.group(1).upper()
    return "unknown"


def extract_project_frames(output: str, max_frames: int = 3) -> list[str]:
    """Extract first N project-relevant stack frames from crash output.
    Returns list of "func@file.cpp" strings for stable hashing."""
    # Match ASan/LLVM frame lines: "#N 0x... in Func(args) path/file.cpp:line"
    frame_pattern = re.compile(
        r"#\d+\s+0x[0-9a-f]+\s+in\s+(\S+).*?(src/\S+\.(?:cpp|h))(?::\d+)?"
    )
    frames = []
    for line in output.split("\n"):
        m = frame_pattern.search(line)
        if not m:
            continue
        func = m.group(1)
        filepath = m.group(2)
        # Skip non-project frames
        if any(re.search(p, func) for p in SKIP_PATTERNS):
            continue
        frames.append(f"{func}@{filepath}")
        if len(frames) >= max_frames:
            break
    return frames


def normalize_frames(frames: list[str]) -> str:
    """Normalize frame list: strip addresses, line numbers, collapse whitespace."""
    normalized = []
    for f in frames:
        # Strip addresses
        f = re.sub(r"0x[0-9a-fA-F]+", "0x0", f)
        # Strip line numbers in file paths
        f = re.sub(r":\d+", ":0", f)
        # Collapse whitespace
        f = re.sub(r"\s+", " ", f).strip()
        normalized.append(f)
    return "\n".join(normalized)


def generate_signature(frames: list[str], crash_type: str, hash_bytes: int = 16) -> str:
    """Generate stable SHA256 signature from normalized crash data.
    Does NOT include fuzzer_name — same bug in two fuzzers = same signature."""
    normalized = normalize_frames(frames)
    raw = f"{normalized}\n{crash_type}"
    full_hash = hashlib.sha256(raw.encode()).hexdigest()
    return full_hash[: hash_bytes * 2]


# ── Severity Classification ────────────────────────────────────────────────


def classify_severity(crash_type: str, frames: list[str], output: str) -> str:
    """Rule-based severity classification from crash type and affected paths."""
    severity = "low"

    # Check crash type boost
    for pattern, sev in CRASH_TYPE_BOOST.items():
        if pattern.lower() in crash_type.lower():
            if SEVERITY_RANK.get(sev, 0) > SEVERITY_RANK.get(severity, 0):
                severity = sev
            break

    # Check source paths in output for component hints
    for pattern, sev in SEVERITY_RULES:
        if re.search(pattern, output):
            if SEVERITY_RANK.get(sev, 0) > SEVERITY_RANK.get(severity, 0):
                severity = sev
            break  # First match wins

    return severity


def classify_components(output: str) -> list[str]:
    """Identify affected components from source paths in stack trace."""
    component_map = {
        "consensus": r"src/consensus/",
        "crypto": r"src/crypto/",
        "utxo": r"src/node/utxo_set",
        "mempool": r"src/node/mempool",
        "net": r"src/net/",
        "wallet": r"src/wallet/",
        "rpc": r"src/rpc/",
        "vdf": r"src/vdf/",
        "dna": r"src/digital_dna/",
    }
    components = []
    for name, pattern in component_map.items():
        if re.search(pattern, output):
            components.append(f"component/{name}")
    return components if components else ["component/unknown"]


# ── Main Triage Loop ──────────────────────────────────────────────────────


def main() -> None:
    state = load_state()
    settings = state.get("settings", {})
    crash_cap = settings.get("crash_cap_per_fuzzer", CRASH_CAP_PER_FUZZER)
    hash_bytes = settings.get("signature_hash_bytes", 16)

    # Scan for untriaged crashes
    crash_patterns = ["crash-*", "timeout-*", "oom-*", "leak-*"]
    untriaged = []

    if CRASH_DIR.exists():
        for fuzzer_dir in sorted(CRASH_DIR.iterdir()):
            if not fuzzer_dir.is_dir():
                continue
            fuzzer_crashes = []
            for pattern in crash_patterns:
                for crash_file in fuzzer_dir.glob(pattern):
                    if not crash_file.with_suffix(
                        crash_file.suffix + ".triaged"
                    ).exists():
                        fuzzer_crashes.append(crash_file)

            # Enforce per-fuzzer cap
            if len(fuzzer_crashes) > crash_cap:
                print(
                    f"[triage] WARNING: {fuzzer_dir.name} has {len(fuzzer_crashes)} crashes, "
                    f"capping at {crash_cap}. Investigate root cause."
                )
                fuzzer_crashes = fuzzer_crashes[:crash_cap]

            untriaged.extend(fuzzer_crashes)

    if not untriaged:
        print("[triage] No untriaged crashes found.")
        return

    print(f"[triage] Found {len(untriaged)} untriaged crash(es).")

    total_triaged = 0
    new_crashes = 0
    known = state["crashes"]["known"]

    for crash_file in untriaged:
        fuzzer_name = crash_file.parent.name
        fuzzer_path = PROJECT_ROOT / fuzzer_name

        if not fuzzer_path.exists():
            print(f"[triage] SKIP: fuzzer binary '{fuzzer_name}' not found")
            continue

        print(f"[triage] Processing: {fuzzer_name}/{crash_file.name} ...")

        # Reproduce
        output = reproduce_crash(fuzzer_path, crash_file)
        crash_type = extract_crash_type(output)
        frames = extract_project_frames(output)
        severity = classify_severity(crash_type, frames, output)
        signature = generate_signature(frames, crash_type, hash_bytes)

        print(f"  type={crash_type} severity={severity} sig={signature}")

        if not frames:
            print(f"  WARNING: no project frames extracted, using raw signature")
            signature = hashlib.sha256(output.encode()).hexdigest()[: hash_bytes * 2]

        # Check known crashes
        if signature in known:
            known[signature]["last_seen"] = datetime.now(timezone.utc).isoformat()
            if fuzzer_name not in known[signature].get("seen_by_fuzzers", []):
                known[signature].setdefault("seen_by_fuzzers", []).append(fuzzer_name)
            print(
                f"  → Known crash (issue #{known[signature].get('github_issue', 'N/A')})"
            )
        else:
            # Novel crash
            print(f"  → Novel crash! Routing for analysis...")
            new_crashes += 1

            if DRY_RUN:
                print(f"  [DRY-RUN] Would call analyze-crash.py with {crash_file}")
            else:
                # Write reproduced trace to a file for the analyzer
                trace_file = crash_file.with_suffix(crash_file.suffix + ".trace")
                trace_file.write_text(output[:5000])

                # Call analyze-crash.py with the trace (not the binary artifact)
                result = subprocess.run(
                    [
                        sys.executable,
                        str(ANALYZE_SCRIPT),
                        "--trace-file",
                        str(trace_file),
                        "--crash-file",
                        str(crash_file),
                        "--fuzzer",
                        fuzzer_name,
                        "--crash-type",
                        crash_type,
                        "--severity",
                        severity,
                        "--signature",
                        signature,
                    ],
                    capture_output=True,
                    text=True,
                    timeout=120,
                    cwd=str(PROJECT_ROOT),
                )
                if result.returncode == 0:
                    print(f"  → Analysis complete: {result.stdout.strip()}")
                else:
                    print(f"  → Analysis FAILED: {result.stderr.strip()[:200]}")

            # Record in state
            known[signature] = {
                "seen_by_fuzzers": [fuzzer_name],
                "crash_type": crash_type,
                "severity": severity,
                "first_seen": datetime.now(timezone.utc).isoformat(),
                "last_seen": datetime.now(timezone.utc).isoformat(),
                "github_issue": None,
                "status": "open",
                "signature_frames": frames,
                "components": classify_components(output),
            }

        # Mark as triaged
        marker = crash_file.with_suffix(crash_file.suffix + ".triaged")
        marker.touch()
        total_triaged += 1

    # Record run
    state["runs"].append(
        {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "trigger": "auto",
            "mode": "triage",
            "total_crashes": len(untriaged),
            "triaged": total_triaged,
            "new_crashes": new_crashes,
            "build_success": True,
        }
    )

    save_state(state)
    print(
        f"\n[triage] Done: {total_triaged} triaged, {new_crashes} novel, {total_triaged - new_crashes} known."
    )


if __name__ == "__main__":
    main()
