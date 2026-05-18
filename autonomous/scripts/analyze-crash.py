#!/usr/bin/env python3
"""
analyze-crash.py — LLM bridge for Dilithion autonomous fuzzing pipeline.

Takes a novel crash artifact, sends it to DeepSeek for first-pass analysis,
then routes by severity:
  - Critical/High: files GitHub issue + saves to review-queue/ for Opus review
  - Medium/Low: files GitHub issue with DeepSeek analysis only

Usage:  python3 autonomous/scripts/analyze-crash.py
          --crash-file <path> --fuzzer <name>
          --crash-type <type> --severity <sev> --signature <sig>
          [--dry-run]
"""

import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

# ── Configuration ──────────────────────────────────────────────────────────

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
REVIEW_QUEUE = Path(__file__).resolve().parent.parent / "review-queue"

DEEPSEEK_API_KEY = os.environ.get("DEEPSEEK_API_KEY", "")
DEEPSEEK_URL = "https://api.deepseek.com/v1/chat/completions"

GITHUB_TOKEN = os.environ.get("GITHUB_TOKEN", "")
GITHUB_REPO = os.environ.get("GITHUB_REPOSITORY", "dilithion/dilithion")
GITHUB_API = "https://api.github.com"

DRY_RUN = "--dry-run" in sys.argv


# ── Argument Parsing ──────────────────────────────────────────────────────


def parse_args() -> dict:
    args = {}
    i = 1
    while i < len(sys.argv):
        if sys.argv[i].startswith("--") and i + 1 < len(sys.argv):
            key = sys.argv[i][2:].replace("-", "_")
            args[key] = sys.argv[i + 1]
            i += 2
        else:
            i += 1
    return args


# ── Source Extraction ──────────────────────────────────────────────────────


def extract_source_snippet(output: str) -> str:
    """Extract relevant source code around crash sites from stack trace references.
    Uses file:line from trace to pull targeted context, not just the file header."""
    # Find source file references with optional line numbers
    file_pattern = re.compile(r"(src/\S+\.(?:cpp|h))(?::(\d+))?")
    files_seen = set()
    snippets = []

    for m in file_pattern.finditer(output):
        fpath = m.group(1)
        line_str = m.group(2)
        if fpath in files_seen:
            continue
        files_seen.add(fpath)

        full_path = PROJECT_ROOT / fpath
        if not full_path.exists():
            continue

        try:
            all_lines = full_path.read_text().split("\n")
            if line_str:
                # Pull ~20 lines around the crash site
                target = int(line_str)
                start = max(0, target - 22)  # -2 for 0-index, -20 for context
                end = min(len(all_lines), target + 18)
                context = all_lines[start:end]
                snippet = f"// {fpath}:{target} (lines {start + 1}-{end})\n"
                snippet += "\n".join(context)
            else:
                # No line number — grab first 30 non-empty lines
                snippet = f"// {fpath}\n"
                snippet += "\n".join(all_lines[:30])
            snippets.append(snippet[:2000])
        except Exception:
            pass

        if len(snippets) >= 2:
            break

    return "\n\n".join(snippets)[:4000] if snippets else "// Source not available"


# ── DeepSeek API ───────────────────────────────────────────────────────────


def call_deepseek(
    stack_trace: str,
    source_snippet: str,
    fuzzer: str,
    crash_type: str,
    rules_severity: str,
) -> dict | None:
    """Send crash to DeepSeek for first-pass analysis. Returns parsed JSON or None."""

    system_prompt = """You are a C++ security engineer analyzing a fuzz-harness crash in a
cryptocurrency codebase. Your analysis must be precise and conservative.
If you are unsure about something, say so rather than guessing.

Respond ONLY with valid JSON. No markdown, no commentary."""

    user_prompt = f"""Analyze this crash:

Fuzzer: {fuzzer}
Crash type: {crash_type}
Severity (rules-based): {rules_severity}

Stack trace (first 80 lines):
{stack_trace[:3000]}

Relevant source code:
{source_snippet[:2000]}

Respond with this JSON schema:
{{
  "root_cause": "<concise explanation of what caused the crash>",
  "is_exploitable": true|false,
  "exploitability_rationale": "<if exploitable, how; if not, why not>",
  "consensus_impact": true|false,
  "consensus_rationale": "<if consensus-relevant, explain the fork risk>",
  "severity": "critical|high|medium|low",
  "severity_rationale": "<why this severity>",
  "suggested_fix": "<concrete fix suggestion or null if unclear>",
  "affected_components": ["<list of source files>"],
  "issue_title": "[fuzz] <crash_type> in <fuzzer>: <brief>",
  "issue_body": "<full markdown issue body with stack trace, reproduction steps, analysis>",
  "confidence": "high|medium|low"
}}"""

    payload = {
        "model": "deepseek-chat",
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": 0.1,
        "max_tokens": 1500,
        "response_format": {"type": "json_object"},
    }

    if DRY_RUN:
        print("[analyze] DRY-RUN: would call DeepSeek API")
        return {
            "root_cause": "DRY-RUN",
            "severity": rules_severity,
            "confidence": "low",
            "issue_title": f"[fuzz] {crash_type} in {fuzzer}",
            "issue_body": "DRY-RUN",
            "is_exploitable": False,
            "consensus_impact": False,
            "suggested_fix": None,
            "affected_components": [],
            "exploitability_rationale": "",
            "consensus_rationale": "",
            "severity_rationale": "",
        }

    try:
        req = urllib.request.Request(
            DEEPSEEK_URL,
            data=json.dumps(payload).encode(),
            headers={
                "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
                "Content-Type": "application/json",
            },
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = json.loads(resp.read())
            content = body["choices"][0]["message"]["content"]
            return json.loads(content)
    except Exception as e:
        print(f"[analyze] DeepSeek API error: {e}", file=sys.stderr)
        return None


# ── GitHub Issue Creation ─────────────────────────────────────────────────


def create_github_issue(title: str, body: str, labels: list[str]) -> int | None:
    """Create a GitHub issue. Returns issue number or None on failure."""

    if DRY_RUN:
        print(f"[analyze] DRY-RUN: would create issue '{title}' with labels {labels}")
        return 0

    if not GITHUB_TOKEN:
        print("[analyze] No GITHUB_TOKEN set, cannot create issue", file=sys.stderr)
        return None

    url = f"{GITHUB_API}/repos/{GITHUB_REPO}/issues"
    payload = json.dumps({"title": title, "body": body, "labels": labels}).encode()

    try:
        req = urllib.request.Request(
            url,
            data=payload,
            headers={
                "Authorization": f"Bearer {GITHUB_TOKEN}",
                "Content-Type": "application/json",
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read())
            return data["number"]
    except Exception as e:
        print(f"[analyze] GitHub API error: {e}", file=sys.stderr)
        return None


# ── Review Queue ───────────────────────────────────────────────────────────


def save_to_review_queue(
    signature: str, crash_data: dict, deepseek_analysis: dict, issue_number: int
) -> None:
    """Save crash context + DeepSeek analysis to review queue for Opus review."""
    REVIEW_QUEUE.mkdir(parents=True, exist_ok=True)
    queue_file = REVIEW_QUEUE / f"{signature}.json"

    review_data = {
        "signature": signature,
        "queued_at": datetime.now(timezone.utc).isoformat(),
        "github_issue": issue_number,
        **crash_data,
        "deepseek_analysis": deepseek_analysis,
    }

    with open(queue_file, "w") as f:
        json.dump(review_data, f, indent=2)

    print(f"[analyze] Queued for Opus review: {queue_file}")


# ── Main ───────────────────────────────────────────────────────────────────


def main() -> None:
    args = parse_args()

    crash_file = Path(args.get("crash_file", ""))
    trace_file = Path(args.get("trace_file", ""))
    fuzzer = args.get("fuzzer", "unknown")
    crash_type = args.get("crash_type", "unknown")
    rules_severity = args.get("severity", "low")
    signature = args.get("signature", "unknown")

    # Read the reproduced stack trace (text), not the binary crash artifact
    if trace_file.exists():
        output = trace_file.read_text()[:5000]
    elif crash_file.exists():
        # Fallback: crash file might be a text log from older runs
        output = crash_file.read_text(errors="replace")[:5000]
    else:
        print(f"[analyze] ERROR: no trace or crash file found", file=sys.stderr)
        sys.exit(1)

    # Extract source context
    source_snippet = extract_source_snippet(output)

    # DeepSeek first pass
    print(f"[analyze] Calling DeepSeek for {fuzzer}/{crash_file.name} ...")
    analysis = call_deepseek(output, source_snippet, fuzzer, crash_type, rules_severity)

    # Build labels
    labels = ["bug", "fuzzing"]
    components = []

    # Determine final severity and labels from analysis
    if analysis:
        final_severity = analysis.get("severity", rules_severity)
        labels.append(f"severity/{final_severity}")
        title = analysis.get("issue_title", f"[fuzz] {crash_type} in {fuzzer}")
        body = analysis.get(
            "issue_body", f"## Crash in {fuzzer}\n\n```\n{output[:2000]}\n```"
        )
    else:
        # DeepSeek unavailable — raw issue
        labels.append("needs-human-triage")
        labels.append(f"severity/{rules_severity}")
        final_severity = rules_severity
        title = f"[fuzz] {crash_type} in {fuzzer} [raw, no LLM analysis]"
        body = f"## Crash Report (Unanalyzed)\n\n"
        body += f"**Fuzzer:** `{fuzzer}`\n**Crash type:** `{crash_type}`\n"
        body += f"**Severity:** `{rules_severity}` (rules-based)\n\n"
        body += f"### Stack Trace\n```\n{output[:2000]}\n```\n\n"
        body += f"### Reproduction\n```bash\n./{fuzzer} {crash_file.name}\n```\n"

    # Create GitHub issue
    print(f"[analyze] Filing issue: {title}")
    issue_number = create_github_issue(title, body, labels)

    if issue_number:
        print(f"[analyze] Issue #{issue_number} created")
    else:
        # Save issue body locally for retry (uploaded by workflow on failure)
        issue_file = crash_file.with_suffix(crash_file.suffix + ".issue.md")
        issue_file.write_text(f"# {title}\n\n{body}")
        print(f"[analyze] Saved issue to {issue_file} for retry")
        sys.exit(1)  # Non-zero exit triggers workflow's failure artifact upload

    # Route to review queue for Critical/High
    if final_severity in ("critical", "high"):
        crash_data = {
            "fuzzer": fuzzer,
            "crash_type": crash_type,
            "rules_severity": rules_severity,
            "stack_trace": output[:3000],
            "source_snippet": source_snippet[:2000],
        }
        save_to_review_queue(
            signature,
            crash_data,
            analysis or {"error": "DeepSeek unavailable"},
            issue_number or 0,
        )


if __name__ == "__main__":
    main()
