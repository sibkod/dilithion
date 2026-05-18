#!/usr/bin/env bash
# ============================================================================
# fuzz-loop.sh — Dilithion Autonomous Fuzz Marathon Runner
# ============================================================================
# Runs all 23 libFuzzer harnesses in tiered time-allocated campaigns,
# collects crash artifacts, and produces a structured report.
#
# Usage:  ./autonomous/scripts/fuzz-loop.sh [--quick]
#         --quick : reduced times (2min/1min/30s) for smoke testing
#
# Exit:   0 = no crashes, 1 = crashes found, 2 = build failure
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CRASH_DIR="$PROJECT_ROOT/fuzz_crashes"
REPORT_FILE="$CRASH_DIR/marathon-report-$(date -u +%Y-%m-%dT%H%M%S).md"
QUICK_MODE="${1:-}"

# ---------------------------------------------------------------------------
# Tier definitions: fuzzer_name → seconds
# ---------------------------------------------------------------------------

CRITICAL_FUZZERS=(
    fuzz_transaction
    fuzz_block
    fuzz_tx_validation
    fuzz_utxo
    fuzz_mempool
)

HIGH_FUZZERS=(
    fuzz_sha3
    fuzz_signature
    fuzz_serialize
    fuzz_merkle
    fuzz_difficulty
)

STANDARD_FUZZERS=(
    fuzz_compactsize
    fuzz_network_message
    fuzz_network_create
    fuzz_network_checksum
    fuzz_network_command
    fuzz_address
    fuzz_address_encode
    fuzz_address_validate
    fuzz_address_bech32
    fuzz_address_type
    fuzz_subsidy
    fuzz_base58
    fuzz_rpc
    fuzz_p2p_validation
)

if [ "$QUICK_MODE" = "--quick" ]; then
    CRITICAL_TIME=120
    HIGH_TIME=60
    STANDARD_TIME=30
    echo "[fuzz-loop] QUICK MODE — reduced run times"
else
    CRITICAL_TIME=1800   # 30 min
    HIGH_TIME=900        # 15 min
    STANDARD_TIME=600    # 10 min
fi

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------
cd "$PROJECT_ROOT"
mkdir -p "$CRASH_DIR"

echo "=== Fuzz Marathon $(date -u) ==="
echo "Critical: ${CRITICAL_TIME}s | High: ${HIGH_TIME}s | Standard: ${STANDARD_TIME}s"
echo ""

# ---------------------------------------------------------------------------
# Phase 1: Build fuzzers
# ---------------------------------------------------------------------------
echo "▸ Building fuzzers..."
if ! make fuzz -j"$(nproc)" 2>&1 | tail -5; then
    echo "✖ BUILD FAILED — aborting marathon"
    echo ""
    echo "## Fuzz Marathon Report — $(date -u)" > "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "**BUILD FAILED** — fuzzers could not be compiled. Marathon aborted." >> "$REPORT_FILE"
    exit 2
fi
echo "✓ All fuzzers built"
echo ""

# ---------------------------------------------------------------------------
# Phase 2: Run campaigns
# ---------------------------------------------------------------------------
declare -A RESULTS_TIME
declare -A RESULTS_EXEC
declare -A RESULTS_CRASHES
declare -A RESULTS_STATUS
ALL_FUZZERS=()
CRASH_FOUND=0

run_fuzzer() {
    local name="$1"
    local seconds="$2"
    local artifact_dir="$CRASH_DIR/$name"

    mkdir -p "$artifact_dir"

    echo -n "  $name (${seconds}s) ... "

    local start_ts=$(date +%s)

    # Run fuzzer with time limit, capture output for stats
    local output
    output=$(timeout $((seconds + 30)) ./"$name" \
        -max_total_time="$seconds" \
        -artifact_prefix="$artifact_dir/" \
        2>&1) || true

    local end_ts=$(date +%s)
    local elapsed=$((end_ts - start_ts))

    # Parse libFuzzer stats
    local execs=$(echo "$output" | grep -oP '#\d+\s' | tail -1 | tr -d '# \t' || echo "0")
    local crash_count=$(find "$artifact_dir" -maxdepth 1 \( -name 'crash-*' -o -name 'timeout-*' -o -name 'oom-*' -o -name 'leak-*' \) 2>/dev/null | wc -l)

    RESULTS_TIME[$name]=$elapsed
    RESULTS_EXEC[$name]=${execs:-0}
    RESULTS_CRASHES[$name]=$crash_count
    ALL_FUZZERS+=("$name")

    if [ "$crash_count" -gt 0 ]; then
        RESULTS_STATUS[$name]="⚠️"
        CRASH_FOUND=1
        echo "$crash_count crashes"
    else
        RESULTS_STATUS[$name]="✅"
        echo "clean"
    fi
}

# Run in tier order
echo "▸ CRITICAL tier (${CRITICAL_TIME}s each)"
for f in "${CRITICAL_FUZZERS[@]}"; do
    [ -x "./$f" ] || { echo "  $f — SKIP (not executable)"; continue; }
    run_fuzzer "$f" "$CRITICAL_TIME"
done

echo ""
echo "▸ HIGH tier (${HIGH_TIME}s each)"
for f in "${HIGH_FUZZERS[@]}"; do
    [ -x "./$f" ] || { echo "  $f — SKIP (not executable)"; continue; }
    run_fuzzer "$f" "$HIGH_TIME"
done

echo ""
echo "▸ STANDARD tier (${STANDARD_TIME}s each)"
for f in "${STANDARD_FUZZERS[@]}"; do
    [ -x "./$f" ] || { echo "  $f — SKIP (not executable)"; continue; }
    run_fuzzer "$f" "$STANDARD_TIME"
done

# ---------------------------------------------------------------------------
# Phase 3: Generate report
# ---------------------------------------------------------------------------
{
    echo "## Fuzz Marathon Report — $(date -u)"
    echo ""
    echo "| Fuzzer | Tier | Time | Executions | Crashes | Status |"
    echo "|--------|------|------|------------|---------|--------|"

    for f in "${ALL_FUZZERS[@]}"; do
        tier="Standard"
        for c in "${CRITICAL_FUZZERS[@]}"; do [ "$c" = "$f" ] && tier="Critical"; done
        for h in "${HIGH_FUZZERS[@]}"; do [ "$h" = "$f" ] && tier="High"; done
        echo "| $f | $tier | ${RESULTS_TIME[$f]}s | ${RESULTS_EXEC[$f]} | ${RESULTS_CRASHES[$f]} | ${RESULTS_STATUS[$f]} |"
    done

    echo ""
    total_crashes=0
    for f in "${ALL_FUZZERS[@]}"; do
        total_crashes=$((total_crashes + RESULTS_CRASHES[$f]))
    done
    echo "**Total crashes found: $total_crashes**"
    echo ""
    echo "Crash artifacts: \`$CRASH_DIR/\`"
} > "$REPORT_FILE"

echo ""
echo "=== Report: $REPORT_FILE ==="
cat "$REPORT_FILE"

exit $CRASH_FOUND
