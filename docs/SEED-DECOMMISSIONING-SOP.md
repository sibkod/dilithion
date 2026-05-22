# Seed Decommissioning SOP

**Owner:** Operations · **Last updated:** 2026-05-22 (created during v4.4.3 hotfix red-team fold)

This document describes the steps required when **decommissioning a mainnet seed node** — taking a seed permanently out of service. It exists primarily to close a security gap introduced in v4.4.3.

## Why this exists (the gap being closed)

v4.4.3 added an in-binary rate-limit exemption for the 4 mainnet seed IPs (sourced from `chainparams.seedAttestationIPs`). The exemption is at the **per-IP token bucket** layer only — per-method limits (e.g., `walletpassphrase`, `sendtoaddress`, `dumpprivkey`, `forcerebuild`) still apply. That contains the worst-case wallet-attack surface but does NOT eliminate the following risk:

**The cloud-IP recycling problem.** If a DigitalOcean droplet is destroyed, the IP is recycled to other tenants. A binary that was built against a now-recycled seed IP will continue to exempt that IP from the per-IP rate limit until a new binary is released, built against an updated chainparams, and rolled. Anyone occupying that IP during that window gets the per-IP exemption "for free" — they can sustain higher request rates than a random outside IP can. Per-method limits still bound the wallet-tier methods, but the increase in budget for read methods can still be used (e.g., as a stronger reconnaissance vector or as part of a layered attack).

## Decommissioning SOP (mandatory)

When a seed is permanently taken offline (sold, destroyed, region-migrated, replaced with a different IP):

1. **Update `chainparams.seedAttestationIPs`** on `main`:
   - Remove the decommissioned IP.
   - If replacing: add the new IP in the same commit.
   - Commit message must include `seed-decommission:<old-ip>`.
2. **Cut a binary release** that incorporates the chainparams change:
   - This is a **release-blocker** event — must ship before the cloud provider can recycle the old IP. DigitalOcean's recycling window is typically days to weeks, so the binary release should ship within **7 days** of seed destruction.
   - The release does NOT need to be a full feature release. A point-release with just the chainparams change is fine.
   - Tag and ship per the normal release SOP (`docs/RELEASE-PROCESS.md` if present, or follow the most recent release for pattern).
3. **Deploy the new binary to all remaining seeds** via the normal rolling-restart procedure.
4. **Verify**: from one of the live seeds, send a high-rate burst from an outsider IP and confirm rate-limit kicks in. Then verify the same burst from one of the remaining live seed IPs is still allowed (no regression).
5. **Update operational state files**:
   - `dilithion-strategy/00-context/CURRENT_STATE.md` — update the seed-mesh table.
   - `memory/snippets.md` (or local equivalent) — update any hardcoded seed-IP references.
   - This README's "Active seeds" section below (keep in sync).

## Active seeds (current)

| Region | IP | P2P Port | RPC Port | Notes |
|---|---|---|---|---|
| NYC | 138.197.68.128 | 8444 | 8332 | Primary; bridge relayer + Explorer host |
| LDN | 167.172.56.119 | 8444 | 8332 | Relay-only |
| SGP | 165.22.103.114 | 8444 | 8332 | Relay-only |
| SYD | 134.199.159.83 | 8444 | 8332 | Relay-only |

When this table changes, the chainparams must also change. They are the single storage-of-record.

## Backstop: tag-time check (planned)

A future improvement (queued at `.claude/missions/release-blocker-gating/`) will add a check in the release-tag script that fails if any GitHub Issue labeled `release-blocker:vX.Y.Z` is open for that tag. Once that lands, this SOP can be enforced by adding a `release-blocker:vNEXT` issue at the moment a seed is decommissioned — the tag won't go out without the chainparams update.

Until that's in place, this SOP relies on operator discipline. **Document seed decommissioning in `CURRENT_STATE.md` immediately so any session has the context.**

## Related

- `src/rpc/ratelimiter.cpp` — exemption implementation, sources IPs from `chainparams.seedAttestationIPs`
- `src/core/chainparams.cpp` — single storage-of-record for seed IPs
- `src/test/ratelimiter_tests.cpp` — regression tests; `TestEmptyChainparamsFailsClosed` validates fail-closed behavior
- Red-team finding F-02 in `.claude/missions/cve-rpc-cors-wallet-drain/redteam_v4_4_3_b_and_c.md`
