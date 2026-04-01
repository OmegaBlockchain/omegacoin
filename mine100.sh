#!/bin/bash
ADDR="$1"
if [ -z "$ADDR" ]; then
    echo "Usage: ./mine100.sh <testnet_address>"
    exit 1
fi
CLI="omega-cli -testnet"
NOW=$(date +%s)
for i in $(seq 1 100); do
    $CLI setmocktime $((NOW + i * 60))
    $CLI generatetoaddress 1 "$ADDR" >/dev/null
    echo "Block $i mined"
done
$CLI setmocktime 0
echo "Done. Mock time reset."
