# Dilithion Autonomous Pipeline — Design Document

**Version:** 1.0.0
**Status:** Implemented — in review
**Location:** `autonomous/` (self-contained sub-project)

---

## Table of Contents

1. [Purpose & Philosophy](#1-purpose--philosophy)
2. [Scope & Non-Goals](#2-scope--non-goals)
3. [Design Principles](#3-design-principles)
4. [Threat Model](#4-threat-model)
5. [Agent Taxonomy](#5-agent-taxonomy)
6. [System Architecture](#6-system-architecture)
7. [Data Flow](#7-data-flow)
8. [State Model](#8-state-model)
9. [LLM Integration Strategy](#9-llm-integration-strategy)
10. [Token Budget & Cost Model](#10-token-budget--cost-model)
11. [Failure Modes & Resilience](#11-failure-modes--resilience)
12. [Security Boundaries](#12-security-boundaries)
13. [Implementation Phases](#13-implementation-phases)
14. [Success Metrics](#14-success-metrics)
15. [Open Questions](#15-open-questions)

---

## 1. Purpose & Philosophy

### Why This Exists

Dilithion is a mainnet-live cryptocurrency. Bugs in this codebase can cause:

- **Consensus forks** — nodes disagreeing on chain state, potentially splitting the network
- **Double-spends** — UTXO validation failures allowing coins to be spent twice
- **Fund loss** — wallet or transaction bugs destroying or stealing user funds
- **Network denial** — crash bugs that can be triggered by remote peers

The existing CI pipeline catches many issues, but it has a fundamental limitation:
**it only runs when a human pushes code**. Between pushes — hours, days, or weeks —
bugs can sit undetected.

### What This Pipeline Changes

The autonomous pipeline shifts from **reactive** (CI on push) to **proactive** (continuous
background execution). It runs on a clock, not on a git event. It finds crashes, analyzes
them, and files actionable issues — without a human in the loop.

### Core Philosophy

> **Deterministic where possible, LLM-assisted where necessary, human-reviewed when it matters.**

We do not use LLMs as a replacement for deterministic tooling. We use them only at the
point where mechanical analysis ends and semantic understanding begins: interpreting
*why* a crash happened, assessing its severity, and communicating it clearly to humans.

---

## 2. Scope & Non-Goals

### In Scope

| Area | Description |
|------|-------------|
| **Continuous fuzzing** | Run all 23 libFuzzer harnesses on a schedule (every 6 hours) with tiered time allocation |
| **Crash deduplication** | Hash-normalize stack traces into stable signatures, eliminate duplicates across runs |
| **Root-cause analysis** | LLM-powered interpretation of novel crashes (affected code, exploitability, fix suggestion) |
| **Issue filing** | Auto-create GitHub issues with structured, actionable content |
| **State persistence** | Maintain a crash signature database and run history for trend analysis |
| **Manual dispatch** | Allow triggering a marathon or triage run on demand via GitHub Actions UI |

### Out of Scope (Current Phase)

| Area | Why Not Now |
|------|-------------|
| **Auto-patching** | Fixes must be human-reviewed before merge; full auto-patch is unsafe for consensus code |
| **Corpus evolution** | Persisting and mutating the fuzz corpus between runs adds complexity without proportional gain for a 23-harness suite |
| **Coverage-driven test generation** | Requires deeper integration with coverage data; Phase 2 |
| **Performance regression detection** | Requires benchmark infrastructure; Phase 2 |
| **Cross-platform fuzzing** | Linux-only for now; Windows/macOS runners are Phase 3 |
| **Federation / distributed fuzzing** | Single-runner is sufficient at current harness count |

---

## 3. Project Principles & Design Principles

### 3.1 Dilithion's Engineering Principles

This pipeline is governed by the same principles as the Dilithion codebase itself:

| Principle | Source | Applied Here As |
|-----------|--------|-----------------|
| **KISS** (Keep It Simple, Stupid) | `HANDOFF_OVERNIGHT.md` | Shell + Python, not a framework. One directory, not a distributed system. |
| **SSOT** (Single Source of Truth) | `docs/analysis/SINGLE-SOURCE-OF-TRUTH-OPPORTUNITIES.md` | `state.json` is canonical. `.triaged` marker files are a cache only. If they disagree, `state.json` wins. |
| **World-Class** | `HANDOFF_OVERNIGHT.md` | Benchmark against Bitcoin Core's OSS-Fuzz setup. Document deliberate divergences. |
| **Technical Excellence** | `CONTRIBUTING.md` | Correctness and security over speed. Every crash is reproducible before it becomes an issue. |
| **No Shortcuts** | Bug reports | Complete one task, A++ quality. Every failure mode in the matrix is handled before Phase 1 ships. |

### 3.2 Pipeline-Specific Design Principles

### P1 — Zero New Runtime Dependencies in the Node

The autonomous pipeline is **external tooling**. It does not link into `dilithion-node`,
does not add libraries to the build, and does not change the C++ codebase. It is
shell scripts, Python, and API calls. The node binary remains unchanged.

### P2 — Deterministic-First

Every task that can be done deterministically **must** be done deterministically.
The LLM is reserved for semantic interpretation of crash data that cannot be
algorithmically derived.

| Task | Method | Reason |
|------|--------|--------|
| Build fuzzers | `make fuzz` | Deterministic |
| Run fuzz campaigns | `timeout` + libFuzzer flags | Deterministic |
| Collect crash artifacts | `find` + `cp` | Deterministic |
| Reproduce crash | Re-run fuzzer against artifact | Deterministic |
| Extract stack frames | Regex parse | Deterministic |
| Normalize + hash | SHA256 | Deterministic |
| Deduplicate | Dictionary lookup | Deterministic |
| Classify severity | Rule-based (source path + crash type) | Deterministic |
| Root-cause analysis | DeepSeek API | LLM required |
| Review + polish | Claude Opus API | LLM required |
| File GitHub issue | REST API | Deterministic |

### P3 — Fail Safe

A pipeline failure must never block development, require immediate human action, or
corrupt data. Every component degrades gracefully:

- Build failure → log, exit cleanly, no issue filed
- API unavailability → save raw analysis, retry next run
- State corruption → auto-restore from backup, recreate if both broken
- Crash flood (>50 from one fuzzer) → cap and flag, don't DOS the issue tracker

### P4 — Observable

Every action produces structured output. Every decision point is logged. The state
file provides a complete audit trail of every crash ever encountered and every run
ever executed.

### P5 — Reproducible

Every crash artifact is preserved with enough context that a human can reproduce it
with a single command: `./fuzz_foo path/to/crash-file`. No findings are reported
that cannot be independently verified.

### P6 — Token-Efficient

LLM calls follow a two-tier model:
1. **DeepSeek** — first pass on every novel crash (cheap, fast)
2. **Claude Opus** — reviews DeepSeek's output (expensive, thorough)

Both calls together cost ~$0.04 per novel crash. At realistic volumes (0-3/day),
annual LLM costs are under $50.

### P7 — Self-Contained

Everything lives in `autonomous/`. No files scattered across the project. One
directory to understand, one directory to remove if we ever change direction.

### P8 — SSOT for Crash State

The `autonomous/state/state.json` file is the single source of truth for all
crash tracking. The `.triaged` marker files on individual crash artifacts are
a filesystem cache — they exist only to speed up the "is this crash new?" check
without loading the entire state file. If a `.triaged` file and `state.json`
disagree, `state.json` is authoritative and the marker is regenerated.

---

## 4. Threat Model

### What We're Defending Against

| Threat | Detection Method |
|--------|-----------------|
| **Memory safety bugs** (buffer overflow, UAF, double-free) | ASan-instrumented fuzzers |
| **Undefined behavior** (null deref, integer overflow, shift overflow) | UBSan-instrumented fuzzers |
| **Input validation bypasses** | Fuzzers feeding malformed blocks, txs, network messages |
| **Consensus divergence** | Fuzzers comparing validation results across code paths |
| **Cryptographic edge cases** | Signature fuzzer, SHA-3 fuzzer |
| **Regression bugs** | New crashes that appear after previously-clean fuzzer runs |

### What We're NOT Defending Against

| Threat | Why Out of Scope |
|--------|-----------------|
| **Supply chain attacks** on dependencies | Requires dependency hashing + reproducible builds — separate concern |
| **Deliberate backdoors** in source | Fuzzing won't find intentionally hidden logic paths |
| **Network-level attacks** (eclipse, partition) | Requires live network testing, not fuzzing |
| **Cryptographic breaks** (broken primitive) | Fuzzing tests implementation, not algorithm strength |
| **Side-channel attacks** (timing, power) | Requires specialized testing harnesses |

### Trust Boundaries

```
┌─────────────────────────────────────────────────────┐
│  TRUSTED                                             │
│  - dilithion-node source code                        │
│  - libFuzzer + sanitizers (well-audited tooling)     │
│  - state.json (local file, no secrets)               │
│  - GitHub Actions runner (ephemeral, isolated)        │
├─────────────────────────────────────────────────────┤
│  SEMI-TRUSTED                                        │
│  - DeepSeek API (receives stack traces + source      │
│    snippets, NOT keys, addresses, or node data)      │
│  - Anthropic API (same data scope)                   │
│  - GitHub API (files issues in public repo)          │
├─────────────────────────────────────────────────────┤
│  UNTRUSTED                                           │
│  - Fuzz inputs (adversarial by design)               │
│  - Crash artifacts (potentially attacker-controlled  │
│    if fuzzer corpus is ever externally sourced)      │
└─────────────────────────────────────────────────────┘
```

### Data Sent to LLM APIs

Only the following leaves the runner:

1. **Stack trace** — function names, file paths, line numbers (from the Dilithion codebase)
2. **Crash type** — e.g., "heap-buffer-overflow", "SIGSEGV"
3. **Fuzzer name** — e.g., "fuzz_transaction"
4. **Source code snippet** — the affected function(s) from the Dilithion source (already public, MIT-licensed)

The following **never** leaves the runner:

- Private keys, wallet data, addresses
- Node configuration, peer lists
- Mainnet/testnet chain data
- Any file not tracked in the public git repository

---

## 5. Agent Taxonomy

The term "agent" here refers to a pipeline stage — not an LLM-driven autonomous entity.
Most agents are deterministic scripts. Only two stages involve an LLM.

### Agent 1: Fuzz Marathon Runner

| Property | Value |
|----------|-------|
| **Type** | Deterministic |
| **Implementation** | `autonomous/scripts/fuzz-loop.sh` (Bash) |
| **Trigger** | Cron (every 6h) or manual dispatch |
| **Runtime** | ~3.5 hours (marathon mode), ~5 min (quick mode) |
| **LLM Tokens** | 0 |
| **Input** | Dilithion source tree |
| **Output** | Crash artifacts in `fuzz_crashes/`, structured report |
| **Exit Codes** | 0 = clean, 1 = crashes found, 2 = build failure |

**Responsibilities:**
- Build all 23 fuzzers via `make fuzz`
- Run each fuzzer with tiered time allocation (30min/15min/10min)
- Collect crash/timeout/OOM/leak artifacts into per-fuzzer directories
- Produce a markdown report summarizing the marathon
- Signal whether crashes were found (exit code)

**Not responsible for:**
- Analyzing crashes (→ Agent 2)
- Filing issues (→ Agent 3)
- Maintaining state (→ Agent 2 does this)

### Agent 2: Crash Triage Engine

| Property | Value |
|----------|-------|
| **Type** | Deterministic |
| **Implementation** | `autonomous/scripts/triage-crashes.py` (Python) |
| **Trigger** | After fuzz-loop.sh exits with code 1, or manual dispatch |
| **Runtime** | ~5 seconds per crash |
| **LLM Tokens** | 0 |
| **Input** | Crash artifacts + `autonomous/state/state.json` |
| **Output** | Updated state.json, calls to Agent 3 for novel crashes |

**Responsibilities:**
- Scan `fuzz_crashes/` for untriaged crash files (no `.triaged` marker)
- Reproduce each crash to capture a clean stack trace
- Normalize: strip addresses, line numbers → stable function-name-only stack
- Generate SHA256 signature from normalized stack + crash type + fuzzer name
- Look up signature in `state.json` known crashes
- Known crash → update `last_seen`, mark `.triaged`
- Novel crash → call Agent 3 (analyze-crash.py), then update state
- Enforce crash cap (50 per fuzzer per run)
- Record run metadata in state.json

**Not responsible for:**
- LLM analysis (→ Agent 3)
- GitHub API calls (→ Agent 3)
- Filing issues for known crashes

### Agent 3: Crash Analyzer (LLM Bridge)

| Property | Value |
|----------|-------|
| **Type** | LLM-assisted (DeepSeek API) + human-reviewed (Claude Code subscription) |
| **Implementation** | `autonomous/scripts/analyze-crash.py` (Python) |
| **Trigger** | Called by Agent 2 when a novel crash signature is found |
| **Runtime** | ~5-10 seconds per crash (DeepSeek API latency) |
| **LLM Tokens** | ~1,000 per crash (DeepSeek only for autonomous path) |
| **Input** | Crash artifact path, fuzzer name, stack trace, affected source snippet |
| **Output** | GitHub issue (via REST API) or saved analysis file on API failure |

**Responsibilities:**
- Read the crash stack trace and extract affected source code
- **DeepSeek pass:** Send structured prompt → receive root cause, severity, exploitability, suggested fix, draft issue
- **Route by severity:**
  - **Critical / High:** Save analysis to `autonomous/review-queue/<signature>.json` for human review via Claude Code subscription (no incremental API cost). File issue immediately with DeepSeek analysis + `needs-opus-review` label. The review queue can be batch-processed when convenient.
  - **Medium / Low:** File issue directly with DeepSeek analysis (Opus review is nice-to-have, not load-bearing for these severities)
- On API failure: save analysis to `.analysis.md` next to crash artifact for retry

**Not responsible for:**
- Crash reproduction (→ Agent 2)
- Deduplication (→ Agent 2)
- State management (→ Agent 2)
- Opus review (this is a manual/human step using Claude Code subscription, see Section 9)

### Agent 4: Regression Watchdog (Phase 2)

| Property | Value |
|----------|-------|
| **Type** | Deterministic |
| **Implementation** | TBD |
| **Trigger** | After fuzz-loop.sh completes |
| **Runtime** | ~1 minute |
| **LLM Tokens** | 0 |
| **Input** | Current marathon report + historical reports |
| **Output** | Alert if metrics regress (executions drop, new crashes spike, build time increases) |

### Agent 5: Coverage Hunter (Phase 2)

| Property | Value |
|----------|-------|
| **Type** | Deterministic |
| **Implementation** | TBD |
| **Trigger** | Daily, or on PR merge |
| **Runtime** | ~2 minutes |
| **LLM Tokens** | 0 |
| **Input** | Codecov API data |
| **Output** | Flag files below coverage threshold, especially in `src/consensus/` and `src/crypto/` |

---

## 6. System Architecture

### Directory Structure

```
autonomous/
├── DESIGN.md                  ← This document
├── AGENTS.md                  ← Human-readable pipeline manifest
├── scripts/
│   ├── fuzz-loop.sh           ← Agent 1: Marathon runner
│   ├── triage-crashes.py      ← Agent 2: Crash dedup + triage
│   └── analyze-crash.py       ← Agent 3: LLM analysis bridge
├── state/
│   ├── state.json             ← Crash signature database + run history
│   └── state.json.bak         ← Auto-backup (created on each write)
└── .github/                   ← (actually in project root .github/)
    └── workflows/
        └── autonomous-pipeline.yml  ← Cron + manual dispatch
```

### Execution Environment

| Property | Value |
|----------|-------|
| **Primary runtime** | GitHub Actions (`ubuntu-24.04` runner) |
| **Local testing** | Any Linux machine with `clang++-14`, `make`, `python3` |
| **State persistence** | `actions/cache` (keyed on branch+run_id, prefix-fallback to main, 7-day retention). No git push. Repo copy is the seed only. |
| **Scheduler** | GitHub Actions cron (`0 */6 * * *`) |
| **Manual trigger** | `workflow_dispatch` with optional `--quick` flag |
| **Timeout** | 6 hours per workflow run (covers marathon + triage + analysis) |

### Dependency Map

```
fuzz-loop.sh
  ├── make (GNU Make)
  ├── clang++-14 (with libFuzzer)
  ├── timeout (coreutils)
  └── Dilithion source tree

triage-crashes.py
  ├── python3 (stdlib only: json, hashlib, subprocess, pathlib, re)
  └── state.json

analyze-crash.py
  ├── python3
  ├── requests (or urllib from stdlib)
  ├── DEEPSEEK_API_KEY (env var / GitHub Secret)
  ├── DEEPSEEK_API_KEY (env var / GitHub Secret)  # only external API key needed
  └── GITHUB_TOKEN (env var / GitHub Actions built-in)
```

---

## 7. Data Flow

### Normal Flow (No Crashes)

```
cron fires (every 6h)
        │
        ▼
fuzz-loop.sh
  ├─ make fuzz                          → builds 23 fuzzers
  ├─ for each fuzzer: run N minutes     → 0 crashes
  └─ produce report                     → exit 0
        │
        ▼
Workflow complete. Nothing to triage.
Run record appended to state.json.
```

### Crash Flow (Novel Crash Found)

```
cron fires
        │
        ▼
fuzz-loop.sh
  ├─ make fuzz
  ├─ fuzz_transaction (30 min)          → 1 crash artifact
  ├─ ... remaining fuzzers ...
  └─ produce report                     → exit 1
        │
        ▼
triage-crashes.py
  ├─ scan fuzz_crashes/                 → 1 untriaged crash
  ├─ reproduce: ./fuzz_transaction <crash>
  ├─ extract stack, normalize, hash     → "a1b2c3d4e5f6a7b8"
  ├─ lookup state.json                  → NOT FOUND (novel)
  ├─ rules classify severity            → "high"
  │
  ├─ call analyze-crash.py ─────────────┐
  │                                     │
  └─ mark .triaged, update state.json   │
        │                               │
        ▼                               ▼
Workflow complete.              analyze-crash.py
                                  ├─ read stack trace + source snippet
                                  ├─ DeepSeek API ──→ root cause analysis
                                  ├─ Claude Opus API ──→ review + polish
                                  └─ GitHub API ──→ file issue #342
```

### Known Crash Flow (Duplicate)

```
triage-crashes.py
  ├─ scan fuzz_crashes/                 → 1 untriaged crash
  ├─ reproduce, normalize, hash         → "a1b2c3d4e5f6a7b8"
  ├─ lookup state.json                  → FOUND (issue #342, status: open)
  ├─ update last_seen timestamp
  └─ mark .triaged
        │
        ▼
No LLM calls. No issue filed.
```

### Build Failure Flow

```
fuzz-loop.sh
  ├─ make fuzz                          → FAIL
  └─ produce report                     → exit 2
        │
        ▼
Workflow completes with failure status.
No triage attempted.
Build failure report saved for inspection.
```

---

## 8. State Model

### Schema

```json
{
  "version": 1,
  "crashes": {
    "known": {
      "<sha256_hex[:16]>": {
        "seen_by_fuzzers": ["fuzz_transaction", "fuzz_block"],
        "crash_type": "heap-buffer-overflow",
        "severity": "critical",
        "first_seen": "2026-01-15T08:22:00Z",
        "last_seen": "2026-01-20T14:30:00Z",
        "github_issue": 342,
        "status": "open",
        "signature_frames": [
          "CTxOut::Deserialize",
          "CTransaction::Deserialize",
          "fuzz_target_transaction_deserialize"
        ],
        "deepseek_analysis": "Buffer overflow in CTxOut::Deserialize when parsing...",
        "opus_review": "Confirmed. Additionally, the same pattern exists in..."
      }
    }
  },
  "runs": [
    {
      "timestamp": "2026-01-20T14:00:00Z",
      "trigger": "cron",
      "mode": "marathon",
      "total_crashes": 3,
      "triaged": 3,
      "new_crashes": 1,
      "build_success": true,
      "report": "fuzz_crashes/marathon-report-2026-01-20T140000.md"
    }
  ],
  "settings": {
    "crash_cap_per_fuzzer": 50,
    "signature_hash_bytes": 16
  }
}
```

### Signature Algorithm

```
Input:  crash_file, fuzzer_name

1. Reproduce:  ./<fuzzer_name> <crash_file> 2>&1 | head -80
2. Parse:      extract stack frames (function names only)
3. Filter:     skip libFuzzer internals, sanitizer frames, stdlib
               keep first 3 project frames (under src/)
4. Normalize:  strip addresses (0x... → 0x0)
               strip line numbers (:\d+ → :0)
               collapse whitespace
5. Identify:   crash type from sanitizer header or signal name
6. Hash:       SHA256(normalized_frames + "\n" + crash_type + "\n" + fuzzer_name)
               truncate to configured bytes (default: 16 → 128 bits)
```

96 bits of entropy from the truncated hash gives ~10^14 signatures before a
collision is expected — far beyond our scale.

### Backup Strategy

- On every write to `state.json`, first write to `state.json.tmp`, then `os.replace()` (atomic on most filesystems)
- Before write, copy existing `state.json` to `state.json.bak`
- On load failure, try `state.json.bak`; if both broken, start fresh and log warning

---

## 9. LLM Integration Strategy

### Why DeepSeek + Claude Subscription (Not API)

| Concern | DeepSeek API | Claude Opus (via subscription) |
|---------|-------------|-------------------------------|
| **Cost** | ~$0.27/M input | $0 (flat subscription, already paid) |
| **Speed** | Fast, programmatic | Manual, batched |
| **C++ analysis** | Good | Excellent |
| **Security reasoning** | Adequate | Strong |
| **Role** | Autonomous first pass on every novel crash | Human-initiated review of Critical/High crashes |
| **Integration** | REST API call from analyze-crash.py | Claude Code reading `autonomous/review-queue/` files |

**Rationale:** The Anthropic API charges per token at a premium rate. The Claude
subscription (Claude Code / claude.ai) is a flat monthly fee. By routing Opus
review through the subscription instead of the API, we eliminate the ~$0.04/crash
Opus cost entirely. The tradeoff is that Opus review becomes asynchronous — it
happens when a human points Claude Code at the review queue, not in real-time.
This is acceptable because:

1. DeepSeek's first-pass analysis is sufficient to file an actionable issue
2. Critical/High crashes are rare (realistic: 0-2 per week)
3. The review queue batches work efficiently — review 5 crashes in one session

### Autonomous Path (DeepSeek Only)

For every novel crash, regardless of severity:

```
analyze-crash.py
  ├─ Read stack trace + extract source snippet
  ├─ DeepSeek API ──→ root cause, severity, exploitability, draft issue
  ├─ File GitHub issue with DeepSeek analysis
  └─ If Critical/High: also save to review-queue/ for later Opus review
```

The issue is filed immediately. The Opus review, when it happens, updates the
issue with corrections or additional findings.

### Human Review Path (Claude Subscription)

For Critical/High crashes, the pipeline saves a review file:

```
autonomous/review-queue/
├── a1b2c3d4e5f6a7b8.json   # Crash #1 - Critical
├── f9e8d7c6b5a4f3e2.json   # Crash #2 - High
└── README.md                # Instructions for Claude Code review
```

Each JSON file contains the full crash context + DeepSeek's analysis. When you
have time, you run:

> "Claude, review the crash analyses in autonomous/review-queue/. For each one,
tell me if DeepSeek's analysis is accurate, what it missed, and whether the
severity classification is correct. Update the corresponding GitHub issues."

Claude Code (using your subscription) reads the queue, reviews each crash, and
can update the GitHub issues via the `gh` CLI — all within your subscription,
no API charges.

### DeepSeek Prompt Template

```
System:
You are a C++ security engineer analyzing a fuzz-harness crash in a
cryptocurrency codebase. Your analysis must be precise and conservative.
If you are unsure about something, say so rather than guessing.

User:
Analyze this crash:

Fuzzer: {fuzzer_name}
Crash type: {crash_type}
Severity (rules-based): {rules_severity}

Stack trace (first 80 lines):
{stack_trace}

Relevant source code:
{source_snippet}

Respond in JSON:
{
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
  "issue_body": "<full markdown issue body>",
  "confidence": "high|medium|low"
}
```

### Claude Opus Review Prompt (for review-queue)

The `review-queue/README.md` contains the prompt template for Claude Code:

```markdown
# Crash Review Queue

You are reviewing DeepSeek's analysis of fuzz crashes in the Dilithion
cryptocurrency codebase. For each JSON file in this directory:

1. Read the crash context and DeepSeek's analysis
2. Verify: Is the root cause correctly identified?
3. Check: Is the severity classification appropriate?
4. Assess: Was anything important missed (consensus impact, exploitability)?
5. If corrections are needed, update the GitHub issue (use `gh issue edit`)
6. Add a comment to the issue with your review notes
7. Move the reviewed JSON to `review-queue/reviewed/`

Focus on:
- Consensus impact (could this cause a chain split?)
- Memory safety (is this exploitable, and how?)
- Fix correctness (will the suggested fix work?)
```

### API Failure Handling

| Failure | Behavior |
|---------|----------|
| DeepSeek timeout/error | Skip LLM analysis entirely; file issue with raw stack trace, label `needs-human-triage` |
| GitHub API down | Save issue body to `.issue.md` next to artifact; retry on next run |

---

## 10. Token Budget & Cost Model

### Per-Crash Token Estimates

| Stage | Model | Input Tokens | Output Tokens | Est. Cost |
|-------|-------|-------------|---------------|-----------|
| First pass | DeepSeek V3 | ~800 | ~200 | ~$0.0004 |
| Review (Critical/High only) | Claude Opus (subscription) | ~1,000 | ~300 | $0 (flat subscription) |
| **Total per novel crash** | | **~1,000-2,000** | **~200-500** | **~$0.0004 (autonomous) + $0 (review)** |

### Annual Cost Projections

| Scenario | Novel Crashes/Day | DeepSeek Cost | Opus Cost | Total |
|----------|-------------------|---------------|-----------|-------|
| Normal operation | 0-1 | $0 — $0.15 | $0 | $0 — $0.15 |
| Active development | 1-3 | $0.15 — $0.44 | $0 | $0.15 — $0.44 |
| After major refactor | 5-10 (temporary) | $0.73 — $1.46 | $0 | $0.73 — $1.46 |
| Worst case month (10/day) | Burst | ~$0.12/month | $0 | ~$0.12 |

**Key insight:** By routing Opus review through the Claude subscription instead
of the API, the per-crash cost drops from ~$0.04 to ~$0.0004. Annual costs are
under $2 even in the worst case.

### Token Optimization Measures

1. **Deduplication before LLM** — only novel crashes reach the API
2. **Truncated stack traces** — first 80 lines only, not full output
3. **Source snippets, not full files** — extract only the affected function(s)
4. **Structured JSON output** — predictable format, no conversational padding
5. **No conversational history** — each crash is a stateless API call

### Budget Controls

- **Per-run cap:** Max 10 novel crashes analyzed per pipeline run (prevents cost explosion if a new bug causes many variants)
- **Monthly budget:** Soft cap at $25 (alerts if exceeded; doesn't block)
- **Cost tracking:** Each run logs estimated token usage to state.json

---

## 11. Failure Modes & Resilience

### Failure Matrix

| Failure | Detection | Automatic Response | Human Action Needed |
|---------|-----------|-------------------|---------------------|
| `make fuzz` fails | Exit code 2 from fuzz-loop.sh | Log, abort run, no triage | Yes — investigate build break |
| Fuzzer binary missing | Skip in fuzz-loop.sh | Noted in report as "SKIP" | Maybe — if many missing |
| Fuzzer hangs (no output) | `timeout` kills it | Marked as timeout in report | No |
| Crash flood (>50/fuzzer) | Count check in triage-crashes.py | Cap at 50, flag in report | Yes — investigate root cause |
| DeepSeek API down | HTTP error in analyze-crash.py | File issue without analysis, label `needs-human-triage` | Yes — review raw crash |
| Opus API down | HTTP error | Use DeepSeek-only, note in issue | No — DeepSeek analysis is sufficient |
| GitHub API down | HTTP error | Save `.issue.md` next to artifact, retry | No — self-healing |
| state.json corrupted | JSON parse error on load | Restore from .bak; if both broken, restart | Maybe — if both broken |
| Disk full on runner | OS error | Workflow fails, GitHub alerts | Yes — investigate runner |
| Runner killed (6h timeout) | Workflow cancelled | Partial results saved (crashes collected before timeout are triaged) | Maybe — run manually to complete |

### Crash Cap Rationale

If a single fuzzer produces >50 unique crashes in one run, something is probably
wrong with the fuzzer or the code under test — not 50 individual bugs. The cap
prevents:
- DOS-ing the GitHub issue tracker
- Burning LLM tokens on 50 variants of the same bug
- Flooding the state file

Capped crashes are still saved to disk. A human can run triage manually on them.

---

## 12. Security Boundaries

### Secrets

| Secret | Scope | Storage | Rotated |
|--------|-------|---------|---------|
| `DEEPSEEK_API_KEY` | DeepSeek API auth | GitHub Actions Secret | On compromise |
| `GITHUB_TOKEN` | GitHub API (issue creation) | Auto-injected by Actions | Per-run (automatic) |

No Anthropic API key is needed. Opus review uses the Claude Code subscription
via the `review-queue/` directory (batch-processed when convenient, no per-token cost).

### Data Egress

Only these leave the GitHub Actions runner to external APIs:

**To DeepSeek API:**
- Normalized stack traces (function names, file paths, line numbers)
- Source code snippets from the **public** Dilithion repository
- Fuzzer name, crash type

**To GitHub API:**
- Issue title and body (which contain the above data in formatted form)

**Never leaves the runner:**
- `state.json` contents (except crash signatures included in issue bodies)
- Wallet data, keys, addresses
- Node configuration
- Mainnet/testnet data

### Runner Isolation

- GitHub Actions runners are ephemeral VMs, destroyed after each run
- No persistent storage except what we explicitly commit or upload as artifacts
- Crash artifacts uploaded as workflow artifacts (90-day retention) for human download

---

## 13. Implementation Phases

### Phase 1 — Core Pipeline (Current)

| Component | Status |
|-----------|--------|
| `autonomous/DESIGN.md` | This document — in review |
| `autonomous/AGENTS.md` | To write after design approval |
| `autonomous/scripts/fuzz-loop.sh` | Drafted, needs review |
| `autonomous/scripts/triage-crashes.py` | To build |
| `autonomous/scripts/analyze-crash.py` | To build |
| `autonomous/state/state.json` | To seed |
| `.github/workflows/autonomous-pipeline.yml` | To build |

**Success criteria for Phase 1:**
- Pipeline runs on cron without human intervention
- Novel crashes produce analyzed, reviewed GitHub issues
- Known crashes are deduplicated silently
- Build failures are logged cleanly
- All failure modes in the matrix above are handled

### Phase 2 — Coverage & Regression (Future)

- Coverage Hunter agent (Codecov integration)
- Regression Watchdog agent (historical metric comparison)
- Auto-label PRs based on affected components in crash issues

### Phase 3 — Cross-Platform (Future)

- macOS runner for fuzzing
- Windows runner for fuzzing
- Cross-platform crash signature normalization (handle platform-specific stack formats)

---

## 14. Success Metrics

| Metric | Target | Measurement |
|--------|--------|-------------|
| **Pipeline uptime** | >95% of scheduled runs complete | GitHub Actions analytics |
| **Crash-to-issue latency** | <30 minutes from crash to filed issue | Timestamps in state.json |
| **False positive rate** | <5% of filed issues are not real bugs | Manual audit after 30 days |
| **Dedup accuracy** | 0 duplicate issues filed for the same crash | Manual audit |
| **Cost per novel crash** | <$0.05 | Token usage logs |
| **Coverage of fuzzers run** | 100% (all 23 run each marathon) | Marathon report |
| **Human intervention rate** | <1 incident/week requiring manual action | Count of workflow failures |

---

## 15. Decisions Log

| # | Question | Decision | Rationale |
|---|----------|----------|-----------|
| 1 | Crash reproduction? | **Yes.** `triage-crashes.py` re-runs fuzzer against each crash artifact. | Fresh stack trace with current build, ~2s per crash. |
| 2 | State file location? | **`autonomous/state/state.json`** | Colocated with pipeline scripts, SSOT-compliant. |
| 3 | Crash artifact retention? | **GitHub Actions artifacts (90-day).** Not committed to repo. | `fuzz_crashes/` is `.gitignore`'d. Artifacts downloadable from Actions UI. |
| 4 | Corpus persistence? | **Deferred to Phase 2.** Start without persistence, add GitHub Actions cache-based corpus later. | Simpler Phase 1, fewer failure modes. Full explanation in Section 13. |
| 5 | DeepSeek API key? | ✅ **Yes.** `DEEPSEEK_API_KEY` in GitHub Secrets. | |
| 6 | Anthropic / Opus? | ✅ **Subscription, not API key.** Claude Code subscription for review queue. No `ANTHROPIC_API_KEY` needed. | Flat subscription cost, no per-token charges. Review becomes asynchronous but free. |
| 7 | State file in git? | **Yes, committed.** | Allows tracking crash history across CI and local runs. No secrets in the file. |
| 8 | `fuzz_p2p_validation` build target? | **Needs verification.** Referenced in `project.yaml` and CI workflow but may not have an explicit Makefile target. | Verify before Phase 1 ships. |

### GitHub Issue Label Scheme

Every crash issue receives these labels to enable instant triage:

**Base labels (always present):**
- `bug` — confirmed bug
- `fuzzing` — found by autonomous fuzzing pipeline

**Severity (exactly one):**
- `severity/critical` — consensus fork, fund loss, memory corruption exploitable by remote peer
- `severity/high` — crash in consensus-adjacent code (UTXO, wallet, signature verification)
- `severity/medium` — crash in non-critical path (network messages, RPC)
- `severity/low` — crash in utility/encoding code with no exploitation path

**Component (one or more):**
- `component/consensus` — `src/consensus/`
- `component/crypto` — `src/crypto/`
- `component/utxo` — `src/node/utxo_set.cpp`
- `component/mempool` — `src/node/mempool.cpp`
- `component/net` — `src/net/`
- `component/wallet` — `src/wallet/`
- `component/rpc` — `src/rpc/`
- `component/vdf` — `src/vdf/`
- `component/dna` — `src/digital_dna/`

**Status flags (as needed):**
- `needs-human-triage` — LLM analysis unavailable, raw stack trace only
- `needs-opus-review` — DeepSeek analysis filed, awaiting Claude subscription review
- `deepseek-only` — Opus review not planned for this severity level

**Example label set for a critical consensus crash:**
```
bug, fuzzing, severity/critical, component/consensus, needs-opus-review
```

A human scanning the issue list can instantly see: "Critical consensus bug from fuzzing, DeepSeek has analyzed it but Opus hasn't reviewed yet."

---

*End of design document. This document must be reviewed and approved before implementation proceeds.*
