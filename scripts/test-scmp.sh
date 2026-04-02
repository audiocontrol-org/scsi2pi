#!/usr/bin/env bash
#
# Automated SCMP test cycle.
# Builds, deploys to Pi, starts s2p with SCMP, waits for S3000XL activity,
# then dumps the trace.
#
# Usage:
#   ./scripts/test-scmp.sh [response_hex_bytes]
#
# Examples:
#   ./scripts/test-scmp.sh                    # Use default response (00 00 00)
#   ./scripts/test-scmp.sh "00 00 00"         # Explicit 3-byte response
#   ./scripts/test-scmp.sh "f0 7e 00 7f 00 f7"  # Full SDS ACK
#   ./scripts/test-scmp.sh "01 00 00"         # Try ready flag
#
# Requires:
#   - Pi at s3k.local with passwordless sudo for /tmp/s2p-midi
#   - Docker for cross-compilation
#   - S3000XL connected and configured for MIDI-via-SCSI to ID 0

set -euo pipefail

PI="orion@s3k.local"
RESPONSE_HEX="${1:-}"
WAIT_SECS="${2:-30}"

# Step 1: Build
echo "=== Building ==="
cd "$(dirname "$0")/.."
docker run --rm --platform linux/arm64 -v "$(pwd)":/src -w /src/cpp scsi2pi-build bash -c "make -j4 2>&1 | tail -2"

# Step 2: Deploy
echo "=== Deploying ==="
scp cpp/bin/s2p ${PI}:/tmp/s2p-midi

# Step 3: Set response bytes on Pi
if [ -n "$RESPONSE_HEX" ]; then
    echo "=== Setting 0x0D response: $RESPONSE_HEX ==="
    # Convert hex string to binary file
    ssh ${PI} "echo '$RESPONSE_HEX' | xxd -r -p > /tmp/scmp-response.bin && xxd /tmp/scmp-response.bin"
else
    echo "=== Using default 0x0D response (from /tmp/scmp-response.bin or 00 00 00) ==="
fi

# Step 4: Start s2p + attach SCMP
echo "=== Starting s2p with SCMP at ID 0 ==="
ssh ${PI} "rm -f /tmp/s2p-capture.log
sudo /tmp/s2p-midi --ignore-conf -L trace --log-limit 0 >/tmp/s2p-capture.log 2>&1 &
sleep 2
/tmp/s2pctl-midi -i 0 -c attach -t SCMP 2>&1
echo 'SCMP attached. Trigger dump on S3000XL now.'
echo 'Waiting ${WAIT_SECS} seconds...'
sleep ${WAIT_SECS}
echo '=== Trace summary ==='
echo \"Lines: \$(wc -l < /tmp/s2p-capture.log)\"
echo ''
strings /tmp/s2p-capture.log | grep -E 'executing|0x0C config|0x0D response|Receiving|Received|Timeout|RESET'
echo ''
echo '=== Unique commands ==='
strings /tmp/s2p-capture.log | grep 'executing' | sort -u
"

# Step 5: Kill s2p
echo "=== Cleaning up ==="
ssh ${PI} "sudo killall s2p-midi 2>/dev/null || true"

echo "=== Done ==="
