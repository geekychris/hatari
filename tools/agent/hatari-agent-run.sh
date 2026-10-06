#!/bin/sh
#
# Start Hatari with the agent API enabled and wait until it answers.
#
# Usage: hatari-agent-run.sh [hatari options...]
#
# Environment:
#   HATARI       Hatari binary (default: build tree binary, then PATH)
#   AGENT_PORT   API port (default 7777)
#   AGENT_ROMS   ROM directory (default: <repo>/roms)
#   AGENT_TOS    TOS image (default: EmuTOS 512k US from ROM directory)
#   AGENT_LOG    Hatari output log file (default: /tmp/hatari-agent.log)

TOP=$(cd "$(dirname "$0")/../.." && pwd)
PORT=${AGENT_PORT:-7777}
ROMS=${AGENT_ROMS:-$TOP/roms}
LOG=${AGENT_LOG:-/tmp/hatari-agent.log}

if [ -z "$HATARI" ]; then
	for h in "$TOP/build/src/Hatari.app/Contents/MacOS/Hatari" \
		 "$TOP/build/src/hatari" "$(command -v hatari)"; do
		if [ -x "$h" ]; then
			HATARI=$h
			break
		fi
	done
fi
if [ ! -x "$HATARI" ]; then
	echo "ERROR: Hatari binary not found, set HATARI" >&2
	exit 1
fi

if curl -s -m 2 "http://127.0.0.1:$PORT/status" >/dev/null 2>&1; then
	echo "ERROR: something already answers on port $PORT" >&2
	exit 1
fi

TOS=${AGENT_TOS:-$ROMS/emutos/emutos-512k-1.4/etos512us.img}
TOSOPT=""
if [ -f "$TOS" ]; then
	TOSOPT="--tos $TOS"
else
	echo "WARNING: $TOS missing, run tools/agent/fetch-emutos.sh" >&2
fi
ROMOPT=""
if [ -d "$ROMS" ]; then
	ROMOPT="--agent-rom-dir $ROMS"
fi

# shellcheck disable=SC2086
"$HATARI" --agent-port "$PORT" $ROMOPT $TOSOPT --confirm-quit off "$@" >"$LOG" 2>&1 &
PID=$!

i=0
while [ $i -lt 50 ]; do
	if curl -s -m 1 "http://127.0.0.1:$PORT/status" >/dev/null 2>&1; then
		echo "Hatari (pid $PID) agent API ready at http://127.0.0.1:$PORT/ (log: $LOG)"
		exit 0
	fi
	if ! kill -0 $PID 2>/dev/null; then
		echo "ERROR: Hatari exited, see $LOG:" >&2
		tail -20 "$LOG" >&2
		exit 1
	fi
	sleep 0.2
	i=$((i+1))
done
echo "ERROR: agent API didn't come up, see $LOG" >&2
exit 1
