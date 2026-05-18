# Dilithion Autonomous Pipeline

Continuous fuzzing, crash analysis, and security auditing for the Dilithion
post-quantum cryptocurrency codebase.

## Quick Start

```bash
# Run a quick smoke test (~5 minutes)
./autonomous/scripts/fuzz-loop.sh --quick

# Run full marathon (~3.5 hours)
./autonomous/scripts/fuzz-loop.sh

# Triage crashes (after a marathon)
python3 autonomous/scripts/triage-crashes.py

# Dry-run the analysis (no API calls)
python3 autonomous/scripts/analyze-crash.py --crash-file <path> --fuzzer <name> \
  --crash-type <type> --severity <sev> --signature <sig> --dry-run
```

## How It Works

```
cron (every 6h) or manual trigger
        │
        ▼
┌──────────────────┐
│ fuzz-loop.sh     │  23 libFuzzer harnesses, tiered time allocation
│ Build + Marathon │  30min/15min/10min per fuzzer
└────────┬─────────┘
         │ crashes found
         ▼
┌──────────────────┐
│ triage-crashes.py│  Reproduce → normalize → hash → dedup
│ Crash Dedup      │  New crashes → route to analysis
└────────┬─────────┘
         │ novel crash
         ▼
┌──────────────────┐
│ analyze-crash.py │  DeepSeek API → root cause analysis
│ LLM Analysis     │  Critical/High → review-queue for Opus
│ + Issue Filing   │  GitHub API → file issue with labels
└──────────────────┘
```

## Secrets Required

| Secret | Purpose |
|--------|---------|
| `DEEPSEEK_API_KEY` | DeepSeek API for crash root-cause analysis |

`GITHUB_TOKEN` is auto-injected by GitHub Actions — no setup needed.

No Anthropic API key required. Opus review uses your Claude Code subscription
(saves to `review-queue/` for batch review when convenient).

## Directory Layout

```
autonomous/
├── README.md                   ← This file
├── pipeline-design.md          ← Full design document
├── scripts/
│   ├── fuzz-loop.sh            ← Marathon runner (Bash)
│   ├── triage-crashes.py       ← Crash dedup + triage (Python)
│   └── analyze-crash.py        ← LLM bridge + issue filing (Python)
├── state/
│   └── state.json              ← Crash signature database (committed)
├── review-queue/               ← Pending Opus reviews (Critical/High only)
│   ├── README.md               ← Review instructions
│   └── reviewed/               ← Completed reviews
└── .github/workflows/
    └── autonomous-pipeline.yml ← Cron + manual dispatch
```

## Labels

Every crash issue receives:

- `bug` + `fuzzing` (always)
- `severity/critical|high|medium|low` (exactly one)
- `component/consensus|crypto|utxo|mempool|net|wallet|rpc|vdf|dna` (one or more)
- `needs-human-triage` (LLM unavailable)
- `needs-opus-review` (awaiting Claude subscription review)

## Cost

~$0.0004 per novel crash (DeepSeek). Opus review: $0 (subscription).
Annual budget: under $2 even in worst-case scenarios.
