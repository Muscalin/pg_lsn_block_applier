#!/bin/sh
set -eu

version=$1
bindir=${PG_BINDIR:-/usr/lib/postgresql/$version/bin}
binary=${BINARY:-/tmp/pg_lsn_block_applier_$version}
datadir=/tmp/pg_lsn_block_applier_data_$version
port=$((55000 + version))

rm -rf "$datadir"
"$bindir/initdb" -D "$datadir" -A trust --no-sync \
    --wal-segsize="${PG_TEST_WAL_SEGSIZE:-16}" >/dev/null
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=off -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
trap '"$bindir/pg_ctl" -D "$datadir" stop -m immediate >/dev/null 2>&1 || true' EXIT

sql() { "$bindir/psql" -X -qAt -h /tmp -p "$port" -d postgres -c "$1"; }

# Replay one relation block across the requested WAL interval.  Callers keep
# their own assertions about the records and the expected page contents.
replay_case() {
    "$binary" --waldir="$1" --base-page="$2" --out="$3" --rel="$4" \
        --block="$5" --start="$6" --stop="$7"
}

sql "CREATE TABLE replay_test (id integer PRIMARY KEY, value text) WITH (autovacuum_enabled=false)" >/dev/null
sql "INSERT INTO replay_test VALUES (1, 'first')" >/dev/null
sql 'CHECKPOINT' >/dev/null

relpath=$(sql "SELECT pg_relation_filepath('replay_test')")
dbid=$(sql 'SELECT oid FROM pg_database WHERE datname = current_database()')
relid=$(sql "SELECT pg_relation_filenode('replay_test')")
dd if="$datadir/$relpath" of="$datadir/base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')

sql "UPDATE replay_test SET value = 'second' WHERE id = 1" >/dev/null
sql "INSERT INTO replay_test VALUES (2, 'temporary')" >/dev/null
sql 'DELETE FROM replay_test WHERE id = 2' >/dev/null
sql 'CHECKPOINT' >/dev/null

dd if="$datadir/$relpath" of="$datadir/final.page" bs=8192 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/records.txt"
last_lsn=$(grep "rel 1663/$dbid/$relid blk 0" "$datadir/records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')

replay_case "$datadir/pg_wal" "$datadir/base.page" \
    "$datadir/replayed.page" "1663/$dbid/$relid" "0" "$start" "$last_lsn" \
    > "$datadir/replay.log"

compare_pages() {
python3 - "$1" "$2" <<'PY'
import struct
import sys

def normalized(path):
    page = bytearray(open(path, 'rb').read())
    assert len(page) == 8192
    page[8:10] = b'\0\0'  # disk checksum depends on hint bits
    page[20:24] = b'\0\0\0\0'  # prune XID is a hint
    lower = struct.unpack_from('<H', page, 12)[0]
    upper = struct.unpack_from('<H', page, 14)[0]
    page[lower:upper] = bytes(upper - lower)  # unused page space
    for pos in range(24, lower, 4):
        item = struct.unpack_from('<I', page, pos)[0]
        offset, flags = item & 0x7fff, (item >> 15) & 3
        if flags == 1:  # LP_NORMAL
            infomask = struct.unpack_from('<H', page, offset + 20)[0]
            struct.pack_into('<H', page, offset + 20, infomask & ~0x0f00)
    return page

left, right = map(normalized, sys.argv[1:])
if left != right:
    differences = [i for i, (a, b) in enumerate(zip(left, right)) if a != b]
    raise SystemExit(f'page differs outside checksum/hint bits: {differences[:16]}')
print('page matches after masking checksum, hints, and free space')
PY
}

check_prune_xid_unchanged() {
python3 - "$1" "$2" "$3" <<'PY'
import struct
import sys

values = [struct.unpack_from('<I', open(path, 'rb').read(), 20)[0]
          for path in sys.argv[1:]]
assert values[0] == values[1] == values[2], f'pd_prune_xid changed on insert: {values}'
print('pd_prune_xid unchanged by insert WAL')
PY
}

compare_pages "$datadir/replayed.page" "$datadir/final.page"
cat "$datadir/replay.log"

"$binary" --help > "$datadir/help.txt"
"$binary" --version > "$datadir/version.txt"
grep -q -- '--base-page' "$datadir/help.txt"
grep -q -- '--start=2A9C/51FB7280' "$datadir/help.txt"
grep -q 'pg_lsn_block_applier' "$datadir/version.txt"

# Omitted --start uses the page header LSN.  Exercise the --input alias and
# require exactly one JSON event for each applied record plus the summary.
"$binary" --waldir="$datadir/pg_wal" --input="$datadir/base.page" \
    --out="$datadir/json-replayed.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" --json > "$datadir/replay.jsonl"
compare_pages "$datadir/json-replayed.page" "$datadir/final.page"
python3 - "$datadir/replay.jsonl" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1])]
assert events[-1]['event'] == 'done'
assert events[-1]['applied'] == len(events) - 1
assert all(e['event'] == 'applied' and e['lsn'] and e['end_lsn']
           and isinstance(e['rmid'], int) and isinstance(e['info'], int)
           and e['operation']
           for e in events[:-1])
assert events[-1]['applied'] > 0
print('JSON events and default start verified')
PY

"$binary" --waldir="$datadir/pg_wal" --base-page="$datadir/base.page" \
    --out="$datadir/warn-replayed.page" --rel="1663/$dbid/$relid" \
    --block=0 --start="$start" --stop="$last_lsn" \
    > /dev/null 2> "$datadir/start-warning.txt"
grep -q 'warning: base page LSN .* precedes --start' "$datadir/start-warning.txt"

# pg_waldump --save-fullpage can preserve a page-header LSN older than the
# available WAL.  Reproduce that condition and check the recovery hint.
python3 - "$datadir/base.page" "$datadir/old-lsn.page" <<'PY'
import struct
import sys
from pathlib import Path

page = bytearray(Path(sys.argv[1]).read_bytes())
struct.pack_into('<II', page, 0, 0, 1)
Path(sys.argv[2]).write_bytes(page)
PY
if "$binary" --waldir="$datadir/pg_wal" \
    --base-page="$datadir/old-lsn.page" \
    --out="$datadir/old-lsn-failed.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" \
    > "$datadir/old-lsn.out" 2> "$datadir/old-lsn.err"; then
    echo 'old page LSN unexpectedly succeeded without --start' >&2
    exit 1
fi
grep -q 'base page LSN 0/00000001 is older than available WAL; pass --start' \
    "$datadir/old-lsn.err"
grep -q 'for --save-fullpage files, use the LSN from the file name' \
    "$datadir/old-lsn.err"
test ! -e "$datadir/old-lsn-failed.page"
"$binary" --waldir="$datadir/pg_wal" \
    --base-page="$datadir/old-lsn.page" \
    --out="$datadir/old-lsn-replayed.page" --rel="1663/$dbid/$relid" \
    --block=0 --start="$start" --stop="$last_lsn" \
    > /dev/null 2> "$datadir/old-lsn-with-start.err"
compare_pages "$datadir/old-lsn-replayed.page" "$datadir/final.page"
echo 'old page LSN requires --start from available WAL'

if [ "${PG_TEST_QUICK:-0}" = 1 ]; then
    echo 'basic replay and CLI checks passed'
    exit 0
fi

# Reject malformed page headers before any WAL handler can use them.  A
# completely zero page is tested later with GIN CREATE_PTREE.
python3 - "$datadir/base.page" "$datadir" <<'PY'
import struct
import sys
from pathlib import Path

base = bytearray(Path(sys.argv[1]).read_bytes())
directory = Path(sys.argv[2])
for name, offset, value in [('special', 16, 8191),
                            ('size', 18, 4096 | 4),
                            ('version', 18, 8192 | 3)]:
    page = bytearray(base)
    struct.pack_into('<H', page, offset, value)
    (directory / f'bad-{name}.page').write_bytes(page)
page = bytearray(8192)
page[0] = 1
(directory / 'bad-partial-zero.page').write_bytes(page)
PY
expect_bad_page() {
    kind=$1
    expected=$2
    out="$datadir/bad-$kind-replayed.page"
    if replay_case "$datadir/pg_wal" "$datadir/bad-$kind.page" "$out" \
        "1663/$dbid/$relid" 0 "$start" "$last_lsn" > "$datadir/bad-$kind.log" 2>&1; then
        echo "malformed $kind page was accepted" >&2
        exit 1
    fi
    grep -F "$expected" "$datadir/bad-$kind.log" >/dev/null
    test ! -e "$out"
}
expect_bad_page special 'misaligned page special pointer'
expect_bad_page size 'invalid page size or layout version'
expect_bad_page version 'invalid page size or layout version'
expect_bad_page partial-zero 'corrupted uninitialized page'
echo 'malformed page headers rejected without output'

sql 'CREATE TABLE replay_insert_hint (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_insert_hint VALUES (1)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_insert_hint')")
relid=$(sql "SELECT pg_relation_filenode('replay_insert_hint')")
dd if="$datadir/$relpath" of="$datadir/insert-hint-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_insert_hint VALUES (2)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/insert-hint-final.page" bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/insert-hint-records.txt"
grep 'rmgr: Heap' "$datadir/insert-hint-records.txt" | grep 'desc: INSERT ' |
    grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
replay_case "$datadir/pg_wal" "$datadir/insert-hint-base.page" \
    "$datadir/insert-hint-replayed.page" "1663/$dbid/$relid" 0 "$start" "$stop" \
    > "$datadir/insert-hint-replay.log"
grep -F 'applied Heap INSERT' "$datadir/insert-hint-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/insert-hint-replay.log" >/dev/null; then
    echo 'Heap insert hint test replayed an FPI' >&2
    exit 1
fi
compare_pages "$datadir/insert-hint-replayed.page" "$datadir/insert-hint-final.page"
check_prune_xid_unchanged "$datadir/insert-hint-base.page" \
    "$datadir/insert-hint-replayed.page" "$datadir/insert-hint-final.page"

sql "CREATE TABLE replay_bulk (id integer, value text) WITH (autovacuum_enabled=false)" >/dev/null
sql "INSERT INTO replay_bulk VALUES (1, 'seed')" >/dev/null
sql 'CHECKPOINT' >/dev/null

relpath=$(sql "SELECT pg_relation_filepath('replay_bulk')")
relid=$(sql "SELECT pg_relation_filenode('replay_bulk')")
dd if="$datadir/$relpath" of="$datadir/bulk-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')

sql "INSERT INTO replay_bulk SELECT n, repeat('x', 10) FROM generate_series(2, 20) AS n" >/dev/null
sql 'DELETE FROM replay_bulk WHERE id % 2 = 0' >/dev/null
sql 'VACUUM (FREEZE) replay_bulk' >/dev/null
sql 'CHECKPOINT' >/dev/null

dd if="$datadir/$relpath" of="$datadir/bulk-final.page" bs=8192 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/bulk-records.txt"
last_lsn=$(grep "rel 1663/$dbid/$relid blk 0" "$datadir/bulk-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')

replay_case "$datadir/pg_wal" "$datadir/bulk-base.page" \
    "$datadir/bulk-replayed.page" "1663/$dbid/$relid" "0" "$start" "$last_lsn" \
    > "$datadir/bulk-replay.log"
compare_pages "$datadir/bulk-replayed.page" "$datadir/bulk-final.page"
cat "$datadir/bulk-replay.log"

"$bindir/pg_ctl" -D "$datadir" stop -m fast >/dev/null
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=on -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
sql "CREATE TABLE replay_fpi (id integer, value text) WITH (autovacuum_enabled=false)" >/dev/null
sql "INSERT INTO replay_fpi VALUES (1, 'before')" >/dev/null
sql 'CHECKPOINT' >/dev/null

relpath=$(sql "SELECT pg_relation_filepath('replay_fpi')")
relid=$(sql "SELECT pg_relation_filenode('replay_fpi')")
dd if="$datadir/$relpath" of="$datadir/fpi-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "UPDATE replay_fpi SET value = 'after' WHERE id = 1" >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/fpi-final.page" bs=8192 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/fpi-records.txt"
last_lsn=$(grep "rel 1663/$dbid/$relid blk 0" "$datadir/fpi-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/fpi-base.page" \
    "$datadir/fpi-replayed.page" "1663/$dbid/$relid" "0" "$start" "$last_lsn" \
    > "$datadir/fpi-replay.log"
compare_pages "$datadir/fpi-replayed.page" "$datadir/fpi-final.page"
cat "$datadir/fpi-replay.log"

"$bindir/pg_ctl" -D "$datadir" stop -m fast >/dev/null
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=off -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
sql "CREATE TABLE replay_copy (id integer, value text) WITH (autovacuum_enabled=false)" >/dev/null
sql "INSERT INTO replay_copy VALUES (1, 'seed')" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_copy')")
relid=$(sql "SELECT pg_relation_filenode('replay_copy')")
dd if="$datadir/$relpath" of="$datadir/copy-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
awk 'BEGIN { for (i = 2; i <= 20; i++) print i "\tvalue" i }' |
    "$bindir/psql" -X -q -h /tmp -p "$port" -d postgres -c 'COPY replay_copy FROM STDIN'
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/copy-final.page" bs=8192 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/copy-records.txt"
grep 'MULTI_INSERT' "$datadir/copy-records.txt" |
    grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
last_lsn=$(grep "rel 1663/$dbid/$relid blk 0" "$datadir/copy-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/copy-base.page" \
    "$datadir/copy-replayed.page" "1663/$dbid/$relid" "0" "$start" "$last_lsn" \
    > "$datadir/copy-replay.log"
grep -F 'applied Heap2 MULTI_INSERT' "$datadir/copy-replay.log" >/dev/null
compare_pages "$datadir/copy-replayed.page" "$datadir/copy-final.page"
check_prune_xid_unchanged "$datadir/copy-base.page" \
    "$datadir/copy-replayed.page" "$datadir/copy-final.page"
cat "$datadir/copy-replay.log"

# VACUUM's second pass only marks dead line pointers unused.  An earlier
# on-access prune followed by insertion leaves live tuples out of line-pointer
# order, making an accidental page compaction visible in their lp_off values.
sql 'CREATE TABLE replay_vacuum_layout (id integer PRIMARY KEY, value text) WITH (fillfactor=100, autovacuum_enabled=false)' >/dev/null
sql "INSERT INTO replay_vacuum_layout VALUES (1, repeat('a', 300)), (2, repeat('b', 300))" >/dev/null
sql 'DELETE FROM replay_vacuum_layout WHERE id = 1' >/dev/null
sql "UPDATE replay_vacuum_layout SET value = repeat('c', 300) WHERE id = 2" >/dev/null
sql "UPDATE replay_vacuum_layout SET value = repeat('d', 300) WHERE id = 2" >/dev/null
sql 'ALTER TABLE replay_vacuum_layout SET (fillfactor=10)' >/dev/null
sql 'SELECT value FROM replay_vacuum_layout WHERE id = 2' >/dev/null
sql "INSERT INTO replay_vacuum_layout VALUES (3, repeat('e', 300))" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_vacuum_layout')")
relid=$(sql "SELECT pg_relation_filenode('replay_vacuum_layout')")
dd if="$datadir/$relpath" of="$datadir/vacuum-layout-base.page" bs=8192 count=1 status=none
python3 - "$datadir/vacuum-layout-base.page" <<'PY'
import struct
import sys

page = open(sys.argv[1], 'rb').read()
lower = struct.unpack_from('<H', page, 12)[0]
offsets = []
for pos in range(24, lower, 4):
    item = struct.unpack_from('<I', page, pos)[0]
    if (item >> 15) & 3 == 1:
        offsets.append(item & 0x7fff)
assert len(offsets) >= 2 and any(a < b for a, b in zip(offsets, offsets[1:])), \
    f'VACUUM layout setup did not leave out-of-order tuples: {offsets}'
print('VACUUM layout input has out-of-order tuple offsets')
PY
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'VACUUM (INDEX_CLEANUP ON) replay_vacuum_layout' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/vacuum-layout-final.page" bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/vacuum-layout-records.txt"
if test "$version" -lt 17; then
    vacuum_op='VACUUM '
    vacuum_log_op='VACUUM:'
else
    vacuum_op='PRUNE_VACUUM_CLEANUP'
    vacuum_log_op='PRUNE_VACUUM_CLEANUP'
fi
grep 'rmgr: Heap2' "$datadir/vacuum-layout-records.txt" |
    grep -F "desc: $vacuum_op" | grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
last_lsn=$(grep -F "rel 1663/$dbid/$relid blk 0" "$datadir/vacuum-layout-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$last_lsn"
replay_case "$datadir/pg_wal" "$datadir/vacuum-layout-base.page" \
    "$datadir/vacuum-layout-replayed.page" "1663/$dbid/$relid" "0" "$start" "$last_lsn" \
    > "$datadir/vacuum-layout-replay.log"
grep -F "applied Heap2 $vacuum_log_op" "$datadir/vacuum-layout-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/vacuum-layout-replay.log" >/dev/null; then
    echo 'VACUUM layout test replayed an FPI' >&2
    exit 1
fi
compare_pages "$datadir/vacuum-layout-replayed.page" "$datadir/vacuum-layout-final.page"
echo 'VACUUM second pass preserves live tuple offsets'

"$bindir/pg_ctl" -D "$datadir" stop -m fast >/dev/null
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=on -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
sql "CREATE TABLE replay_index (id integer PRIMARY KEY, value text) WITH (autovacuum_enabled=false)" >/dev/null
sql "INSERT INTO replay_index VALUES (1, 'seed')" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_index_pkey')")
relid=$(sql "SELECT pg_relation_filenode('replay_index_pkey')")
dd if="$datadir/$relpath" of="$datadir/index-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_index VALUES (2, 'next')" >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/index-final.page" bs=8192 skip=1 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/index-records.txt"
last_lsn=$(grep "rel 1663/$dbid/$relid blk 1" "$datadir/index-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/index-base.page" \
    "$datadir/index-replayed.page" "1663/$dbid/$relid" "1" "$start" "$last_lsn" \
    > "$datadir/index-replay.log"
python3 - "$datadir/index-replayed.page" "$datadir/index-final.page" <<'PY'
import struct
import sys

def normalized(path):
    page = bytearray(open(path, 'rb').read())
    assert len(page) == 8192
    lower, upper = struct.unpack_from('<HH', page, 12)
    page[8:10] = b'\0\0'
    page[lower:upper] = bytes(upper - lower)
    return page

assert normalized(sys.argv[1]) == normalized(sys.argv[2])
print('Btree FPI page matches')
PY
cat "$datadir/index-replay.log"

"$bindir/pg_ctl" -D "$datadir" stop -m fast >/dev/null
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=off -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
sql "CREATE TABLE replay_index_insert (id integer PRIMARY KEY) WITH (autovacuum_enabled=false)" >/dev/null
sql 'INSERT INTO replay_index_insert VALUES (1), (3)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_index_insert_pkey')")
relid=$(sql "SELECT pg_relation_filenode('replay_index_insert_pkey')")
dd if="$datadir/$relpath" of="$datadir/index-insert-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_index_insert VALUES (2)' >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/index-insert-final.page" bs=8192 skip=1 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/index-insert-records.txt"
grep 'INSERT_LEAF' "$datadir/index-insert-records.txt" |
    grep -F "rel 1663/$dbid/$relid blk 1" >/dev/null
last_lsn=$(grep "rel 1663/$dbid/$relid blk 1" "$datadir/index-insert-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/index-insert-base.page" \
    "$datadir/index-insert-replayed.page" "1663/$dbid/$relid" "1" "$start" "$last_lsn" \
    > "$datadir/index-insert-replay.log"
grep -F 'applied Btree INSERT_LEAF' "$datadir/index-insert-replay.log" >/dev/null
python3 - "$datadir/index-insert-replayed.page" "$datadir/index-insert-final.page" <<'PY'
import struct
import sys

def normalized(path):
    page = bytearray(open(path, 'rb').read())
    assert len(page) == 8192
    lower, upper = struct.unpack_from('<HH', page, 12)
    page[8:10] = b'\0\0'
    page[lower:upper] = bytes(upper - lower)
    return page

assert normalized(sys.argv[1]) == normalized(sys.argv[2])
print('Btree incremental INSERT_LEAF page matches')
PY
cat "$datadir/index-insert-replay.log"

compare_index_pages() {
python3 - "$1" "$2" "${3:-}" <<'PY'
import struct
import sys

def normalized(path):
    page = bytearray(open(path, 'rb').read())
    assert len(page) == 8192
    lower, upper = struct.unpack_from('<HH', page, 12)
    assert 24 <= lower <= upper <= 8192
    page[8:10] = b'\0\0'  # checksum
    page[lower:upper] = bytes(upper - lower)  # unallocated tuple space
    if sys.argv[3] == 'gist':
        special = struct.unpack_from('<H', page, 16)[0]
        page[special + 12] &= ~0x04  # GiST's tuple-deleted hint has no WAL record
    return page

left, right = normalized(sys.argv[1]), normalized(sys.argv[2])
assert left == right, f'index page differs at {[i for i, (a, b) in enumerate(zip(left, right)) if a != b][:16]}'
print('incremental index page matches')
PY
}

make_blank_page() {
python3 - "$1" <<'PY'
import struct
import sys

page = bytearray(8192)
struct.pack_into('<HHHH', page, 12, 24, 8192, 8192, 8192 | 4)
open(sys.argv[1], 'wb').write(page)
PY
}

# A split record changes the old leaf, a new right leaf and sometimes more
# pages.  Replay only the old leaf.  FPIs are disabled for this entire case.
sql 'CREATE TABLE replay_split (id integer PRIMARY KEY) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_split SELECT generate_series(1, 150)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_split_pkey')")
relid=$(sql "SELECT pg_relation_filenode('replay_split_pkey')")
dd if="$datadir/$relpath" of="$datadir/split-base.page" bs=8192 skip=1 count=1 status=none
dd if="$datadir/$relpath" of="$datadir/split-meta-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_split SELECT generate_series(151, 500)' >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/split-final.page" bs=8192 skip=1 count=1 status=none
dd if="$datadir/$relpath" of="$datadir/split-meta-final.page" bs=8192 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/split-records.txt"
grep -E 'SPLIT_[LR]' "$datadir/split-records.txt" |
    grep -F "rel 1663/$dbid/$relid blk 1" >/dev/null
last_lsn=$(grep -F "rel 1663/$dbid/$relid blk 1" "$datadir/split-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/split-base.page" \
    "$datadir/split-replayed.page" "1663/$dbid/$relid" "1" "$start" "$last_lsn" \
    > "$datadir/split-replay.log"
grep -E 'applied Btree SPLIT_[LR]' "$datadir/split-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/split-replay.log"; then
    echo 'B-tree split test replayed an FPI instead of the increment' >&2
    exit 1
fi
compare_index_pages "$datadir/split-replayed.page" "$datadir/split-final.page"
cat "$datadir/split-replay.log"
make_blank_page "$datadir/split-right-base.page"

meta_lsn=$(grep 'rmgr: Btree' "$datadir/split-records.txt" | grep 'NEWROOT' |
    grep -F "rel 1663/$dbid/$relid blk 0" | tail -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$meta_lsn"
replay_case "$datadir/pg_wal" "$datadir/split-meta-base.page" \
    "$datadir/split-meta-replayed.page" "1663/$dbid/$relid" "0" "$start" "$meta_lsn" \
    > "$datadir/split-meta-replay.log"
grep -F 'applied Btree NEWROOT' "$datadir/split-meta-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/split-meta-replay.log"; then
    echo 'B-tree metapage test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/split-meta-replayed.page" "$datadir/split-meta-final.page"
cat "$datadir/split-meta-replay.log"

root_blk=$(grep 'rmgr: Btree' "$datadir/split-records.txt" | grep 'NEWROOT' |
    grep -F "rel 1663/$dbid/$relid" | head -1 |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$root_blk"
root_lsn=$(grep 'rmgr: Btree' "$datadir/split-records.txt" | grep 'NEWROOT' |
    head -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
dd if="$datadir/$relpath" of="$datadir/split-root-final.page" bs=8192 skip="$root_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/split-right-base.page" \
    "$datadir/split-root-replayed.page" "1663/$dbid/$relid" "$root_blk" "$start" "$root_lsn" \
    > "$datadir/split-root-replay.log"
grep -F 'applied Btree NEWROOT' "$datadir/split-root-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/split-root-replay.log"; then
    echo 'B-tree root initialization test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/split-root-replayed.page" "$datadir/split-root-final.page"
cat "$datadir/split-root-replay.log"

right_blk=$(grep -E 'SPLIT_[LR]' "$datadir/split-records.txt" |
    grep -F "blkref #0: rel 1663/$dbid/$relid blk 1" | head -1 |
    sed -n 's/.*blkref #1: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$right_blk"
dd if="$datadir/$relpath" of="$datadir/split-right-final.page" bs=8192 skip="$right_blk" count=1 status=none
right_lsn=$(grep -F "rel 1663/$dbid/$relid blk $right_blk" "$datadir/split-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/split-right-base.page" \
    "$datadir/split-right-replayed.page" "1663/$dbid/$relid" "$right_blk" "$start" "$right_lsn" \
    > "$datadir/split-right-replay.log"
grep -E 'applied Btree SPLIT_[LR].*block_ref=1' "$datadir/split-right-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/split-right-replay.log"; then
    echo 'B-tree right-page split test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/split-right-replayed.page" "$datadir/split-right-final.page"
tail -4 "$datadir/split-right-replay.log"

sql 'CREATE TABLE replay_split_left (id integer PRIMARY KEY) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_split_left SELECT generate_series(500, 351, -1)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_split_left_pkey')")
relid=$(sql "SELECT pg_relation_filenode('replay_split_left_pkey')")
dd if="$datadir/$relpath" of="$datadir/split-left-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_split_left SELECT generate_series(350, 1, -1)' >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/split-left-final.page" bs=8192 skip=1 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/split-left-records.txt"
grep 'rmgr: Btree' "$datadir/split-left-records.txt" | grep 'SPLIT_L' |
    grep -F "blkref #0: rel 1663/$dbid/$relid blk 1" >/dev/null
last_lsn=$(grep -F "rel 1663/$dbid/$relid blk 1" "$datadir/split-left-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/split-left-base.page" \
    "$datadir/split-left-replayed.page" "1663/$dbid/$relid" "1" "$start" "$last_lsn" \
    > "$datadir/split-left-replay.log"
grep -F 'applied Btree SPLIT_L' "$datadir/split-left-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/split-left-replay.log"; then
    echo 'B-tree left split test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/split-left-replayed.page" "$datadir/split-left-final.page"
tail -4 "$datadir/split-left-replay.log"

next_sibling=$(grep 'SPLIT_L' "$datadir/split-left-records.txt" |
    grep -F "blkref #0: rel 1663/$dbid/$relid blk 1" | head -1 |
    sed -n 's/.*blkref #1: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$next_sibling"
dd if="$datadir/$relpath" of="$datadir/split-left-sibling-final.page" bs=8192 skip="$next_sibling" count=1 status=none
sibling_lsn=$(grep -F "rel 1663/$dbid/$relid blk $next_sibling" "$datadir/split-left-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/split-right-base.page" \
    "$datadir/split-left-sibling-replayed.page" "1663/$dbid/$relid" "$next_sibling" "$start" "$sibling_lsn" \
    > "$datadir/split-left-sibling-replay.log"
grep -E 'applied Btree SPLIT_L.*block_ref=2' "$datadir/split-left-sibling-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/split-left-sibling-replay.log"; then
    echo 'B-tree sibling test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/split-left-sibling-replayed.page" "$datadir/split-left-sibling-final.page"
grep -E 'applied Btree SPLIT_L' "$datadir/split-left-sibling-replay.log"

left_root_blk=$(grep 'rmgr: Btree' "$datadir/split-left-records.txt" | grep 'NEWROOT' |
    head -1 | sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$left_root_blk"
grep 'rmgr: Btree' "$datadir/split-left-records.txt" | grep 'INSERT_UPPER' |
    grep -F "rel 1663/$dbid/$relid blk $left_root_blk" >/dev/null
dd if="$datadir/$relpath" of="$datadir/split-left-root-final.page" bs=8192 skip="$left_root_blk" count=1 status=none
left_root_lsn=$(grep -F "rel 1663/$dbid/$relid blk $left_root_blk" "$datadir/split-left-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/split-right-base.page" \
    "$datadir/split-left-root-replayed.page" "1663/$dbid/$relid" "$left_root_blk" "$start" "$left_root_lsn" \
    > "$datadir/split-left-root-replay.log"
grep -F 'applied Btree INSERT_UPPER' "$datadir/split-left-root-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/split-left-root-replay.log"; then
    echo 'B-tree upper-level insertion test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/split-left-root-replayed.page" "$datadir/split-left-root-final.page"
grep -E 'applied Btree (NEWROOT|INSERT_UPPER)' "$datadir/split-left-root-replay.log"

sql 'CREATE TABLE replay_btree_dedup (value integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_btree_dedup_idx ON replay_btree_dedup (value)' >/dev/null
sql 'INSERT INTO replay_btree_dedup VALUES (1)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_btree_dedup_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_btree_dedup_idx')")
dd if="$datadir/$relpath" of="$datadir/btree-dedup-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_btree_dedup SELECT 1 FROM generate_series(1, 500)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/btree-dedup-final.page" bs=8192 skip=1 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/btree-dedup-records.txt"
grep 'rmgr: Btree' "$datadir/btree-dedup-records.txt" | grep 'desc: DEDUP ' >/dev/null
replay_case "$datadir/pg_wal" "$datadir/btree-dedup-base.page" \
    "$datadir/btree-dedup-replayed.page" "1663/$dbid/$relid" 1 "$start" "$stop" \
    > "$datadir/btree-dedup-replay.log"
grep -F 'applied Btree DEDUP' "$datadir/btree-dedup-replay.log" >/dev/null
compare_index_pages "$datadir/btree-dedup-replayed.page" "$datadir/btree-dedup-final.page"
tail -4 "$datadir/btree-dedup-replay.log"

start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "DELETE FROM replay_btree_dedup WHERE ctid = '(0,50)'" >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_btree_dedup' >/dev/null
vacuum_stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/btree-post-base.page" bs=8192 skip=1 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$vacuum_stop" > "$datadir/btree-post-vacuum-records.txt"
grep 'rmgr: Btree' "$datadir/btree-post-vacuum-records.txt" |
    grep 'desc: VACUUM ' | grep -E 'nupdated:? 1' >/dev/null
replay_case "$datadir/pg_wal" "$datadir/btree-dedup-final.page" \
    "$datadir/btree-post-vacuum-replayed.page" "1663/$dbid/$relid" "1" "$start" "$vacuum_stop" \
    > "$datadir/btree-post-vacuum-replay.log"
grep -F 'applied Btree VACUUM' "$datadir/btree-post-vacuum-replay.log" >/dev/null
compare_index_pages "$datadir/btree-post-vacuum-replayed.page" "$datadir/btree-post-base.page"
tail -3 "$datadir/btree-post-vacuum-replay.log"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_btree_dedup VALUES (1)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/btree-post-final.page" bs=8192 skip=1 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/btree-post-records.txt"
grep 'rmgr: Btree' "$datadir/btree-post-records.txt" | grep 'INSERT_POST' >/dev/null
replay_case "$datadir/pg_wal" "$datadir/btree-post-base.page" \
    "$datadir/btree-post-replayed.page" "1663/$dbid/$relid" 1 "$start" "$stop" \
    > "$datadir/btree-post-replay.log"
grep -F 'applied Btree INSERT_POST' "$datadir/btree-post-replay.log" >/dev/null
compare_index_pages "$datadir/btree-post-replayed.page" "$datadir/btree-post-final.page"
tail -3 "$datadir/btree-post-replay.log"

# Hash INSERT touches both a bucket and the metapage.  Check each block
# separately from the same WAL record, without relying on a full-page image.
sql 'CREATE TABLE replay_hash (id integer, value text) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_hash SELECT n, n::text FROM generate_series(1, 20) AS n' >/dev/null
sql 'CREATE INDEX replay_hash_idx ON replay_hash USING hash (value)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_hash_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_hash_idx')")
cp "$datadir/$relpath" "$datadir/hash-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_hash VALUES (21, '21')" >/dev/null
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-final.rel"
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-records.txt"
hash_record=$(grep 'rmgr: Hash' "$datadir/hash-records.txt" | grep 'desc: INSERT ' |
    grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$hash_record"
hash_lsn=$(printf '%s\n' "$hash_record" | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
hash_blk=$(printf '%s\n' "$hash_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$hash_lsn"
test -n "$hash_blk"
for blk in "$hash_blk" 0; do
    dd if="$datadir/hash-base.rel" of="$datadir/hash-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/hash-final.rel" of="$datadir/hash-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/hash-base-$blk.page" \
        "$datadir/hash-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$hash_lsn" \
        > "$datadir/hash-replay-$blk.log"
    grep -F 'applied Hash INSERT' "$datadir/hash-replay-$blk.log" >/dev/null
    if grep -q 'applied FPI' "$datadir/hash-replay-$blk.log"; then
        echo 'Hash INSERT test replayed an FPI instead of the increment' >&2
        exit 1
    fi
    compare_index_pages "$datadir/hash-replayed-$blk.page" "$datadir/hash-final-$blk.page"
    cat "$datadir/hash-replay-$blk.log"
done

sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/hash-meta-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_hash WHERE id <= 10' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_hash' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/hash-meta-final.page" bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-meta-records.txt"
grep 'rmgr: Hash' "$datadir/hash-meta-records.txt" |
    grep 'UPDATE_META_PAGE' | grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
replay_case "$datadir/pg_wal" "$datadir/hash-meta-base.page" \
    "$datadir/hash-meta-replayed.page" "1663/$dbid/$relid" 0 "$start" "$stop" \
    > "$datadir/hash-meta-replay.log"
grep -F 'applied Hash UPDATE_META_PAGE' "$datadir/hash-meta-replay.log" >/dev/null
compare_index_pages "$datadir/hash-meta-replayed.page" "$datadir/hash-meta-final.page"
tail -3 "$datadir/hash-meta-replay.log"

sql 'CREATE TABLE replay_hash_init (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_hash_init VALUES (1), (2)' >/dev/null
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CREATE INDEX replay_hash_init_idx ON replay_hash_init USING hash (id)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_hash_init_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_hash_init_idx')")
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-init-records.txt"
init_start=$(grep 'rmgr: Hash' "$datadir/hash-init-records.txt" |
    grep 'INIT_META_PAGE' | head -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$init_start"
bitmap_blk=$(grep 'rmgr: Hash' "$datadir/hash-init-records.txt" |
    grep 'INIT_BITMAP_PAGE' | head -1 |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$bitmap_blk"
make_blank_page "$datadir/hash-init-base.page"
for blk in 0 "$bitmap_blk"; do
    dd if="$datadir/$relpath" of="$datadir/hash-init-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/hash-init-base.page" \
        "$datadir/hash-init-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$init_start" "$stop" \
        > "$datadir/hash-init-replay-$blk.log"
    if grep -q 'applied FPI' "$datadir/hash-init-replay-$blk.log"; then
        echo 'Hash initialization replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/hash-init-replayed-$blk.page" "$datadir/hash-init-final-$blk.page"
    tail -3 "$datadir/hash-init-replay-$blk.log"
done
grep -F 'applied Hash INIT_META_PAGE' "$datadir/hash-init-replay-0.log" >/dev/null
grep -F 'applied Hash INIT_BITMAP_PAGE' "$datadir/hash-init-replay-$bitmap_blk.log" >/dev/null

sql 'CREATE TABLE replay_hash_overflow (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_hash_overflow_idx ON replay_hash_overflow USING hash (id)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_hash_overflow_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_hash_overflow_idx')")
cp "$datadir/$relpath" "$datadir/hash-overflow-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_hash_overflow SELECT 1 FROM generate_series(1, 1000)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-overflow-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-overflow-records.txt"
overflow_record=$(grep 'rmgr: Hash' "$datadir/hash-overflow-records.txt" |
    grep 'ADD_OVFL_PAGE' | tail -1)
test -n "$overflow_record"
new_blk=$(printf '%s\n' "$overflow_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
left_blk=$(printf '%s\n' "$overflow_record" |
    sed -n 's/.*blkref #1: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$new_blk"
test -n "$left_blk"
for blk in "$new_blk" "$left_blk" 0; do
    if test $(( (blk + 1) * 8192 )) -le "$(wc -c < "$datadir/hash-overflow-base.rel")"; then
        dd if="$datadir/hash-overflow-base.rel" of="$datadir/hash-overflow-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    else
        make_blank_page "$datadir/hash-overflow-base-$blk.page"
    fi
    dd if="$datadir/hash-overflow-final.rel" of="$datadir/hash-overflow-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/hash-overflow-base-$blk.page" \
        "$datadir/hash-overflow-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/hash-overflow-replay-$blk.log"
    grep -F 'applied Hash ADD_OVFL_PAGE' "$datadir/hash-overflow-replay-$blk.log" >/dev/null
    compare_index_pages "$datadir/hash-overflow-replayed-$blk.page" "$datadir/hash-overflow-final-$blk.page"
    tail -2 "$datadir/hash-overflow-replay-$blk.log"
done

sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-squeeze-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_hash_overflow WHERE ctid IN (SELECT ctid FROM replay_hash_overflow LIMIT 900)' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_hash_overflow' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-squeeze-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-squeeze-records.txt"
squeeze_record=$(grep 'rmgr: Hash' "$datadir/hash-squeeze-records.txt" |
    grep 'SQUEEZE_PAGE' | head -1)
test -n "$squeeze_record"
for ref in 1 2 5; do
    blk=$(printf '%s\n' "$squeeze_record" |
        sed -n "s/.*blkref #$ref: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p")
    test -n "$blk"
    dd if="$datadir/hash-squeeze-base.rel" of="$datadir/hash-squeeze-base-$ref.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/hash-squeeze-final.rel" of="$datadir/hash-squeeze-final-$ref.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/hash-squeeze-base-$ref.page" \
        "$datadir/hash-squeeze-replayed-$ref.page" "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/hash-squeeze-replay-$ref.log"
    grep -F 'applied Hash SQUEEZE_PAGE' "$datadir/hash-squeeze-replay-$ref.log" >/dev/null
    compare_index_pages "$datadir/hash-squeeze-replayed-$ref.page" "$datadir/hash-squeeze-final-$ref.page"
    tail -2 "$datadir/hash-squeeze-replay-$ref.log"
done

# A partly emptied overflow chain must move tuples between existing hash
# pages before it can squeeze away the tail.  Check the destination page's
# incremental WAL, not just the later SQUEEZE_PAGE records.
sql 'CREATE TABLE replay_hash_move (id integer, value integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_hash_move SELECT n, 1 FROM generate_series(1, 1500) AS n' >/dev/null
sql 'CREATE INDEX replay_hash_move_idx ON replay_hash_move USING hash (value)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_hash_move_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_hash_move_idx')")
cp "$datadir/$relpath" "$datadir/hash-move-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_hash_move WHERE id % 3 = 0' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_hash_move' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-move-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-move-records.txt"
move_record=$(grep 'rmgr: Hash' "$datadir/hash-move-records.txt" |
    grep 'desc: MOVE_PAGE_CONTENTS' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$move_record"
for ref in 1 2; do
    move_blk=$(printf '%s\n' "$move_record" |
        sed -n "s/.*blkref #$ref: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p")
    test -n "$move_blk"
    dd if="$datadir/hash-move-base.rel" of="$datadir/hash-move-base-$ref.page" bs=8192 skip="$move_blk" count=1 status=none
    dd if="$datadir/hash-move-final.rel" of="$datadir/hash-move-final-$ref.page" bs=8192 skip="$move_blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/hash-move-base-$ref.page" \
        "$datadir/hash-move-replayed-$ref.page" "1663/$dbid/$relid" "$move_blk" "$start" "$stop" \
        > "$datadir/hash-move-replay-$ref.log"
    grep -F "applied Hash MOVE_PAGE_CONTENTS" "$datadir/hash-move-replay-$ref.log" |
        grep -F "block_ref=$ref" >/dev/null
    if grep -F 'applied FPI' "$datadir/hash-move-replay-$ref.log" >/dev/null; then
        echo 'Hash page-content move replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/hash-move-replayed-$ref.page" "$datadir/hash-move-final-$ref.page"
done
echo 'Hash MOVE_PAGE_CONTENTS source and destination increments match'

# bloom uses PostgreSQL's Generic WAL manager for index changes.
sql 'CREATE EXTENSION IF NOT EXISTS bloom' >/dev/null
sql 'CREATE TABLE replay_generic (a integer, b integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_generic SELECT n, n FROM generate_series(1, 200) AS n' >/dev/null
sql 'CREATE INDEX replay_generic_idx ON replay_generic USING bloom (a, b)' >/dev/null
sql 'INSERT INTO replay_generic VALUES (201, 201)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_generic_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_generic_idx')")
dd if="$datadir/$relpath" of="$datadir/generic-base.page" bs=8192 skip=2 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_generic VALUES (202, 202)' >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/generic-final.page" bs=8192 skip=2 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/generic-records.txt"
generic_lsn=$(grep 'rmgr: Generic' "$datadir/generic-records.txt" |
    grep -F "rel 1663/$dbid/$relid blk 2" | tail -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$generic_lsn"
replay_case "$datadir/pg_wal" "$datadir/generic-base.page" \
    "$datadir/generic-replayed.page" "1663/$dbid/$relid" "2" "$start" "$generic_lsn" \
    > "$datadir/generic-replay.log"
grep -F 'applied Generic CHANGE' "$datadir/generic-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/generic-replay.log"; then
    echo 'Generic test replayed an FPI instead of the increment' >&2
    exit 1
fi
compare_index_pages "$datadir/generic-replayed.page" "$datadir/generic-final.page"
cat "$datadir/generic-replay.log"

# Force bucket splits.  The old bucket receives flag updates and deletes;
# the metapage receives both per-insert counts and split metadata.
sql 'CREATE TABLE replay_hash_split (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_hash_split SELECT generate_series(1, 20)' >/dev/null
sql 'CREATE INDEX replay_hash_split_idx ON replay_hash_split USING hash (id) WITH (fillfactor=10)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_hash_split_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_hash_split_idx')")
cp "$datadir/$relpath" "$datadir/hash-split-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_hash_split SELECT generate_series(21, 3000)' >/dev/null
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-split-final.rel"
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-split-records.txt"
grep 'rmgr: Hash' "$datadir/hash-split-records.txt" | grep 'SPLIT_ALLOCATE_PAGE' |
    grep -F "blkref #0: rel 1663/$dbid/$relid blk 1" >/dev/null
for blk in 1 0; do
    dd if="$datadir/hash-split-base.rel" of="$datadir/hash-split-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/hash-split-final.rel" of="$datadir/hash-split-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    last_lsn=$(grep -F "rel 1663/$dbid/$relid blk $blk" "$datadir/hash-split-records.txt" |
        tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
    replay_case "$datadir/pg_wal" "$datadir/hash-split-base-$blk.page" \
        "$datadir/hash-split-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$last_lsn" \
        > "$datadir/hash-split-replay-$blk.log"
    grep -F 'applied Hash SPLIT_ALLOCATE_PAGE' "$datadir/hash-split-replay-$blk.log" >/dev/null
    if grep -q 'applied FPI' "$datadir/hash-split-replay-$blk.log"; then
        echo 'Hash split test replayed an FPI on the selected block' >&2
        exit 1
    fi
    compare_index_pages "$datadir/hash-split-replayed-$blk.page" "$datadir/hash-split-final-$blk.page"
    grep -E 'applied Hash (SPLIT_ALLOCATE_PAGE|SPLIT_COMPLETE|DELETE|SPLIT_CLEANUP)' \
        "$datadir/hash-split-replay-$blk.log" | head -6
done
new_bucket_blk=$(grep 'SPLIT_ALLOCATE_PAGE' "$datadir/hash-split-records.txt" |
    grep -F "blkref #0: rel 1663/$dbid/$relid blk 1" | head -1 |
    sed -n 's/.*blkref #1: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$new_bucket_blk"
dd if="$datadir/hash-split-final.rel" of="$datadir/hash-split-new-final.page" bs=8192 skip="$new_bucket_blk" count=1 status=none
new_bucket_lsn=$(grep -F "rel 1663/$dbid/$relid blk $new_bucket_blk" "$datadir/hash-split-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/split-right-base.page" \
    "$datadir/hash-split-new-replayed.page" "1663/$dbid/$relid" "$new_bucket_blk" "$start" "$new_bucket_lsn" \
    > "$datadir/hash-split-new-replay.log"
grep -F 'applied Hash SPLIT_ALLOCATE_PAGE' "$datadir/hash-split-new-replay.log" >/dev/null
grep -F 'applied Hash SPLIT_COMPLETE' "$datadir/hash-split-new-replay.log" >/dev/null
grep -F 'applied FPI' "$datadir/hash-split-new-replay.log" >/dev/null
compare_index_pages "$datadir/hash-split-new-replayed.page" "$datadir/hash-split-new-final.page"
grep -E 'applied (Hash SPLIT_|FPI)' "$datadir/hash-split-new-replay.log" | head -5

sql 'CREATE TABLE replay_index_vacuum (id integer PRIMARY KEY) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_index_vacuum SELECT generate_series(1, 100)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_index_vacuum_pkey')")
relid=$(sql "SELECT pg_relation_filenode('replay_index_vacuum_pkey')")
dd if="$datadir/$relpath" of="$datadir/index-vacuum-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_index_vacuum WHERE id <= 30' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_index_vacuum' >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/index-vacuum-final.page" bs=8192 skip=1 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/index-vacuum-records.txt"
vacuum_record=$(grep 'rmgr: Btree' "$datadir/index-vacuum-records.txt" | grep 'desc: VACUUM ' |
    grep -F "rel 1663/$dbid/$relid blk 1" | head -1)
test -n "$vacuum_record"
vacuum_lsn=$(printf '%s\n' "$vacuum_record" | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/index-vacuum-base.page" \
    "$datadir/index-vacuum-replayed.page" "1663/$dbid/$relid" "1" "$start" "$vacuum_lsn" \
    > "$datadir/index-vacuum-replay.log"
grep -F 'applied Btree VACUUM' "$datadir/index-vacuum-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/index-vacuum-replay.log"; then
    echo 'B-tree VACUUM test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/index-vacuum-replayed.page" "$datadir/index-vacuum-final.page"
cat "$datadir/index-vacuum-replay.log"

# GiST leaf update and GIN entry insertion each exercise their own rmgr's
# incremental payload, with full_page_writes still disabled.
sql 'CREATE TABLE replay_gist (id integer, area box) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_gist_idx ON replay_gist USING gist (area)' >/dev/null
sql "INSERT INTO replay_gist VALUES (1, box(point(0,0), point(1,1)))" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_gist_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_gist_idx')")
dd if="$datadir/$relpath" of="$datadir/gist-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gist VALUES (2, box(point(2,2), point(3,3)))" >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/gist-final.page" bs=8192 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gist-records.txt"
gist_record=$(grep 'rmgr: Gist' "$datadir/gist-records.txt" | grep 'PAGE_UPDATE' |
    grep -F "rel 1663/$dbid/$relid blk 0" | head -1)
test -n "$gist_record"
gist_lsn=$(printf '%s\n' "$gist_record" | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/gist-base.page" \
    "$datadir/gist-replayed.page" "1663/$dbid/$relid" "0" "$start" "$gist_lsn" \
    > "$datadir/gist-replay.log"
grep -F 'applied GiST PAGE_UPDATE' "$datadir/gist-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/gist-replay.log"; then
    echo 'GiST test replayed an FPI instead of the increment' >&2
    exit 1
fi
compare_index_pages "$datadir/gist-replayed.page" "$datadir/gist-final.page"
cat "$datadir/gist-replay.log"

# Grow a GiST root across a page split, replaying the root incrementally.
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/gist-split-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gist SELECT n, box(point(n, n), point(n + 1, n + 1)) FROM generate_series(3, 1000) AS n" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/gist-split-final.page" bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gist-split-records.txt"
grep 'rmgr: Gist' "$datadir/gist-split-records.txt" | grep 'PAGE_SPLIT' >/dev/null
replay_case "$datadir/pg_wal" "$datadir/gist-split-base.page" \
    "$datadir/gist-split-replayed.page" "1663/$dbid/$relid" 0 "$start" "$stop" \
    > "$datadir/gist-split-replay.log"
grep -F 'applied GiST PAGE_SPLIT' "$datadir/gist-split-replay.log" >/dev/null
compare_index_pages "$datadir/gist-split-replayed.page" "$datadir/gist-split-final.page"
tail -3 "$datadir/gist-split-replay.log"
make_blank_page "$datadir/gist-new-base.page"
dd if="$datadir/$relpath" of="$datadir/gist-new-final.page" bs=8192 skip=1 count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gist-new-base.page" \
    "$datadir/gist-new-replayed.page" "1663/$dbid/$relid" 1 "$start" "$stop" \
    > "$datadir/gist-new-replay.log"
grep -F 'applied GiST PAGE_SPLIT' "$datadir/gist-new-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/gist-new-replay.log"; then
    echo 'GiST new page replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/gist-new-replayed.page" "$datadir/gist-new-final.page"
tail -3 "$datadir/gist-new-replay.log"

sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gist-delete-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_gist' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_gist' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gist-delete-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gist-delete-records.txt"
delete_record=$(grep 'rmgr: Gist' "$datadir/gist-delete-records.txt" |
    grep 'PAGE_DELETE' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$delete_record"
for ref in 0 1; do
    blk=$(printf '%s\n' "$delete_record" |
        sed -n "s/.*blkref #$ref: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p")
    test -n "$blk"
    dd if="$datadir/gist-delete-base.rel" of="$datadir/gist-delete-base-$ref.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/gist-delete-final.rel" of="$datadir/gist-delete-final-$ref.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/gist-delete-base-$ref.page" \
        "$datadir/gist-delete-replayed-$ref.page" "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/gist-delete-replay-$ref.log"
    grep -F 'applied GiST PAGE_DELETE' "$datadir/gist-delete-replay-$ref.log" >/dev/null
    if grep -q 'applied FPI' "$datadir/gist-delete-replay-$ref.log"; then
        echo 'GiST page deletion replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/gist-delete-replayed-$ref.page" "$datadir/gist-delete-final-$ref.page" gist
    tail -3 "$datadir/gist-delete-replay-$ref.log"
done

sql 'CREATE TABLE replay_gin (id integer, words tsvector) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_gin_idx ON replay_gin USING gin (words) WITH (fastupdate=off)' >/dev/null
sql "INSERT INTO replay_gin VALUES (1, to_tsvector('english', 'alpha'))" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_gin_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_gin_idx')")
dd if="$datadir/$relpath" of="$datadir/gin-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gin VALUES (2, to_tsvector('english', 'beta'))" >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/gin-final.page" bs=8192 skip=1 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-records.txt"
gin_record=$(grep 'rmgr: Gin' "$datadir/gin-records.txt" | grep 'desc: INSERT ' |
    grep -F "rel 1663/$dbid/$relid blk 1" | head -1)
test -n "$gin_record"
gin_lsn=$(printf '%s\n' "$gin_record" | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/gin-base.page" \
    "$datadir/gin-replayed.page" "1663/$dbid/$relid" "1" "$start" "$gin_lsn" \
    > "$datadir/gin-replay.log"
grep -F 'applied GIN INSERT' "$datadir/gin-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/gin-replay.log"; then
    echo 'GIN test replayed an FPI instead of the increment' >&2
    exit 1
fi
compare_index_pages "$datadir/gin-replayed.page" "$datadir/gin-final.page"
cat "$datadir/gin-replay.log"

dd if="$datadir/$relpath" of="$datadir/gin-replace-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gin VALUES (3, to_tsvector('english', 'beta'))" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/gin-replace-final.page" bs=8192 skip=1 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-replace-records.txt"
grep 'rmgr: Gin' "$datadir/gin-replace-records.txt" |
    grep 'desc: INSERT isdata: F isleaf: T isdelete: T' >/dev/null
replay_case "$datadir/pg_wal" "$datadir/gin-replace-base.page" \
    "$datadir/gin-replace-replayed.page" "1663/$dbid/$relid" "1" "$start" "$stop" \
    > "$datadir/gin-replace-replay.log"
grep -F 'applied GIN INSERT' "$datadir/gin-replace-replay.log" >/dev/null
compare_index_pages "$datadir/gin-replace-replayed.page" "$datadir/gin-replace-final.page"
echo 'GIN entry-tuple replacement increment matches'

sql 'CREATE TABLE replay_gin_pending (id integer, words tsvector) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_gin_pending_idx ON replay_gin_pending USING gin (words)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_gin_pending_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_gin_pending_idx')")
cp "$datadir/$relpath" "$datadir/gin-pending-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gin_pending SELECT n, to_tsvector('english', 'word' || n::text) FROM generate_series(1, 100) AS n" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-pending-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-pending-records.txt"
pending_record=$(grep 'rmgr: Gin' "$datadir/gin-pending-records.txt" |
    grep 'INSERT_LISTPAGE' | head -1)
test -n "$pending_record"
pending_blk=$(printf '%s\n' "$pending_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$pending_blk"
make_blank_page "$datadir/gin-pending-new-base.page"
dd if="$datadir/gin-pending-final.rel" of="$datadir/gin-pending-new-final.page" bs=8192 skip="$pending_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gin-pending-new-base.page" \
    "$datadir/gin-pending-new-replayed.page" "1663/$dbid/$relid" "$pending_blk" "$start" "$stop" \
    > "$datadir/gin-pending-new-replay.log"
grep -F 'applied GIN INSERT_LISTPAGE' "$datadir/gin-pending-new-replay.log" >/dev/null
compare_index_pages "$datadir/gin-pending-new-replayed.page" "$datadir/gin-pending-new-final.page"
tail -3 "$datadir/gin-pending-new-replay.log"
dd if="$datadir/gin-pending-base.rel" of="$datadir/gin-pending-meta-base.page" bs=8192 count=1 status=none
dd if="$datadir/gin-pending-final.rel" of="$datadir/gin-pending-meta-final.page" bs=8192 count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gin-pending-meta-base.page" \
    "$datadir/gin-pending-meta-replayed.page" "1663/$dbid/$relid" "0" "$start" "$stop" \
    > "$datadir/gin-pending-meta-replay.log"
grep -F 'applied GIN UPDATE_META_PAGE' "$datadir/gin-pending-meta-replay.log" >/dev/null
compare_index_pages "$datadir/gin-pending-meta-replayed.page" "$datadir/gin-pending-meta-final.page"
tail -3 "$datadir/gin-pending-meta-replay.log"

sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-clean-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "SELECT gin_clean_pending_list('replay_gin_pending_idx'::regclass)" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'SELECT pg_switch_wal()' >/dev/null
mkdir "$datadir/gin-clean-wal"
cp "$datadir"/pg_wal/0000000* "$datadir/gin-clean-wal/"
"$bindir/pg_ctl" -D "$datadir" stop -m immediate >/dev/null
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=off -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-clean-final.rel"
"$bindir/pg_waldump" -p "$datadir/gin-clean-wal" -s "$start" -e "$stop" > "$datadir/gin-clean-records.txt"
clean_record=$(grep 'rmgr: Gin' "$datadir/gin-clean-records.txt" |
    grep 'DELETE_LISTPAGE' | head -1)
test -n "$clean_record"
for blk in 0 "$pending_blk"; do
    dd if="$datadir/gin-clean-base.rel" of="$datadir/gin-clean-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/gin-clean-final.rel" of="$datadir/gin-clean-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/gin-clean-wal" "$datadir/gin-clean-base-$blk.page" \
        "$datadir/gin-clean-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/gin-clean-replay-$blk.log"
    grep -F 'applied GIN DELETE_LISTPAGE' "$datadir/gin-clean-replay-$blk.log" >/dev/null
    compare_index_pages "$datadir/gin-clean-replayed-$blk.page" "$datadir/gin-clean-final-$blk.page"
    tail -3 "$datadir/gin-clean-replay-$blk.log"
done

# Posting-tree leaf WAL carries compressed segment edits rather than a full
# image.  Replay a long stream of ADDITEMS and a later vacuum recompression.
sql 'CREATE TABLE replay_gin_posting (id integer, tags text[]) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_gin_posting_idx ON replay_gin_posting USING gin(tags) WITH (fastupdate=off)' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_gin_posting_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_gin_posting_idx')")
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gin_posting SELECT n, ARRAY['same'] FROM generate_series(1,4000) AS n" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-ptree-records.txt"
ptree_record=$(grep 'rmgr: Gin' "$datadir/gin-ptree-records.txt" |
    grep 'desc: CREATE_PTREE' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$ptree_record"
ptree_blk=$(printf '%s\n' "$ptree_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$ptree_blk"
dd if=/dev/zero of="$datadir/gin-ptree-base.page" bs=8192 count=1 status=none
dd if="$datadir/$relpath" of="$datadir/gin-ptree-final.page" bs=8192 skip="$ptree_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gin-ptree-base.page" \
    "$datadir/gin-ptree-replayed.page" "1663/$dbid/$relid" "$ptree_blk" "$start" "$stop" \
    > "$datadir/gin-ptree-replay.log"
grep -F 'applied GIN CREATE_PTREE' "$datadir/gin-ptree-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/gin-ptree-replay.log" >/dev/null; then
    echo 'GIN posting-tree creation replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/gin-ptree-replayed.page" "$datadir/gin-ptree-final.page"
echo 'GIN CREATE_PTREE and subsequent page increments match'

cp "$datadir/$relpath" "$datadir/gin-posting-base-index"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gin_posting SELECT n, ARRAY['same'] FROM generate_series(4001,5000) AS n" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-posting-final-index"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-posting-records.txt"
gin_posting_record=$(grep 'rmgr: Gin' "$datadir/gin-posting-records.txt" |
    grep 'desc: INSERT isdata: T isleaf: T' | head -1)
test -n "$gin_posting_record"
gin_posting_blk=$(printf '%s\n' "$gin_posting_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$gin_posting_blk"
dd if="$datadir/gin-posting-base-index" of="$datadir/gin-posting-base.page" \
    bs=8192 skip="$gin_posting_blk" count=1 status=none
dd if="$datadir/gin-posting-final-index" of="$datadir/gin-posting-final.page" \
    bs=8192 skip="$gin_posting_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gin-posting-base.page" \
    "$datadir/gin-posting-replayed.page" "1663/$dbid/$relid" "$gin_posting_blk" "$start" "$stop" \
    > "$datadir/gin-posting-replay.log"
grep -F 'applied GIN INSERT' "$datadir/gin-posting-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/gin-posting-replay.log" >/dev/null; then
    echo 'GIN posting-tree insert replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/gin-posting-replayed.page" "$datadir/gin-posting-final.page"
echo 'GIN compressed posting-list insert increments match'

cp "$datadir/$relpath" "$datadir/gin-posting-vacuum-base-index"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_gin_posting WHERE id % 2 = 0' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_gin_posting' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-posting-vacuum-final-index"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-posting-vacuum-records.txt"
gin_vacuum_record=$(grep 'rmgr: Gin' "$datadir/gin-posting-vacuum-records.txt" |
    grep 'VACUUM_DATA_LEAF_PAGE' | head -1)
test -n "$gin_vacuum_record"
gin_vacuum_blk=$(printf '%s\n' "$gin_vacuum_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$gin_vacuum_blk"
dd if="$datadir/gin-posting-vacuum-base-index" of="$datadir/gin-posting-vacuum-base.page" \
    bs=8192 skip="$gin_vacuum_blk" count=1 status=none
dd if="$datadir/gin-posting-vacuum-final-index" of="$datadir/gin-posting-vacuum-final.page" \
    bs=8192 skip="$gin_vacuum_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gin-posting-vacuum-base.page" \
    "$datadir/gin-posting-vacuum-replayed.page" "1663/$dbid/$relid" "$gin_vacuum_blk" "$start" "$stop" \
    > "$datadir/gin-posting-vacuum-replay.log"
grep -F 'applied GIN VACUUM_DATA_LEAF_PAGE' "$datadir/gin-posting-vacuum-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/gin-posting-vacuum-replay.log" >/dev/null; then
    echo 'GIN posting-tree vacuum replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/gin-posting-vacuum-replayed.page" "$datadir/gin-posting-vacuum-final.page"
echo 'GIN compressed posting-list vacuum increment matches'

# Force posting-tree leaf splits, then verify the incremental insertion of a
# new child pointer into an already existing internal page.
sql "ALTER SYSTEM SET wal_keep_size = '128MB'" >/dev/null
sql 'SELECT pg_reload_conf()' >/dev/null
sql "INSERT INTO replay_gin_posting SELECT n, ARRAY['same'] FROM generate_series(5001,20000) AS n" >/dev/null
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-internal-base-index"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gin_posting SELECT n, ARRAY['same'] FROM generate_series(20001,35000) AS n" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-internal-final-index"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-internal-records.txt"
gin_internal_record=$(grep 'rmgr: Gin' "$datadir/gin-internal-records.txt" |
    grep 'desc: INSERT isdata: T isleaf: F' | head -1)
test -n "$gin_internal_record"
gin_internal_blk=$(printf '%s\n' "$gin_internal_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$gin_internal_blk"
dd if="$datadir/gin-internal-base-index" of="$datadir/gin-internal-base.page" \
    bs=8192 skip="$gin_internal_blk" count=1 status=none
dd if="$datadir/gin-internal-final-index" of="$datadir/gin-internal-final.page" \
    bs=8192 skip="$gin_internal_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gin-internal-base.page" \
    "$datadir/gin-internal-replayed.page" "1663/$dbid/$relid" "$gin_internal_blk" "$start" "$stop" \
    > "$datadir/gin-internal-replay.log"
grep -F 'applied GIN INSERT' "$datadir/gin-internal-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/gin-internal-replay.log" >/dev/null; then
    echo 'GIN internal posting-tree insertion replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/gin-internal-replayed.page" "$datadir/gin-internal-final.page"
echo 'GIN internal posting-tree increment matches'

# Remove a middle span of a large posting tree.  VACUUM must unlink empty
# middle pages; replay all three references from one DELETE_PAGE record.
cp "$datadir/$relpath" "$datadir/gin-delete-base-index"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_gin_posting WHERE id BETWEEN 7000 AND 28000' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_gin_posting' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-delete-final-index"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-delete-records.txt"
gin_delete_record=$(grep 'rmgr: Gin' "$datadir/gin-delete-records.txt" |
    grep 'desc: DELETE_PAGE' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$gin_delete_record"
for ref in 0 1 2; do
    gin_delete_blk=$(printf '%s\n' "$gin_delete_record" |
        sed -n "s/.*blkref #$ref: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p")
    test -n "$gin_delete_blk"
    dd if="$datadir/gin-delete-base-index" of="$datadir/gin-delete-base-$ref.page" \
        bs=8192 skip="$gin_delete_blk" count=1 status=none
    dd if="$datadir/gin-delete-final-index" of="$datadir/gin-delete-final-$ref.page" \
        bs=8192 skip="$gin_delete_blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/gin-delete-base-$ref.page" \
        "$datadir/gin-delete-replayed-$ref.page" "1663/$dbid/$relid" "$gin_delete_blk" "$start" "$stop" \
        > "$datadir/gin-delete-replay-$ref.log"
    grep -F 'applied GIN DELETE_PAGE' "$datadir/gin-delete-replay-$ref.log" |
        grep -F "block_ref=$ref" >/dev/null
    if grep -F 'applied FPI' "$datadir/gin-delete-replay-$ref.log" >/dev/null; then
        echo 'GIN page deletion replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/gin-delete-replayed-$ref.page" "$datadir/gin-delete-final-$ref.page"
done
echo 'GIN DELETE_PAGE references match'

sql 'CREATE TABLE replay_spgist (id integer, pos point) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_spgist_idx ON replay_spgist USING spgist (pos)' >/dev/null
sql 'INSERT INTO replay_spgist VALUES (1, point(1,1))' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_spgist_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_spgist_idx')")
dd if="$datadir/$relpath" of="$datadir/spgist-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_spgist VALUES (2, point(2,2))' >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/spgist-final.page" bs=8192 skip=1 count=1 status=none
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/spgist-records.txt"
spgist_record=$(grep 'rmgr: SPGist' "$datadir/spgist-records.txt" | grep 'ADD_LEAF' |
    grep -F "rel 1663/$dbid/$relid blk 1" | head -1)
test -n "$spgist_record"
spgist_lsn=$(printf '%s\n' "$spgist_record" | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/spgist-base.page" \
    "$datadir/spgist-replayed.page" "1663/$dbid/$relid" "1" "$start" "$spgist_lsn" \
    > "$datadir/spgist-replay.log"
grep -F 'applied SP-GiST ADD_LEAF' "$datadir/spgist-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/spgist-replay.log"; then
    echo 'SP-GiST test replayed an FPI instead of the increment' >&2
    exit 1
fi
compare_index_pages "$datadir/spgist-replayed.page" "$datadir/spgist-final.page"
cat "$datadir/spgist-replay.log"

sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/spgist-vacuum-base.page" bs=8192 skip=1 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_spgist WHERE id = 1' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_spgist' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/spgist-vacuum-final.page" bs=8192 skip=1 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/spgist-vacuum-records.txt"
grep 'rmgr: SPGist' "$datadir/spgist-vacuum-records.txt" |
    grep 'VACUUM_ROOT' | grep -F "rel 1663/$dbid/$relid blk 1" >/dev/null
replay_case "$datadir/pg_wal" "$datadir/spgist-vacuum-base.page" \
    "$datadir/spgist-vacuum-replayed.page" "1663/$dbid/$relid" 1 "$start" "$stop" \
    > "$datadir/spgist-vacuum-replay.log"
grep -F 'applied SP-GiST VACUUM_ROOT' "$datadir/spgist-vacuum-replay.log" >/dev/null
compare_index_pages "$datadir/spgist-vacuum-replayed.page" "$datadir/spgist-vacuum-final.page"
tail -3 "$datadir/spgist-vacuum-replay.log"

# A single insertion batch makes leaf chains, moves, and splits.  Replay
# every index block, including newly initialized blocks, from the same WAL
# interval.  A saved initialized page is used as the input for new blocks;
# the first WAL record for each of those blocks reinitializes it.
sql 'CREATE TABLE replay_spgist_stress (id integer, pos point) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_spgist_stress_idx ON replay_spgist_stress USING spgist (pos)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_spgist_stress_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_spgist_stress_idx')")
cp "$datadir/$relpath" "$datadir/spgist-stress-base-index"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_spgist_stress SELECT n, point(random()*100, random()*100) FROM generate_series(1,3000) AS n' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/spgist-stress-final-index"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/spgist-stress-records.txt"
grep -F 'desc: PICKSPLIT' "$datadir/spgist-stress-records.txt" >/dev/null
grep -F 'desc: MOVE_LEAFS' "$datadir/spgist-stress-records.txt" >/dev/null
base_blocks=$(( $(wc -c < "$datadir/spgist-stress-base-index") / 8192 ))
final_blocks=$(( $(wc -c < "$datadir/spgist-stress-final-index") / 8192 ))
dd if=/dev/zero of="$datadir/spgist-stress-zero.page" bs=8192 count=1 status=none
: > "$datadir/spgist-stress-all-replay.log"
blk=1
while test "$blk" -lt "$final_blocks"; do
    if test "$blk" -lt "$base_blocks"; then
        dd if="$datadir/spgist-stress-base-index" of="$datadir/spgist-stress-base-$blk.page" \
            bs=8192 skip="$blk" count=1 status=none
    else
        make_blank_page "$datadir/spgist-stress-base-$blk.page"
    fi
    dd if="$datadir/spgist-stress-final-index" of="$datadir/spgist-stress-final-$blk.page" \
        bs=8192 skip="$blk" count=1 status=none
    if cmp -s "$datadir/spgist-stress-final-$blk.page" "$datadir/spgist-stress-zero.page"; then
        blk=$((blk + 1))
        continue
    fi
    replay_case "$datadir/pg_wal" "$datadir/spgist-stress-base-$blk.page" \
        "$datadir/spgist-stress-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/spgist-stress-replay-$blk.log"
    cat "$datadir/spgist-stress-replay-$blk.log" >> "$datadir/spgist-stress-all-replay.log"
    compare_index_pages "$datadir/spgist-stress-replayed-$blk.page" "$datadir/spgist-stress-final-$blk.page"
    blk=$((blk + 1))
done
grep -F 'applied SP-GiST PICKSPLIT' "$datadir/spgist-stress-all-replay.log" >/dev/null
grep -F 'applied SP-GiST MOVE_LEAFS' "$datadir/spgist-stress-all-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/spgist-stress-all-replay.log" >/dev/null; then
    echo 'SP-GiST stress replayed a full-page image instead of increments' >&2
    exit 1
fi
echo "SP-GiST split and move increments match across $((final_blocks - 1)) pages"

cp "$datadir/$relpath" "$datadir/spgist-vacuum-stress-base-index"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_spgist_stress WHERE id % 2 = 0' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_spgist_stress' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/spgist-vacuum-stress-final-index"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/spgist-vacuum-stress-records.txt"
grep -F 'desc: VACUUM_LEAF' "$datadir/spgist-vacuum-stress-records.txt" >/dev/null
grep -F 'desc: VACUUM_REDIRECT' "$datadir/spgist-vacuum-stress-records.txt" >/dev/null
: > "$datadir/spgist-vacuum-stress-all-replay.log"
blk=1
while test "$blk" -lt "$final_blocks"; do
    dd if="$datadir/spgist-vacuum-stress-base-index" \
        of="$datadir/spgist-vacuum-stress-base-$blk.page" \
        bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/spgist-vacuum-stress-final-index" \
        of="$datadir/spgist-vacuum-stress-final-$blk.page" \
        bs=8192 skip="$blk" count=1 status=none
    if cmp -s "$datadir/spgist-vacuum-stress-final-$blk.page" "$datadir/spgist-stress-zero.page"; then
        blk=$((blk + 1))
        continue
    fi
    replay_case "$datadir/pg_wal" "$datadir/spgist-vacuum-stress-base-$blk.page" \
        "$datadir/spgist-vacuum-stress-replayed-$blk.page" \
        "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/spgist-vacuum-stress-replay-$blk.log"
    cat "$datadir/spgist-vacuum-stress-replay-$blk.log" >> "$datadir/spgist-vacuum-stress-all-replay.log"
    compare_index_pages "$datadir/spgist-vacuum-stress-replayed-$blk.page" \
        "$datadir/spgist-vacuum-stress-final-$blk.page"
    blk=$((blk + 1))
done
grep -F 'applied SP-GiST VACUUM_LEAF' "$datadir/spgist-vacuum-stress-all-replay.log" >/dev/null
grep -F 'applied SP-GiST VACUUM_REDIRECT' "$datadir/spgist-vacuum-stress-all-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/spgist-vacuum-stress-all-replay.log" >/dev/null; then
    echo 'SP-GiST vacuum replayed a full-page image instead of increments' >&2
    exit 1
fi
echo "SP-GiST vacuum increments match across $((final_blocks - 1)) pages"

# Text radix trees can split an existing inner tuple when a new value
# disagrees inside its prefix.  This also updates a separate parent downlink.
sql 'CREATE TABLE replay_spgist_text (id integer, k text) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_spgist_text_idx ON replay_spgist_text USING spgist(k)' >/dev/null
sql "INSERT INTO replay_spgist_text SELECT n, repeat('a',30) || lpad(n::text,6,'0') FROM generate_series(1,1000) AS n" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_spgist_text_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_spgist_text_idx')")
cp "$datadir/$relpath" "$datadir/spgist-text-base-index"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_spgist_text VALUES (1001, repeat('a',10) || 'b')" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/spgist-text-final-index"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/spgist-text-records.txt"
grep 'rmgr: SPGist' "$datadir/spgist-text-records.txt" | grep -F 'desc: SPLIT_TUPLE' >/dev/null
grep 'rmgr: SPGist' "$datadir/spgist-text-records.txt" | grep -F 'desc: ADD_NODE' >/dev/null
spgist_text_blocks=$(python3 - "$datadir/spgist-text-records.txt" "$dbid" "$relid" <<'PY'
import re
import sys

records = open(sys.argv[1]).read()
tag = rf'blkref #\d+: rel 1663/{sys.argv[2]}/{sys.argv[3]} blk (\d+)'
print(' '.join(map(str, sorted({int(x) for x in re.findall(tag, records)}))))
PY
)
test -n "$spgist_text_blocks"
base_blocks=$(( $(wc -c < "$datadir/spgist-text-base-index") / 8192 ))
: > "$datadir/spgist-text-all-replay.log"
for blk in $spgist_text_blocks; do
    if test "$blk" -lt "$base_blocks"; then
        dd if="$datadir/spgist-text-base-index" of="$datadir/spgist-text-base-$blk.page" \
            bs=8192 skip="$blk" count=1 status=none
    else
        make_blank_page "$datadir/spgist-text-base-$blk.page"
    fi
    dd if="$datadir/spgist-text-final-index" of="$datadir/spgist-text-final-$blk.page" \
        bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/spgist-text-base-$blk.page" \
        "$datadir/spgist-text-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/spgist-text-replay-$blk.log"
    cat "$datadir/spgist-text-replay-$blk.log" >> "$datadir/spgist-text-all-replay.log"
    compare_index_pages "$datadir/spgist-text-replayed-$blk.page" "$datadir/spgist-text-final-$blk.page"
done
grep -F 'applied SP-GiST SPLIT_TUPLE' "$datadir/spgist-text-all-replay.log" >/dev/null
grep -F 'applied SP-GiST ADD_NODE' "$datadir/spgist-text-all-replay.log" >/dev/null
grep -F 'applied SP-GiST ADD_LEAF' "$datadir/spgist-text-all-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/spgist-text-all-replay.log" >/dev/null; then
    echo 'SP-GiST text split replayed an FPI' >&2
    exit 1
fi
echo 'SP-GiST text prefix split increments match'

sql 'CREATE TABLE replay_brin (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_brin SELECT generate_series(1, 300)' >/dev/null
sql 'CREATE INDEX replay_brin_idx ON replay_brin USING brin (id) WITH (pages_per_range=1, autosummarize=off)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_brin_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_brin_idx')")
cp "$datadir/$relpath" "$datadir/brin-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_brin SELECT generate_series(301, 600)' >/dev/null
sql "SELECT brin_summarize_new_values('replay_brin_idx')" >/dev/null
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/brin-final.rel"
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/brin-records.txt"
grep 'rmgr: BRIN' "$datadir/brin-records.txt" | grep 'SAMEPAGE_UPDATE' |
    grep -F "rel 1663/$dbid/$relid blk 2" >/dev/null
grep 'rmgr: BRIN' "$datadir/brin-records.txt" | grep 'desc: INSERT ' |
    grep -F "rel 1663/$dbid/$relid blk 2" >/dev/null
for blk in 2 1; do
    dd if="$datadir/brin-base.rel" of="$datadir/brin-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/brin-final.rel" of="$datadir/brin-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    last_lsn=$(grep -F "rel 1663/$dbid/$relid blk $blk" "$datadir/brin-records.txt" |
        tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
    replay_case "$datadir/pg_wal" "$datadir/brin-base-$blk.page" \
        "$datadir/brin-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$last_lsn" \
        > "$datadir/brin-replay-$blk.log"
    grep -F 'applied BRIN INSERT' "$datadir/brin-replay-$blk.log" >/dev/null
    if grep -q 'applied FPI' "$datadir/brin-replay-$blk.log"; then
        echo 'BRIN test replayed an FPI instead of the increment' >&2
        exit 1
    fi
    compare_index_pages "$datadir/brin-replayed-$blk.page" "$datadir/brin-final-$blk.page"
    tail -3 "$datadir/brin-replay-$blk.log"
done

# BRIN de-summarization changes both the revmap and the regular page.
cp "$datadir/$relpath" "$datadir/brin-desum-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "SELECT brin_desummarize_range('replay_brin_idx', 0)" >/dev/null
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/brin-desum-final.rel"
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/brin-desum-records.txt"
grep 'rmgr: BRIN' "$datadir/brin-desum-records.txt" | grep 'DESUMMARIZE' >/dev/null
for blk in 1 2; do
    dd if="$datadir/brin-desum-base.rel" of="$datadir/brin-desum-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/brin-desum-final.rel" of="$datadir/brin-desum-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    desum_lsn=$(grep 'rmgr: BRIN' "$datadir/brin-desum-records.txt" |
        grep 'DESUMMARIZE' | grep -E "rel 1663/$dbid/$relid blk $blk(,|$)" | tail -1 |
        sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
    test -n "$desum_lsn"
    replay_case "$datadir/pg_wal" "$datadir/brin-desum-base-$blk.page" \
        "$datadir/brin-desum-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$desum_lsn" \
        > "$datadir/brin-desum-replay-$blk.log"
    grep -F 'applied BRIN DESUMMARIZE' "$datadir/brin-desum-replay-$blk.log" >/dev/null
    if grep -q 'applied FPI' "$datadir/brin-desum-replay-$blk.log"; then
        echo 'BRIN DESUMMARIZE replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/brin-desum-replayed-$blk.page" "$datadir/brin-desum-final-$blk.page"
    tail -2 "$datadir/brin-desum-replay-$blk.log"
done

# BRIN metapage creation and revmap extension both initialize pages from WAL.
sql 'CREATE TABLE replay_brin_init (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_brin_init SELECT generate_series(1, 300)' >/dev/null
sql 'CHECKPOINT' >/dev/null
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CREATE INDEX replay_brin_init_idx ON replay_brin_init USING brin (id) WITH (pages_per_range=1)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_brin_init_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_brin_init_idx')")
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/brin-init-records.txt"
grep 'rmgr: BRIN' "$datadir/brin-init-records.txt" | grep 'CREATE_INDEX' |
    grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
grep 'rmgr: BRIN' "$datadir/brin-init-records.txt" | grep 'REVMAP_EXTEND' |
    grep -F "rel 1663/$dbid/$relid blk 1" >/dev/null
init_start=$(grep 'rmgr: BRIN' "$datadir/brin-init-records.txt" |
    grep 'CREATE_INDEX' | grep -F "rel 1663/$dbid/$relid blk 0" | head -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
make_blank_page "$datadir/brin-init-base.page"
for blk in 0 1; do
    dd if="$datadir/$relpath" of="$datadir/brin-init-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    last_lsn=$(grep -E "rel 1663/$dbid/$relid blk $blk(,|$)" "$datadir/brin-init-records.txt" |
        tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
    replay_case "$datadir/pg_wal" "$datadir/brin-init-base.page" \
        "$datadir/brin-init-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$init_start" "$last_lsn" \
        > "$datadir/brin-init-replay-$blk.log"
    if grep -q 'applied FPI' "$datadir/brin-init-replay-$blk.log"; then
        echo 'BRIN page initialization replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/brin-init-replayed-$blk.page" "$datadir/brin-init-final-$blk.page"
    tail -3 "$datadir/brin-init-replay-$blk.log"
done

# Grow one BRIN summary enough to move it to a different regular page.
sql 'CREATE TABLE replay_brin_move (id integer, value text) WITH (fillfactor=10, autovacuum_enabled=false)' >/dev/null
sql "INSERT INTO replay_brin_move SELECT n, repeat('a', 100) FROM generate_series(1, 3000) AS n" >/dev/null
sql 'CREATE INDEX replay_brin_move_idx ON replay_brin_move USING brin (value text_minmax_ops) WITH (pages_per_range=1)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_brin_move_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_brin_move_idx')")
cp "$datadir/$relpath" "$datadir/brin-move-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "UPDATE replay_brin_move SET value = (SELECT string_agg(md5(random()::text), '' ORDER BY n) FROM generate_series(1, 32) AS n) WHERE id = 1" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/brin-move-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/brin-move-records.txt"
move_record=$(grep 'rmgr: BRIN' "$datadir/brin-move-records.txt" |
    grep 'desc: UPDATE ' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$move_record"
move_lsn=$(printf '%s\n' "$move_record" |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
for ref in 0 1 2; do
    blk=$(printf '%s\n' "$move_record" |
        sed -n "s/.*blkref #$ref: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p")
    test -n "$blk"
    if test $(( (blk + 1) * 8192 )) -le "$(wc -c < "$datadir/brin-move-base.rel")"; then
        dd if="$datadir/brin-move-base.rel" of="$datadir/brin-move-base-$ref.page" bs=8192 skip="$blk" count=1 status=none
    else
        make_blank_page "$datadir/brin-move-base-$ref.page"
    fi
    dd if="$datadir/brin-move-final.rel" of="$datadir/brin-move-final-$ref.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/brin-move-base-$ref.page" \
        "$datadir/brin-move-replayed-$ref.page" "1663/$dbid/$relid" "$blk" "$start" "$move_lsn" \
        > "$datadir/brin-move-replay-$ref.log"
    grep -F 'applied BRIN UPDATE' "$datadir/brin-move-replay-$ref.log" >/dev/null
    if grep -q 'applied FPI' "$datadir/brin-move-replay-$ref.log"; then
        echo 'BRIN cross-page UPDATE replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/brin-move-replayed-$ref.page" "$datadir/brin-move-final-$ref.page"
    tail -2 "$datadir/brin-move-replay-$ref.log"
done

# Delete complete B-tree leaf pages and replay their half-dead/unlink WAL.
sql 'CREATE TABLE replay_btree_unlink (id integer PRIMARY KEY) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_btree_unlink SELECT generate_series(1, 5000)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_btree_unlink_pkey')")
relid=$(sql "SELECT pg_relation_filenode('replay_btree_unlink_pkey')")
cp "$datadir/$relpath" "$datadir/btree-unlink-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_btree_unlink' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_btree_unlink' >/dev/null
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/btree-unlink-final.rel"
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/btree-unlink-records.txt"
mark_record=$(grep 'rmgr: Btree' "$datadir/btree-unlink-records.txt" |
    grep 'MARK_PAGE_HALFDEAD' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$mark_record"
unlink_blk=$(printf '%s\n' "$mark_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$unlink_blk"
grep 'rmgr: Btree' "$datadir/btree-unlink-records.txt" |
    grep 'UNLINK_PAGE' | grep -F "rel 1663/$dbid/$relid blk $unlink_blk" >/dev/null
dd if="$datadir/btree-unlink-base.rel" of="$datadir/btree-unlink-base.page" bs=8192 skip="$unlink_blk" count=1 status=none
dd if="$datadir/btree-unlink-final.rel" of="$datadir/btree-unlink-final.page" bs=8192 skip="$unlink_blk" count=1 status=none
last_lsn=$(grep -E "rel 1663/$dbid/$relid blk $unlink_blk(,|$)" "$datadir/btree-unlink-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
replay_case "$datadir/pg_wal" "$datadir/btree-unlink-base.page" \
    "$datadir/btree-unlink-replayed.page" "1663/$dbid/$relid" "$unlink_blk" "$start" "$last_lsn" \
    > "$datadir/btree-unlink-replay.log"
grep -F 'applied Btree MARK_PAGE_HALFDEAD' "$datadir/btree-unlink-replay.log" >/dev/null
grep -F 'applied Btree UNLINK_PAGE' "$datadir/btree-unlink-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/btree-unlink-replay.log"; then
    echo 'B-tree page unlink replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/btree-unlink-replayed.page" "$datadir/btree-unlink-final.page"
tail -4 "$datadir/btree-unlink-replay.log"
parent_blk=$(printf '%s\n' "$mark_record" |
    sed -n 's/.*blkref #1: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
unlink_record=$(grep 'rmgr: Btree' "$datadir/btree-unlink-records.txt" |
    grep 'UNLINK_PAGE' | grep -E "blkref #0: rel 1663/$dbid/$relid blk $unlink_blk(,|$)" | head -1)
sibling_blk=$(printf '%s\n' "$unlink_record" |
    sed -n 's/.*blkref #2: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$parent_blk"
test -n "$sibling_blk"
for blk in "$parent_blk" "$sibling_blk"; do
    dd if="$datadir/btree-unlink-base.rel" of="$datadir/btree-unlink-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/btree-unlink-final.rel" of="$datadir/btree-unlink-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    last_lsn=$(grep -E "rel 1663/$dbid/$relid blk $blk(,|$)" "$datadir/btree-unlink-records.txt" |
        tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
    replay_case "$datadir/pg_wal" "$datadir/btree-unlink-base-$blk.page" \
        "$datadir/btree-unlink-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$last_lsn" \
        > "$datadir/btree-unlink-replay-$blk.log"
    if grep -q 'applied FPI' "$datadir/btree-unlink-replay-$blk.log"; then
        echo 'B-tree parent/sibling page replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/btree-unlink-replayed-$blk.page" "$datadir/btree-unlink-final-$blk.page"
    tail -2 "$datadir/btree-unlink-replay-$blk.log"
done

# Sequence WAL prelogs future values, so compare with PostgreSQL crash redo,
# not the live page immediately after nextval().
sql 'CREATE SEQUENCE replay_sequence' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_sequence')")
relid=$(sql "SELECT pg_relation_filenode('replay_sequence')")
dd if="$datadir/$relpath" of="$datadir/sequence-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "SELECT nextval('replay_sequence')" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/sequence-records.txt"
sequence_lsn=$(grep 'rmgr: Sequence' "$datadir/sequence-records.txt" |
    grep -F "rel 1663/$dbid/$relid blk 0" | tail -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$sequence_lsn"
replay_case "$datadir/pg_wal" "$datadir/sequence-base.page" \
    "$datadir/sequence-replayed.page" "1663/$dbid/$relid" "0" "$start" "$sequence_lsn" \
    > "$datadir/sequence-replay.log"
grep -F 'applied Sequence LOG' "$datadir/sequence-replay.log" >/dev/null
if grep -q 'applied FPI' "$datadir/sequence-replay.log"; then
    echo 'Sequence test replayed an FPI instead of the increment' >&2
    exit 1
fi
"$bindir/pg_ctl" -D "$datadir" stop -m immediate >/dev/null
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=off -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/sequence-final.page" bs=8192 count=1 status=none
compare_index_pages "$datadir/sequence-replayed.page" "$datadir/sequence-final.page"
cat "$datadir/sequence-replay.log"
