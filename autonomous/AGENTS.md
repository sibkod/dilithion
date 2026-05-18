# Autonomous Pipeline — Quick Reference

One-page orientation for developers and AI agents working on Dilithion.

## What It Does

Every night at 2 AM UTC, the fuzzing workflow runs 9 libFuzzer harnesses.
When crashes are found, a crash-triage job automatically:
- Reproduces the crash → normalizes the stack trace → hashes into a stable signature
- Deduplicates against known crashes in `autonomous/state/state.json`
- Sends novel crashes to DeepSeek for root-cause analysis
- Files a GitHub issue with severity and component labels
- Queues Critical/High crashes for Claude Opus review (via subscription)

## Files That Matter

| File | Purpose | Tokens |
|------|---------|--------|
| `pipeline-design.md` | Full architecture, threat model, failure modes | Read for context |
| `scripts/triage-crashes.py` | Crash dedup engine (deterministic) | 0 |
| `scripts/analyze-crash.py` | DeepSeek bridge + issue filing | ~1K/crash |
| `state/state.json` | Crash signature database (SSOT) | 0 |
| `review-queue/README.md` | Opus review instructions | 0 |
| `../.github/workflows/fuzz-extended-campaigns.yml` | Cron + crash-triage job | — |

## How To

**Trigger manually:** Actions → Fuzzing Extended Campaigns → Run workflow

**Review queued crashes:** Point Claude Code at `review-queue/`:
> "Claude, review the crash analyses in autonomous/review-queue/"

**Add a new fuzzer:** Add a job to `fuzz-extended-campaigns.yml` following the existing pattern. The crash-triage job will pick it up automatically.

**Check state:** `cat autonomous/state/state.json` — records every crash ever seen.

## Labels

Every crash issue gets: `bug` + `fuzzing` + `severity/*` + `component/*`

Status flags: `needs-human-triage` (LLM down), `needs-opus-review` (awaiting review), `deepseek-only` (low severity, no review needed).

## Known Limitations

- Only 9 of 23 fuzzers run in CI (the rest are local-only via `fuzz-loop.sh`)
- Corpus not persisted between runs (Phase 2)
- State cache evicted after 7 days of pipeline inactivity (self-heals on next run)
- Windows/macOS fuzzing not covered (Phase 3)

## Design Principles

- **KISS:** Bash + Python stdlib. No frameworks, no npm.
- **SSOT:** `state.json` is canonical. `.triaged` files are a cache.
- **Deterministic-first:** Only novel crashes touch an LLM.
- **Fail safe:** Every failure mode degrades gracefully — no pipeline outage blocks development.
