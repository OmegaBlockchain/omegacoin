#!/bin/bash
# Regtest verification of the SMSG security fixes. Usage: regtest.sh <patched-bin-dir> <unpatched-bin-dir> <workdir>
# Never touches the default datadir or default ports.
PB=$1; UB=$2; W=$3; POC=$(dirname "$(readlink -f "$0")")
. "$POC/lib.sh"
nap() { python3 -I -c "import time,sys; time.sleep(float(sys.argv[1]))" "$1"; }
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }
c1="$PB/omega-cli -datadir=$W/n1"; c2="$PB/omega-cli -datadir=$W/n2"; c3="$UB/omega-cli -datadir=$W/n3"
stopall() { for n in n1 n2 n3 n4 n5; do $PB/omega-cli -datadir=$W/$n stop >/dev/null 2>&1; done; nap 4; pkill -f "omegad -datadir=$W/" 2>/dev/null; }
trap stopall EXIT
nmsgs() { $1 smsgbuckets 2>/dev/null | python3 -I -c "import sys,json; d=json.load(sys.stdin); print(d['total']['messages'] if isinstance(d.get('total'),dict) and 'messages' in d['total'] else sum(int(b.get('no. messages', b.get('active messages',0))) for b in d.get('buckets',[])))" 2>/dev/null; }
nbkts() { $1 smsgbuckets 2>/dev/null | python3 -I -c "import sys,json; print(json.load(sys.stdin)['total']['numbuckets'])" 2>/dev/null; }
waitmsgs() { for i in $(seq 1 $3); do [ "$(nmsgs "$1")" = "$2" ] && return 0; nap 1; done; return 1; }

mkconf $W/n1 19561 19562 19563; mkconf $W/n2 19571 19572 19573; mkconf $W/n3 19581 19582 19583
$PB/omegad -datadir=$W/n1 -daemon >/dev/null; $PB/omegad -datadir=$W/n2 -daemon >/dev/null; $UB/omegad -datadir=$W/n3 -daemon >/dev/null
waitup "$c1" && waitup "$c2" && waitup "$c3" || { echo "nodes failed to start"; exit 2; }
echo "patched:   $($PB/omegad --version | head -1)"; echo "unpatched: $($UB/omegad --version | head -1)"
# SMSG networking is disabled during initial block download, so leave it first.
$c1 addnode 127.0.0.1:19572 onetry >/dev/null 2>&1; $c1 addnode 127.0.0.1:19582 onetry >/dev/null 2>&1
$c1 createwallet w >/dev/null 2>&1
$c1 generatetoaddress 1 "$($c1 getnewaddress)" >/dev/null 2>&1
for i in $(seq 1 60); do [ "$($c1 getblockcount)$($c2 getblockcount)$($c3 getblockcount)" = "111" ] && break; nap 1; done
ibd() { $1 getblockchaininfo | python3 -I -c "import sys,json; print(json.load(sys.stdin)['initialblockdownload'])"; }
check "all nodes left initial block download" "$(ibd "$c1")$(ibd "$c2")$(ibd "$c3")" "FalseFalseFalse"
echo "smsgbuckets raw: $($c1 smsgbuckets | tr -d '\n ' | cut -c1-200)"

echo "== SEC-01 smsgimport header length"
hex=$(python3 -I $POC/mk.py 0 2>$W/expect.txt); out=$($c1 smsgimport "$hex" 2>&1 | tr -d '\n ')
check "consistent message imports with the honest msgid" "$out" "{\"msgid\":\"$(cat $W/expect.txt)\"}"
for d in 1 64 -1 -96 2000000000 4294967200; do
  hex=$(python3 -I $POC/mk.py $d 2>/dev/null); out=$($c1 smsgimport "$hex" 2>&1 | tr -d '\n')
  case "$out" in *"Header payload length does not match data length"*) ok "delta=$d rejected";; *) bad "delta=$d not rejected: ${out:0:120}";; esac
done
check "node alive after malformed imports" "$($c1 getblockcount 2>&1)" "1"
check "only the valid message was stored" "$(nmsgs "$c1")" "1"

echo "== SEC-06 unsolicited smsgHave"
out=$(python3 -I $POC/have.py 19562); echo "  $out"
case "$out" in *"got smsgWant back: False"*) ok "no smsgWant for unsolicited smsgHave";; *) bad "unsolicited smsgHave answered";; esac
grep -q "Ignoring unsolicited smsgHave" $W/n1/regtest/debug.log && ok "unsolicited smsgHave logged and ignored" || bad "no ignore log line"
grep -q "Locking bucket" $W/n1/regtest/debug.log && bad "a bucket was locked" || ok "no bucket locked"
check "no bucket created by smsgHave" "$(nbkts "$c1")" "1"

echo "== sync still works: patched<->patched and patched<->unpatched"
waitmsgs "$c2" 1 90 && ok "patched n2 pulled message from patched n1" || bad "n2 has $(nmsgs "$c2") messages"
waitmsgs "$c3" 1 90 && ok "unpatched n3 pulled message from patched n1" || bad "n3 has $(nmsgs "$c3") messages"
hex=$(python3 -I $POC/mk.py 0 2>/dev/null); $c3 smsgimport "$hex" >/dev/null 2>&1
waitmsgs "$c1" 2 120 && ok "patched n1 pulled message from unpatched n3" || bad "n1 has $(nmsgs "$c1") messages"
waitmsgs "$c2" 2 120 && ok "patched n2 received it via n1" || bad "n2 has $(nmsgs "$c2") messages"
hex=$(python3 -I $POC/mk.py 0 2>/dev/null); $c2 smsgimport "$hex" >/dev/null 2>&1
waitmsgs "$c3" 3 120 && ok "unpatched n3 received a message relayed n2->n1->n3" || bad "n3 has $(nmsgs "$c3") messages"

echo "== SEC-05 trollbox listing and pruning"
$c1 smsgdisable >/dev/null 2>&1; $c1 smsgenable w 2>&1 | tr -d '\n' | cut -c1-80; echo
a=$($c1 getnewaddress); $c1 smsgaddlocaladdress $a >/dev/null 2>&1
o1=$($c1 trollboxsend "first" 2>&1 | tr -d '\n' | cut -c1-120); echo "  send1: $o1"
nap 31
o2=$($c1 trollboxsend "second" 2>&1 | tr -d '\n' | cut -c1-120); echo "  send2: $o2"
texts() { $c1 trollboxlist $1 2>/dev/null | python3 -I -c "import sys,json; print(','.join(m['text'] for m in json.load(sys.stdin)))" 2>/dev/null; }
# Sending is asynchronous (proof of work runs on its own thread).
for i in $(seq 1 90); do [ "$(texts 50)" = "first,second" ] && break; nap 1; done
check "trollboxlist returns oldest-first" "$(texts 50)" "first,second"
check "trollboxlist count=1 returns the newest" "$(texts 1)" "second"
check "trollboxlist default" "$(texts)" "first,second"
now=$(date +%s); $c1 setmocktime $((now + 23*3600)) >/dev/null 2>&1
check "inside retention (23h) still listed" "$(texts 50)" "first,second"
$c1 setmocktime $((now + 25*3600)) >/dev/null 2>&1
check "past retention (25h) not listed" "$(texts 50)" ""
$c1 stop >/dev/null 2>&1; nap 6
$PB/omegad -datadir=$W/n1 -mocktime=$((now + 25*3600)) -daemon >/dev/null; waitup "$c1"
$c1 smsgenable w >/dev/null 2>&1; nap 5
grep -q "Pruned 2 expired trollbox messages" $W/n1/regtest/debug.log && ok "expired trollbox rows pruned from the DB" || bad "prune log line missing: $(grep -c 'Pruned' $W/n1/regtest/debug.log)"
$c1 setmocktime 0 >/dev/null 2>&1
check "rows are gone for good" "$(texts 50)" ""
echo "== SEC-10 purged message must not be accepted again"
# The purged token stays in the in-memory bucket until the node restarts, and
# that alone blocks a re-import. Restart so only the purge record can stop it.
purge_roundtrip() { # <cli> <bindir> <datadir> -> sets OUT, CNT_BEFORE, CNT_AFTER
  local hex id
  hex=$(python3 -I $POC/mk.py 0 2>/dev/null)
  id=$($1 smsgimport "$hex" 2>/dev/null | python3 -I -c "import sys,json; print(json.load(sys.stdin)['msgid'])" 2>/dev/null)
  [ -n "$id" ] || { OUT="import-failed"; return; }
  $1 smsgpurge "$id" >/dev/null 2>&1
  $1 stop >/dev/null 2>&1; nap 6
  $2/omegad -datadir=$3 -daemon >/dev/null 2>&1; waitup "$1"; nap 2
  CNT_BEFORE=$(nmsgs "$1")
  OUT=$($1 smsgimport "$hex" 2>&1 | tr -d '\n ')
  CNT_AFTER=$(nmsgs "$1")
}
purge_roundtrip "$c2" "$PB" "$W/n2"
case "$OUT" in *"Storefailed"*) ok "patched node refuses a purged message after a restart";; *) bad "patched node re-accepted a purged message: ${OUT:0:120}";; esac
check "purged message is not stored again" "$CNT_AFTER" "$CNT_BEFORE"
hex=$(python3 -I $POC/mk.py 0 2>$W/expect.txt); out=$($c2 smsgimport "$hex" 2>&1 | tr -d '\n ')
check "unrelated message still imports after a purge" "$out" "{\"msgid\":\"$(cat $W/expect.txt)\"}"
purge_roundtrip "$c3" "$UB" "$W/n3"
# The unpatched node refuses it too: the zeroed token kept in the bucket blocks the
# re-import. SEC-10 therefore restores the cheap prefilter; it was not a bypass.
case "$OUT" in *"Storefailed"*) ok "unpatched node also refuses it (kept bucket token)";; *) bad "unexpected baseline result: ${OUT:0:120}";; esac

echo "== SEC-17 ZMQ address already in use"
c4="$PB/omega-cli -datadir=$W/n4"; c5="$UB/omega-cli -datadir=$W/n5"
mkconf $W/n4 19591 19592 19573; mkconf $W/n5 19601 19602 19573   # 19573 is held by n2
$PB/omegad -datadir=$W/n4 -daemon >/dev/null 2>&1
if waitup "$c4"; then ok "patched node starts although the ZMQ port is taken"; else bad "patched node did not start"; fi
grep -q "ZMQ publishing is disabled" $W/n4/regtest/debug.log && ok "the ZMQ failure is logged" || bad "no ZMQ failure log line"
$c4 stop >/dev/null 2>&1; nap 5
grep -q "Shutdown: done" $W/n4/regtest/debug.log && ok "patched node shuts down cleanly afterwards" || bad "no clean shutdown"
$UB/omegad -datadir=$W/n5 >$W/n5.out 2>&1 &
up5=no; for i in $(seq 1 20); do $c5 getblockcount >/dev/null 2>&1 && { up5=yes; break; }; nap 1; done
if [ "$up5" = no ] && grep -q "Assertion" $W/n5.out; then ok "unpatched node aborts on the same conflict (bug is real)"; else bad "baseline did not reproduce (up=$up5): $(tail -c 120 $W/n5.out)"; fi
$c5 stop >/dev/null 2>&1

echo "== BUG-01 SMSG address registrations and topic subscriptions survive a restart"
# Start() used to call ReadIni() before fSecMsgEnabled was set, so smsg.ini was
# never read and Shutdown() wrote an empty [Keys] list back.
restart_with_wallet() { # <cli> <bindir> <datadir>
  $1 stop >/dev/null 2>&1; for i in $(seq 1 30); do pgrep -f "omegad -datadir=$3 " >/dev/null || break; nap 1; done
  $2/omegad -datadir=$3 -wallet=w -daemon >/dev/null 2>&1; waitup "$1"; nap 2
}
listed() { $1 smsglocalkeys 2>/dev/null | grep -c "\"$2\""; }
subscribed() { $1 smsglisttopics 2>/dev/null | grep -c "\"$2\""; }
bug01() { # <cli> <bindir> <datadir> -> sets REG_BEFORE INI_HAS REG_AFTER SUB_AFTER
  $1 createwallet w >/dev/null 2>&1
  restart_with_wallet "$1" "$2" "$3"
  local a; a=$($1 getnewaddress)
  $1 smsgaddlocaladdress "$a" >/dev/null 2>&1
  $1 smsgsubscribe "omega.bug01" >/dev/null 2>&1
  REG_BEFORE=$(listed "$1" "$a")
  restart_with_wallet "$1" "$2" "$3"
  INI_HAS=$(grep -c "$a" "$3/regtest/smsg.ini")
  REG_AFTER=$(listed "$1" "$a")
  SUB_AFTER=$(subscribed "$1" "omega.bug01")
  # A second restart must not lose it either (the bug wiped smsg.ini on shutdown).
  restart_with_wallet "$1" "$2" "$3"
  REG_AFTER2=$(listed "$1" "$a")
}
bug01 "$c1" "$PB" "$W/n1"
check "registration listed before the restart" "$REG_BEFORE" "1"
check "smsg.ini kept the address over the restart" "$INI_HAS" "1"
check "registration still listed after a restart" "$REG_AFTER" "1"
check "registration still listed after a second restart" "$REG_AFTER2" "1"
check "topic subscription restored after a restart" "$SUB_AFTER" "1"
grep -q "Loaded addresses from SMSG.ini" $W/n1/regtest/debug.log && ok "addresses loaded from smsg.ini at start" || bad "no 'Loaded addresses from SMSG.ini' log line"
bug01 "$c3" "$UB" "$W/n3"
check "unpatched node loses the registration on restart (bug is real)" "$REG_AFTER" "0"

echo "== baseline: same PoC against the unpatched node"
out=$(python3 -I $POC/have.py 19582); echo "  $out"
case "$out" in *"got smsgWant back: True"*) ok "unpatched node answers the unsolicited smsgHave (PoC is valid)";; *) bad "baseline PoC did not trigger on unpatched node";; esac
check "node alive at end" "$($c1 getblockcount 2>&1)" "1"
echo "RESULT: $PASS passed, $FAIL failed"
