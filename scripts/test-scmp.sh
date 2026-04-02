#!/usr/bin/env bash
#
# End-to-end SCSI MIDI test.
# Builds, deploys, and runs an SDS round-trip via SCSI.
#
# Usage: ./scripts/test-scmp.sh [sample_number]

set -euo pipefail

PI="orion@s3k.local"
SAMPLE="${1:-99}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

echo "=== Building ==="
cd "$ROOT"
docker run --rm --platform linux/arm64 -v "$(pwd)":/src -w /src/cpp scsi2pi-build bash -c "make -j4 2>&1 | tail -2"

echo "=== Deploying ==="
scp cpp/bin/s2p ${PI}:/tmp/s2p-midi
scp scripts/scsi-midi-test.py ${PI}:/tmp/scsi-midi-test.py

echo "=== Stopping old processes ==="
ssh ${PI} "sudo killall s2p-midi 2>/dev/null; rm -f /tmp/scsi-midi-bridge.sock /tmp/s2p-capture.log; true"

echo "=== Starting test harness + s2p ==="
ssh ${PI} "
# Start test harness in background (creates socket server)
python3 /tmp/scsi-midi-test.py ${SAMPLE} > /tmp/scsi-midi-test.log 2>&1 &
TEST_PID=\$!
echo \"Test harness PID: \$TEST_PID\"
sleep 1

# Start s2p with SCMP
sudo /tmp/s2p-midi --ignore-conf -L trace --log-limit 0 >/tmp/s2p-capture.log 2>&1 &
S2P_PID=\$!
echo \"s2p PID: \$S2P_PID\"
sleep 2

# Attach SCMP
/tmp/s2pctl-midi -i 0 -c attach -t SCMP 2>&1

echo 'Waiting for test to complete (60s max)...'
for i in \$(seq 1 60); do
  if ! kill -0 \$TEST_PID 2>/dev/null; then
    echo \"Test harness exited after \${i}s\"
    break
  fi
  sleep 1
done

# Kill remaining processes
kill \$TEST_PID 2>/dev/null || true
sudo killall s2p-midi 2>/dev/null || true

echo ''
echo '=== Test harness output ==='
cat /tmp/scsi-midi-test.log

echo ''
echo '=== SCSI trace (key events) ==='
strings /tmp/s2p-capture.log | grep -E 'MIDI|executing|Receiving|Received|Sending|Timeout|RESET' | head -50
" 2>&1

echo "=== Done ==="
