# Crash Review Queue

You are reviewing DeepSeek's analysis of fuzz crashes in the Dilithion
cryptocurrency codebase. For each JSON file in this directory:

1. Read the crash context and DeepSeek's analysis
2. Verify: Is the root cause correctly identified?
3. Check: Is the severity classification appropriate?
4. Assess: Was anything important missed (consensus impact, exploitability)?
5. If corrections are needed, update the GitHub issue (`gh issue edit`)
6. Add a comment to the issue with your review notes (`gh issue comment`)
7. Move the reviewed JSON to `reviewed/`

Focus on:
- **Consensus impact** — could this cause a chain split?
- **Memory safety** — is this exploitable, and how?
- **Fix correctness** — will the suggested fix work?

## Usage

> "Claude, review the crash analyses in autonomous/review-queue/. For each one,
tell me if DeepSeek's analysis is accurate, what it missed, and whether the
severity classification is correct. Update the corresponding GitHub issues."
