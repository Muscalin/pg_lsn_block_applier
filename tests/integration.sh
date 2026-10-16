#!/bin/sh
set -eu

version=$1
bindir=${PG_BINDIR:-/usr/lib/postgresql/$version/bin}
binary=${BINARY:-/tmp/pg_lsn_block_applier_$version}
datadir=/tmp/pg_lsn_block_applier_data_$version
rollover_datadir=/tmp/pg_lsn_block_applier_rollover_$version
rollover_started=0
port=$((55000 + version))

rm -rf "$datadir"
if [ "${PG_TEST_FORCE_CHECKSUMS:-0}" = 1 ]; then
    "$bindir/initdb" -D "$datadir" -A trust --no-sync --data-checksums \
        --wal-segsize="${PG_TEST_WAL_SEGSIZE:-16}" >/dev/null
else
    "$bindir/initdb" -D "$datadir" -A trust --no-sync \
        --wal-segsize="${PG_TEST_WAL_SEGSIZE:-16}" >/dev/null
fi
# Keep WAL from the start of this test so gaps after real page LSNs can be inspected.
printf "wal_keep_size = '128MB'\n" >> "$datadir/postgresql.conf"
"$bindir/pg_ctl" -D "$datadir" -o "-k /tmp -p $port -c fsync=off -c full_page_writes=off -c autovacuum=off" -l "$datadir/server.log" start >/dev/null
stop_test_clusters() {
    "$bindir/pg_ctl" -D "$datadir" stop -m immediate >/dev/null 2>&1 || true
    if [ "$rollover_started" -eq 1 ]; then
        "$bindir/pg_ctl" -D "$rollover_datadir" stop -m immediate >/dev/null 2>&1 || true
    fi
}
trap stop_test_clusters EXIT

sql() { "$bindir/psql" -X -qAt -h /tmp -p "$port" -d postgres -c "$1"; }

# Audit each warning against WAL. A non-hint target record in the skipped gap
# means the saved page is not a valid starting point for this replay case.
start_warnings="$datadir/start-warnings.log"
start_gap_audit="$datadir/start-gap-audit.log"
: > "$start_warnings"
: > "$start_gap_audit"
audit_start_gap() {
    python3 - "$bindir/pg_waldump" "$@" <<'PY'
import re
import struct
import subprocess
import sys
from pathlib import Path

waldump, *args = sys.argv[1:]
options = dict(arg[2:].split('=', 1) for arg in args
               if arg.startswith('--') and '=' in arg)
page = Path(options['base-page']).read_bytes()
lsn_hi, lsn_lo = struct.unpack_from('<II', page)
page_lsn = (lsn_hi << 32) | lsn_lo
start = options['start']
target = (options['rel'], int(options['block']))

# make_blank_page() deliberately has no LSN. Check the exact fixture bytes;
# its --start must be supplied by the test that checks the initializing record.
if page_lsn == 0:
    blank = bytearray(8192)
    struct.pack_into('<HHHH', blank, 12, 24, 8192, 8192, 8192 | 4)
    if page != blank:
        sys.exit(f'unverifiable --start gap: zero-LSN page is not the blank fixture: '
                 f'{options["base-page"]}')
    print(f'blank fixture: {options["base-page"]}')
    sys.exit(0)

page_lsn_text = f'{page_lsn >> 32:X}/{page_lsn & 0xffffffff:08X}'
# The test is also run with Python 3.6, which has no text/capture_output kwargs.
dump = subprocess.run([waldump, '-p', options['waldir'], '-s', page_lsn_text,
                       '-e', start], stdout=subprocess.PIPE,
                      stderr=subprocess.PIPE, universal_newlines=True)
if dump.returncode:
    sys.exit(f'cannot inspect --start gap for {options["base-page"]}: '
             f'{dump.stderr.strip()}')

block_ref = re.compile(r'blkref #\d+: rel (\d+/\d+/\d+) '
                       r'(?:(fork \w+) )?blk (\d+)')
target_records = []
for line in dump.stdout.splitlines():
    refs = block_ref.findall(line)
    if any((rel, int(block)) == target and fork in ('', 'fork main')
           for rel, fork, block in refs):
        target_records.append(line)

unexpected = [line for line in target_records
              if not ('rmgr: XLOG' in line and 'desc: FPI_FOR_HINT' in line)]
if unexpected:
    sys.exit(f'--start skips WAL for target {target} from {page_lsn_text} '
             f'to {start}:\n' + '\n'.join(unexpected))
if target_records:
    print(f'hint-only FPI gap: {options["base-page"]}')
else:
    print(f'no target records: {options["base-page"]}')
PY
}
run_applier() {
    applier_stderr="$datadir/applier-stderr.$$"
    if "$binary" "$@" 2> "$applier_stderr"; then
        if grep -q '^warning: base page LSN .* precedes --start .*; intervening WAL records may be skipped$' \
            "$applier_stderr"; then
            audit_start_gap "$@" >> "$start_gap_audit" || {
                cat "$applier_stderr" >&2
                rm -f "$applier_stderr"
                return 1
            }
        fi
        grep -E '^warning: base page LSN .* precedes --start .*; intervening WAL records may be skipped$' \
            "$applier_stderr" >> "$start_warnings" || true
        grep -Ev '^warning: base page LSN .* precedes --start .*; intervening WAL records may be skipped$' \
            "$applier_stderr" >&2 || true
        rm -f "$applier_stderr"
    else
        applier_status=$?
        cat "$applier_stderr" >&2
        rm -f "$applier_stderr"
        return "$applier_status"
    fi
}
report_start_warnings() {
    start_warning_count=$(wc -l < "$start_warnings" | tr -d '[:space:]')
    audit_count=$(wc -l < "$start_gap_audit" | tr -d '[:space:]')
    test "$start_warning_count" -eq "$audit_count" || {
        echo "--start gap audit count $audit_count differs from warning count $start_warning_count" >&2
        exit 1
    }
    no_target_count=$(grep -c '^no target records:' "$start_gap_audit" || true)
    hint_only_count=$(grep -c '^hint-only FPI gap:' "$start_gap_audit" || true)
    blank_count=$(grep -c '^blank fixture:' "$start_gap_audit" || true)
    test "$audit_count" -eq "$((no_target_count + hint_only_count + blank_count))" || {
        echo 'unclassified --start gap in audit log' >&2
        exit 1
    }
    printf '%s\n' "--start warnings: $audit_count; no target records: $no_target_count; hint-only FPI: $hint_only_count; blank fixtures: $blank_count; unexpected: 0 (details: $start_gap_audit)"
}

# Replay one relation block across the requested WAL interval.  Callers keep
# their own assertions about the records and the expected page contents.
replay_case() {
    run_applier --waldir="$1" --base-page="$2" --out="$3" --rel="$4" \
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

# The gap audit must reject a start that omits the HOT UPDATE/INSERT WAL for
# this target block, even though the normal replay case above starts earlier.
if audit_start_gap --waldir="$datadir/pg_wal" --base-page="$datadir/base.page" \
    --rel="1663/$dbid/$relid" --block=0 --start="$last_lsn" \
    > "$datadir/skipped-target.out" 2> "$datadir/skipped-target.err"; then
    echo 'gap audit accepted skipped target WAL' >&2
    exit 1
fi
grep -q -- '--start skips WAL for target' "$datadir/skipped-target.err"
echo 'gap audit rejects skipped target WAL'

"$binary" --help > "$datadir/help.txt"
"$binary" --version > "$datadir/version.txt"
grep -q -- '--base-page' "$datadir/help.txt"
grep -q -- '--start=2A9C/51FB7280' "$datadir/help.txt"
grep -q 'pg_lsn_block_applier' "$datadir/version.txt"

# Invalid CLI values must fail before replay and must never create a page.
expect_cli_error() {
    case_name=$1
    expected=$2
    shift 2
    out="$datadir/cli-$case_name.page"
    if "$binary" --waldir="$datadir/pg_wal" --base-page="$datadir/base.page" \
        --out="$out" --rel="1663/$dbid/$relid" --block=0 \
        --stop="$last_lsn" "$@" \
        > "$datadir/cli-$case_name.out" 2> "$datadir/cli-$case_name.err"; then
        echo "invalid CLI case $case_name unexpectedly succeeded" >&2
        exit 1
    fi
    grep -F -- "$expected" "$datadir/cli-$case_name.err" >/dev/null
    test ! -e "$out"
}
expect_cli_error rel_short 'invalid --rel value:' "--rel=1663/$dbid"
expect_cli_error rel_suffix 'invalid --rel value:' "--rel=1663/$dbid/${relid}junk"
expect_cli_error block_text 'invalid --block value:' '--block=abc'
expect_cli_error block_suffix 'invalid --block value:' '--block=0junk'
expect_cli_error start_lsn 'invalid --start LSN:' '--start=not-an-lsn'
expect_cli_error start_hex_prefix 'invalid --start LSN:' '--start=0x1A/0'
expect_cli_error stop_lsn 'invalid --stop LSN:' '--stop=0/nothex'
expect_cli_error tli_zero '--tli must be greater than zero' '--tli=0'
expect_cli_error reversed_lsn '--stop must be >= --start' \
    "--start=$last_lsn" "--stop=$start"
echo 'nine invalid CLI cases rejected without output pages'

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
for event in events[:-1]:
    assert (event['event'] == 'applied' and event['lsn'] and event['end_lsn']
            and isinstance(event['rmid'], int) and isinstance(event['info'], int)
            and event['operation']), event
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

# An explicit timeline must select the same WAL as the default.  A different
# timeline and a directory without a segment for the requested LSN must fail
# before creating an output page.
"$binary" --waldir="$datadir/pg_wal" --base-page="$datadir/base.page" \
    --out="$datadir/tli-replayed.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" --tli=1 > "$datadir/tli-replay.log"
cmp "$datadir/tli-replayed.page" "$datadir/json-replayed.page"
if "$binary" --waldir="$datadir/pg_wal" --base-page="$datadir/base.page" \
    --out="$datadir/wrong-tli.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" --tli=2 \
    > "$datadir/wrong-tli.out" 2> "$datadir/wrong-tli.err"; then
    echo 'replay unexpectedly accepted WAL from another timeline' >&2
    exit 1
fi
grep -q 'could not find a valid WAL segment on timeline 2 containing start LSN' \
    "$datadir/wrong-tli.err"
test ! -e "$datadir/wrong-tli.page"

mkdir "$datadir/no-wal"
printf '%s\n' 'not a WAL segment' > "$datadir/no-wal/README"
# Use a real long page header as a template: one fake has the wrong magic,
# and the other has a valid header whose page address disagrees with its name.
header_source=$(sql "SELECT pg_walfile_name('$start'::pg_lsn)")
python3 - "$datadir/pg_wal/$header_source" "$datadir/no-wal" <<'PY'
import struct
import sys
from pathlib import Path

with open(sys.argv[1], 'rb') as source:
    header = bytearray(source.read(8192))
assert len(header) == 8192
assert struct.unpack_from('<H', header, 0)[0] != 0
segment_size = struct.unpack_from('<I', header, 32)[0]
assert segment_size > 0
assert struct.unpack_from('<Q', header, 8)[0] != 0xF1 * segment_size
wrong_magic = bytearray(header)
struct.pack_into('<H', wrong_magic, 0, 0)
directory = Path(sys.argv[2])
(directory / '0000000100000000000000F0').write_bytes(wrong_magic)
(directory / '0000000100000000000000F1').write_bytes(header)
PY
if "$binary" --waldir="$datadir/no-wal" --base-page="$datadir/base.page" \
    --out="$datadir/no-wal.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" \
    > "$datadir/no-wal.out" 2> "$datadir/no-wal.err"; then
    echo 'replay unexpectedly accepted a directory without WAL' >&2
    exit 1
fi
grep -q 'could not find a valid WAL segment on timeline 1 containing start LSN' \
    "$datadir/no-wal.err"
test ! -e "$datadir/no-wal.page"

# Header-based segment-size detection must skip ordinary files, suffixes,
# directories, and a segment-shaped filename with a short header.
mkdir "$datadir/mixed-wal" "$datadir/mixed-wal/archive_status"
printf '%s\n' 'not WAL' > "$datadir/mixed-wal/README"
printf '%s\n' 'not WAL' > "$datadir/mixed-wal/000000010000000000000000"
printf '%s\n' 'not WAL' > "$datadir/mixed-wal/000000010000000000000001.partial"
for segment in "$datadir"/pg_wal/????????????????????????; do
    test -f "$segment" || continue
    ln -s "$segment" "$datadir/mixed-wal/$(basename "$segment")"
done
"$binary" --waldir="$datadir/mixed-wal" --base-page="$datadir/base.page" \
    --out="$datadir/mixed-wal-replayed.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" > "$datadir/mixed-wal-replay.log"
cmp "$datadir/mixed-wal-replayed.page" "$datadir/json-replayed.page"

# The first WAL segment is valid, but the next one is absent from the archive.
# A real switch and INSERT ensure that replay must open the missing segment.
sql 'CREATE TABLE replay_wal_gap (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_wal_gap VALUES (1)' >/dev/null
sql 'CHECKPOINT' >/dev/null
gap_relpath=$(sql "SELECT pg_relation_filepath('replay_wal_gap')")
gap_relid=$(sql "SELECT pg_relation_filenode('replay_wal_gap')")
dd if="$datadir/$gap_relpath" of="$datadir/wal-gap-base.page" \
    bs=8192 count=1 status=none
gap_start=$(sql 'SELECT pg_current_wal_insert_lsn()')
gap_start_segment=$(sql "SELECT pg_walfile_name('$gap_start'::pg_lsn)")
sql 'SELECT pg_switch_wal()' >/dev/null
sql 'INSERT INTO replay_wal_gap VALUES (2)' >/dev/null
gap_stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$gap_relpath" of="$datadir/wal-gap-final.page" \
    bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$gap_start" -e "$gap_stop" \
    > "$datadir/wal-gap-records.txt"
gap_insert_lsn=$(grep 'rmgr: Heap' "$datadir/wal-gap-records.txt" |
    grep 'desc: INSERT ' | grep -F "rel 1663/$dbid/$gap_relid blk 0" | tail -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$gap_insert_lsn"
gap_missing_segment=$(sql "SELECT pg_walfile_name('$gap_insert_lsn'::pg_lsn)")
test "$gap_start_segment" != "$gap_missing_segment"
replay_case "$datadir/pg_wal" "$datadir/wal-gap-base.page" \
    "$datadir/wal-gap-complete.page" "1663/$dbid/$gap_relid" 0 \
    "$gap_start" "$gap_insert_lsn" > "$datadir/wal-gap-complete.log"
compare_pages "$datadir/wal-gap-complete.page" "$datadir/wal-gap-final.page"
mkdir "$datadir/wal-gap-archive"
ln -s "$datadir/pg_wal/$gap_start_segment" \
    "$datadir/wal-gap-archive/$gap_start_segment"
if "$binary" --waldir="$datadir/wal-gap-archive" \
    --base-page="$datadir/wal-gap-base.page" \
    --out="$datadir/wal-gap-missing.page" --rel="1663/$dbid/$gap_relid" \
    --block=0 --start="$gap_start" --stop="$gap_insert_lsn" \
    > "$datadir/wal-gap-missing.out" 2> "$datadir/wal-gap-missing.err"; then
    echo 'replay unexpectedly crossed a missing WAL segment' >&2
    exit 1
fi
grep -F "could not open WAL file \"$datadir/wal-gap-archive/$gap_missing_segment\"" \
    "$datadir/wal-gap-missing.err" >/dev/null
test ! -e "$datadir/wal-gap-missing.page"
echo 'missing WAL segment inside replay interval detected'

# Clear the input checksum so the option is exercised on every PG version,
# including versions whose initdb enables checksums by default.
python3 - "$datadir/base.page" "$datadir/no-checksum-base.page" <<'PY'
import sys
from pathlib import Path

page = bytearray(Path(sys.argv[1]).read_bytes())
page[8:10] = b'\0\0'
Path(sys.argv[2]).write_bytes(page)
PY
"$binary" --waldir="$datadir/pg_wal" --base-page="$datadir/no-checksum-base.page" \
    --out="$datadir/no-checksum-replayed.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" > "$datadir/no-checksum-replay.log"
"$binary" --waldir="$datadir/pg_wal" --base-page="$datadir/no-checksum-base.page" \
    --out="$datadir/checksum-replayed.page" --rel="1663/$dbid/$relid" \
    --block=0 --stop="$last_lsn" --data-checksums \
    > "$datadir/checksum-replay.log"
python3 - "$datadir/no-checksum-replayed.page" "$datadir/checksum-replayed.page" <<'PY'
import struct
import sys
from pathlib import Path

plain, checked = (bytearray(Path(path).read_bytes()) for path in sys.argv[1:])
assert len(plain) == len(checked) == 8192
stored = struct.unpack_from('<H', checked, 8)[0]
assert struct.unpack_from('<H', plain, 8)[0] == 0
checked[8:10] = b'\0\0'
assert plain == checked, 'checksum option changed more than pd_checksum'

# PostgreSQL's 32-lane checksum algorithm from storage/checksum_impl.h.
offsets = [
    0x5B1F36E9, 0xB8525960, 0x02AB50AA, 0x1DE66D2A,
    0x79FF467A, 0x9BB9F8A3, 0x217E7CD2, 0x83E13D2C,
    0xF8D4474F, 0xE39EB970, 0x42C6AE16, 0x993216FA,
    0x7B093B5D, 0x98DAFF3C, 0xF718902A, 0x0B1C9CDB,
    0xE58F764B, 0x187636BC, 0x5D7B3BB1, 0xE73DE7DE,
    0x92BEC979, 0xCCA6C0B2, 0x304A0979, 0x85AA43D4,
    0x783125BB, 0x6CA8EAA2, 0xE407EAC6, 0x4B5CFC3E,
    0x9FBF8C76, 0x15CA20BE, 0xF2CA9FD3, 0x959BD756,
]
for n, word in enumerate(struct.unpack('<2048I', checked)):
    lane = n % 32
    value = offsets[lane] ^ word
    offsets[lane] = ((value * 16777619) & 0xffffffff) ^ (value >> 17)
for _ in range(2):
    for lane, value in enumerate(offsets):
        offsets[lane] = ((value * 16777619) & 0xffffffff) ^ (value >> 17)
checksum = 0
for value in offsets:
    checksum ^= value
assert stored == checksum % 65535 + 1, (stored, checksum % 65535 + 1)
print('timeline, missing and unrelated WAL files, and output checksum verified')
PY

# A stale page cannot be replayed across creation of a relation with the
# requested file locator.  The saved page stands in for an old incarnation.
boundary_start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CREATE TABLE replay_boundary_test (payload text) WITH (autovacuum_enabled=false)' >/dev/null
boundary_relid=$(sql "SELECT pg_relation_filenode('replay_boundary_test')")
boundary_relpath=$(sql "SELECT pg_relation_filepath('replay_boundary_test')")
sql 'CHECKPOINT' >/dev/null
boundary_stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$boundary_start" -e "$boundary_stop" \
    > "$datadir/boundary-create-records.txt"
create_lsn=$(grep -F "desc: CREATE base/$dbid/$boundary_relid" \
    "$datadir/boundary-create-records.txt" | head -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$create_lsn"
if "$binary" --waldir="$datadir/pg_wal" --base-page="$datadir/base.page" \
    --out="$datadir/recreated.page" --rel="1663/$dbid/$boundary_relid" \
    --block=0 --stop="$create_lsn" \
    > "$datadir/recreated.out" 2> "$datadir/recreated.err"; then
    echo 'replay crossed relation creation' >&2
    exit 1
fi
grep -F "target relation was recreated at LSN $create_lsn" \
    "$datadir/recreated.err" >/dev/null
test ! -e "$datadir/recreated.page"

# VACUUM truncates a heap file without changing its relfilenode.  Select a
# block beyond the resulting EOF and require a specific truncation error.
sql "INSERT INTO replay_boundary_test SELECT repeat('x', 500) FROM generate_series(1, 5000)" >/dev/null
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$boundary_relpath" of="$datadir/truncated-base.page" \
    bs=8192 skip=1 count=1 status=none
test "$(wc -c < "$datadir/truncated-base.page")" -eq 8192
truncate_start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'DELETE FROM replay_boundary_test' >/dev/null
sql 'VACUUM (TRUNCATE ON) replay_boundary_test' >/dev/null
sql 'CHECKPOINT' >/dev/null
truncate_stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$truncate_start" -e "$truncate_stop" \
    > "$datadir/boundary-truncate-records.txt"
truncate_lsn=$(grep -F "desc: TRUNCATE base/$dbid/$boundary_relid to" \
    "$datadir/boundary-truncate-records.txt" | tail -1 |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$truncate_lsn"
if "$binary" --waldir="$datadir/pg_wal" \
    --base-page="$datadir/truncated-base.page" \
    --out="$datadir/truncated.page" --rel="1663/$dbid/$boundary_relid" \
    --block=1 --start="$truncate_start" --stop="$truncate_lsn" \
    > "$datadir/truncated.out" 2> "$datadir/truncated.err"; then
    echo 'replay crossed truncation of the target block' >&2
    exit 1
fi
grep -F "target heap block was truncated at LSN $truncate_lsn" \
    "$datadir/truncated.err" >/dev/null
test ! -e "$datadir/truncated.page"
echo 'relation creation and truncation stop replay without writing a page'

if [ "${PG_TEST_QUICK:-0}" = 1 ]; then
    echo 'basic replay and CLI checks passed'
    report_start_warnings
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
grep -F 'applied FPI' "$datadir/fpi-replay.log" >/dev/null
run_applier --waldir="$datadir/pg_wal" --base-page="$datadir/fpi-base.page" \
    --out="$datadir/fpi-json.page" --rel="1663/$dbid/$relid" \
    --block=0 --start="$start" --stop="$last_lsn" --json \
    > "$datadir/fpi-replay.jsonl"
compare_pages "$datadir/fpi-json.page" "$datadir/fpi-final.page"
python3 - "$datadir/fpi-replay.jsonl" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1])]
assert any(event['event'] == 'applied' and event['rmgr'] == 'FPI'
           and event['operation'] == 'FPI' for event in events[:-1]), events
assert events[-1]['event'] == 'done'
assert events[-1]['applied'] == len(events) - 1
print('FPI JSON event verified')
PY

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

# PG14's PageRepairFragmentation leaves trailing LP_UNUSED pointers in place.
# A deleted HOT chain leaves an unused pointer at the end of a heap page.
# Replay its on-access PRUNE incrementally and check the retained pointer.
if [ "$version" -eq 14 ]; then
    sql 'CREATE TABLE replay_prune_tail (id integer, payload text) WITH (fillfactor=100, autovacuum_enabled=false)' >/dev/null
    sql "INSERT INTO replay_prune_tail SELECT n, repeat('x', 100) FROM generate_series(1, 100) AS n" >/dev/null
    tail_id=$(sql "SELECT max(id) FROM replay_prune_tail WHERE ctid::text LIKE '(0,%'")
    test -n "$tail_id"
    sql "UPDATE replay_prune_tail SET payload = 'y' WHERE id = $tail_id" >/dev/null
    case "$(sql "SELECT ctid::text FROM replay_prune_tail WHERE id = $tail_id")" in
        '(0,'*) ;;
        *) echo 'PG14 prune-tail HOT update left block 0' >&2; exit 1 ;;
    esac
    sql "DELETE FROM replay_prune_tail WHERE id = $tail_id" >/dev/null
    sql 'CHECKPOINT' >/dev/null
    relpath=$(sql "SELECT pg_relation_filepath('replay_prune_tail')")
    relid=$(sql "SELECT pg_relation_filenode('replay_prune_tail')")
    dd if="$datadir/$relpath" of="$datadir/prune-tail-base.page" bs=8192 count=1 status=none
    start=$(sql 'SELECT pg_current_wal_insert_lsn()')
    sql 'SELECT count(*) FROM replay_prune_tail' >/dev/null
    stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
    sql 'CHECKPOINT' >/dev/null
    dd if="$datadir/$relpath" of="$datadir/prune-tail-final.page" bs=8192 count=1 status=none
    "$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/prune-tail-records.txt"
    prune_record=$(grep 'rmgr: Heap2' "$datadir/prune-tail-records.txt" |
        grep 'desc: PRUNE' | grep -F "rel 1663/$dbid/$relid blk 0" | head -1)
    test -n "$prune_record" || { echo 'PG14 did not emit on-access PRUNE for trailing line pointer' >&2; exit 1; }
    prune_lsn=$(printf '%s\n' "$prune_record" |
        sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
    # A scan can WAL-log a hint-only FPI before PRUNE even with
    # full_page_writes=off.  The base page already has the required tuple
    # state; replay the PRUNE record itself to test its incremental redo.
    replay_case "$datadir/pg_wal" "$datadir/prune-tail-base.page" \
        "$datadir/prune-tail-replayed.page" "1663/$dbid/$relid" 0 "$prune_lsn" "$prune_lsn" \
        > "$datadir/prune-tail-replay.log"
    grep -F 'applied Heap2 PRUNE:' "$datadir/prune-tail-replay.log" >/dev/null
    if grep -F 'applied FPI' "$datadir/prune-tail-replay.log" >/dev/null; then
        echo 'PG14 prune-tail test replayed an FPI' >&2
        exit 1
    fi
    compare_pages "$datadir/prune-tail-replayed.page" "$datadir/prune-tail-final.page"
    python3 - "$datadir/prune-tail-base.page" "$datadir/prune-tail-final.page" <<'PY'
import struct
import sys

base, final = [open(path, 'rb').read() for path in sys.argv[1:]]
base_lower = struct.unpack_from('<H', base, 12)[0]
final_lower = struct.unpack_from('<H', final, 12)[0]
assert base_lower == final_lower and base_lower > 24, (base_lower, final_lower)
last_item = struct.unpack_from('<I', final, final_lower - 4)[0]
assert (last_item >> 15) & 3 == 0, last_item  # LP_UNUSED remains on PG14
print('PG14 PRUNE retains trailing unused line pointer')
PY
    cat "$datadir/prune-tail-replay.log"
fi

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
target_records=$(grep -F "rel 1663/$dbid/$relid blk 0" \
    "$datadir/vacuum-layout-records.txt")
replay_start=$start
case "$(printf '%s\n' "$target_records" | head -1)" in
    *'desc: FPI_FOR_HINT'*)
        # PG14 can log a hint-bit FPI for this page before the VACUUM redo.
        # The comparison masks hints; start at the first real page change so
        # the test still exercises the incremental cleanup record.
        replay_start=$(printf '%s\n' "$target_records" |
            grep -v 'desc: FPI_FOR_HINT' | head -1 |
            sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
        test -n "$replay_start"
        echo 'VACUUM layout: skipped preceding hint-only FPI'
        ;;
esac
last_lsn=$(grep -F "rel 1663/$dbid/$relid blk 0" "$datadir/vacuum-layout-records.txt" |
    tail -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$last_lsn"
replay_case "$datadir/pg_wal" "$datadir/vacuum-layout-base.page" \
    "$datadir/vacuum-layout-replayed.page" "1663/$dbid/$relid" "0" "$replay_start" "$last_lsn" \
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
python3 - "$1" "$2" <<'PY'
import struct
import sys

def normalized(path):
    page = bytearray(open(path, 'rb').read())
    assert len(page) == 8192
    lower, upper = struct.unpack_from('<HH', page, 12)
    assert 24 <= lower <= upper <= 8192
    page[8:10] = b'\0\0'  # checksum
    page[lower:upper] = bytes(upper - lower)  # unallocated tuple space
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

# Exercise heap branches that are easy to lose behind ordinary INSERT/UPDATE
# tests: tuple locks, speculative insertion confirmation, and INIT_PAGE.
sql 'CREATE TABLE replay_heap_branches (id integer PRIMARY KEY, value text) WITH (autovacuum_enabled=false)' >/dev/null
sql "INSERT INTO replay_heap_branches VALUES (1, 'seed')" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_heap_branches')")
relid=$(sql "SELECT pg_relation_filenode('replay_heap_branches')")
dd if="$datadir/$relpath" of="$datadir/heap-branches-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'BEGIN; SELECT id FROM replay_heap_branches WHERE id=1 FOR UPDATE; COMMIT;' >/dev/null
sql "INSERT INTO replay_heap_branches VALUES (2, 'confirmed') ON CONFLICT DO NOTHING" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/heap-branches-final.page" bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/heap-branches-records.txt"
grep 'rmgr: Heap ' "$datadir/heap-branches-records.txt" |
    grep 'desc: LOCK ' | grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
grep 'rmgr: Heap ' "$datadir/heap-branches-records.txt" |
    grep 'desc: HEAP_CONFIRM ' | grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
lock_lsn=$(grep 'rmgr: Heap ' "$datadir/heap-branches-records.txt" |
    grep 'desc: LOCK ' | grep -F "rel 1663/$dbid/$relid blk 0" |
    head -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$lock_lsn"
# With checksums enabled, the SELECT can log a hint-bit FPI before LOCK.
# Start at LOCK so the test still verifies the incremental redo path.
replay_case "$datadir/pg_wal" "$datadir/heap-branches-base.page" \
    "$datadir/heap-branches-replayed.page" "1663/$dbid/$relid" 0 "$lock_lsn" "$stop" \
    > "$datadir/heap-branches-replay.log"
grep -F 'applied Heap LOCK ' "$datadir/heap-branches-replay.log" >/dev/null
grep -F 'applied Heap CONFIRM ' "$datadir/heap-branches-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/heap-branches-replay.log" >/dev/null; then
    echo 'Heap LOCK/CONFIRM test replayed an FPI' >&2
    exit 1
fi
compare_pages "$datadir/heap-branches-replayed.page" "$datadir/heap-branches-final.page"
tail -4 "$datadir/heap-branches-replay.log"

sql 'CREATE TABLE replay_heap_init (id integer) WITH (autovacuum_enabled=false)' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_heap_init')")
relid=$(sql "SELECT pg_relation_filenode('replay_heap_init')")
make_blank_page "$datadir/heap-init-base.page"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_heap_init VALUES (1)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/heap-init-final.page" bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/heap-init-records.txt"
grep 'rmgr: Heap ' "$datadir/heap-init-records.txt" |
    grep 'desc: INSERT+INIT ' | grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
replay_case "$datadir/pg_wal" "$datadir/heap-init-base.page" \
    "$datadir/heap-init-replayed.page" "1663/$dbid/$relid" 0 "$start" "$stop" \
    > "$datadir/heap-init-replay.log"
grep -F 'applied Heap INSERT ' "$datadir/heap-init-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/heap-init-replay.log" >/dev/null; then
    echo 'Heap INIT_PAGE test replayed an FPI' >&2
    exit 1
fi
compare_pages "$datadir/heap-init-replayed.page" "$datadir/heap-init-final.page"
tail -2 "$datadir/heap-init-replay.log"

# VACUUM ANALYZE updates pg_class statistics in place.  Select the catalog
# block containing this table's row before taking the base-page snapshot.
sql 'CREATE TABLE replay_inplace (id integer, value text) WITH (autovacuum_enabled=false)' >/dev/null
sql "INSERT INTO replay_inplace SELECT n, repeat('x', 100) FROM generate_series(1, 1000) AS n" >/dev/null
catalog_blk=$(sql "SELECT split_part(trim(both '()' from ctid::text), ',', 1) FROM pg_class WHERE oid='replay_inplace'::regclass")
test -n "$catalog_blk"
catalog_path=$(sql "SELECT pg_relation_filepath('pg_class')")
catalog_relid=$(sql "SELECT pg_relation_filenode('pg_class')")
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$catalog_path" of="$datadir/inplace-base.page" bs=8192 skip="$catalog_blk" count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'VACUUM ANALYZE replay_inplace' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$catalog_path" of="$datadir/inplace-final.page" bs=8192 skip="$catalog_blk" count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/inplace-records.txt"
grep 'rmgr: Heap ' "$datadir/inplace-records.txt" |
    grep 'desc: INPLACE ' | grep -F "rel 1663/$dbid/$catalog_relid blk $catalog_blk" >/dev/null
replay_case "$datadir/pg_wal" "$datadir/inplace-base.page" \
    "$datadir/inplace-replayed.page" "1663/$dbid/$catalog_relid" "$catalog_blk" "$start" "$stop" \
    > "$datadir/inplace-replay.log"
grep -F 'applied Heap INPLACE ' "$datadir/inplace-replay.log" >/dev/null
compare_pages "$datadir/inplace-replayed.page" "$datadir/inplace-final.page"
tail -3 "$datadir/inplace-replay.log"

# A KEY SHARE lock started before a concurrent non-key HOT update must also
# lock the new tuple version, producing Heap2 LOCK_UPDATED.
sql 'CREATE TABLE replay_lock_updated (id integer PRIMARY KEY, value text) WITH (autovacuum_enabled=false)' >/dev/null
sql "INSERT INTO replay_lock_updated VALUES (1, 'before')" >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_lock_updated')")
relid=$(sql "SELECT pg_relation_filenode('replay_lock_updated')")
dd if="$datadir/$relpath" of="$datadir/lock-updated-base.page" bs=8192 count=1 status=none
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
"$bindir/psql" -X -qAt -h /tmp -p "$port" -d postgres \
    -c 'SELECT id FROM replay_lock_updated WHERE id=1 AND pg_sleep(2) IS NOT NULL FOR KEY SHARE' \
    > "$datadir/lock-updated-select.out" &
lock_pid=$!
sleeping=0
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if [ "$(sql "SELECT count(*) FROM pg_stat_activity WHERE wait_event='PgSleep' AND query LIKE '%replay_lock_updated%' AND pid <> pg_backend_pid()")" -gt 0 ]; then
        sleeping=1
        break
    fi
    sleep 0.1
done
if [ "$sleeping" -ne 1 ]; then
    wait "$lock_pid" || true
    echo 'KEY SHARE statement did not reach pg_sleep' >&2
    exit 1
fi
sql "UPDATE replay_lock_updated SET value='after' WHERE id=1" >/dev/null
wait "$lock_pid"
grep -x 1 "$datadir/lock-updated-select.out" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
dd if="$datadir/$relpath" of="$datadir/lock-updated-final.page" bs=8192 count=1 status=none
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/lock-updated-records.txt"
grep 'rmgr: Heap2' "$datadir/lock-updated-records.txt" |
    grep 'desc: LOCK_UPDATED ' | grep -F "rel 1663/$dbid/$relid blk 0" >/dev/null
hot_update_lsn=$(grep 'rmgr: Heap ' "$datadir/lock-updated-records.txt" |
    grep 'desc: HOT_UPDATE ' | grep -F "rel 1663/$dbid/$relid blk 0" |
    head -1 | sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$hot_update_lsn"
# Ignore any hint-bit FPI emitted before the first modifying record.
replay_case "$datadir/pg_wal" "$datadir/lock-updated-base.page" \
    "$datadir/lock-updated-replayed.page" "1663/$dbid/$relid" 0 "$hot_update_lsn" "$stop" \
    > "$datadir/lock-updated-replay.log"
grep -F 'applied Heap2 LOCK_UPDATED ' "$datadir/lock-updated-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/lock-updated-replay.log" >/dev/null; then
    echo 'Heap2 LOCK_UPDATED test replayed an FPI' >&2
    exit 1
fi
compare_pages "$datadir/lock-updated-replayed.page" "$datadir/lock-updated-final.page"
tail -4 "$datadir/lock-updated-replay.log"

# A unique-index race can reject a speculative heap insertion after the
# precheck, generating Heap DELETE with XLH_DELETE_IS_SUPER (flags 0x08).
# The index function returns its argument unchanged; its delay widens only
# the gap between the precheck and the actual index insertion.
sql "CREATE FUNCTION replay_slow_key(i integer) RETURNS integer LANGUAGE plpgsql IMMUTABLE AS \$\$ BEGIN PERFORM pg_sleep(0.003); RETURN i; END \$\$" >/dev/null
sql 'CREATE TABLE replay_super (id integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE UNIQUE INDEX replay_super_idx ON replay_super (replay_slow_key(id))' >/dev/null
sql 'INSERT INTO replay_super VALUES (-1)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_super')")
relid=$(sql "SELECT pg_relation_filenode('replay_super')")
cp "$datadir/$relpath" "$datadir/super-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
super_record=
for round in 0 1 2; do
    first=$((round * 100 + 1))
    last=$((first + 79))
    pids=
    for worker in 1 2 3 4 5 6; do
        "$bindir/psql" -X -qAt -h /tmp -p "$port" -d postgres \
            -c "INSERT INTO replay_super SELECT n FROM generate_series($first, $last) AS n ON CONFLICT DO NOTHING" \
            > "$datadir/super-worker-$worker.out" 2>&1 &
        pids="$pids $!"
    done
    for pid in $pids; do
        wait "$pid"
    done
    stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
    "$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/super-records.txt"
    super_record=$(grep 'rmgr: Heap ' "$datadir/super-records.txt" |
        grep 'desc: DELETE ' | grep -E 'flags:? 0x08' |
        grep -F "rel 1663/$dbid/$relid" | head -1 || true)
    if [ -n "$super_record" ]; then
        break
    fi
done
test -n "$super_record" || { echo 'concurrent ON CONFLICT did not emit speculative DELETE WAL' >&2; exit 1; }
super_blk=$(printf '%s\n' "$super_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$super_blk"
sql 'CHECKPOINT' >/dev/null
if test $(( (super_blk + 1) * 8192 )) -le "$(wc -c < "$datadir/super-base.rel")"; then
    dd if="$datadir/super-base.rel" of="$datadir/super-base.page" bs=8192 skip="$super_blk" count=1 status=none
else
    make_blank_page "$datadir/super-base.page"
fi
dd if="$datadir/$relpath" of="$datadir/super-final.page" bs=8192 skip="$super_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/super-base.page" \
    "$datadir/super-replayed.page" "1663/$dbid/$relid" "$super_blk" "$start" "$stop" \
    > "$datadir/super-replay.log"
grep -F 'applied Heap DELETE ' "$datadir/super-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/super-replay.log" >/dev/null; then
    echo 'speculative DELETE test replayed an FPI' >&2
    exit 1
fi
compare_pages "$datadir/super-replayed.page" "$datadir/super-final.page"
echo 'Heap DELETE with XLH_DELETE_IS_SUPER replayed incrementally'

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

# An index scan marks deleted hash entries LP_DEAD.  VACUUM then emits
# DELETE with clear_dead_marking=T on the bucket page.
sql 'CREATE TABLE replay_hash_dead_flag (id integer, value integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_hash_dead_flag_idx ON replay_hash_dead_flag USING hash (value)' >/dev/null
sql 'INSERT INTO replay_hash_dead_flag SELECT n, 1 FROM generate_series(1, 200) AS n' >/dev/null
sql 'DELETE FROM replay_hash_dead_flag WHERE id <= 30' >/dev/null
scan_plan=$(sql 'SET enable_seqscan=off; SET enable_bitmapscan=off; EXPLAIN ANALYZE SELECT id FROM replay_hash_dead_flag WHERE value=1')
printf '%s\n' "$scan_plan" | grep -F 'Index Scan using replay_hash_dead_flag_idx' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_hash_dead_flag_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_hash_dead_flag_idx')")
cp "$datadir/$relpath" "$datadir/hash-dead-flag-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'VACUUM (INDEX_CLEANUP ON) replay_hash_dead_flag' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-dead-flag-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-dead-flag-records.txt"
dead_flag_record=$(grep 'rmgr: Hash' "$datadir/hash-dead-flag-records.txt" |
    grep 'desc: DELETE clear_dead_marking T' |
    grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$dead_flag_record" || { echo 'Hash DELETE did not clear HAS_DEAD_TUPLES' >&2; exit 1; }
dead_flag_blk=$(printf '%s\n' "$dead_flag_record" |
    sed -n 's/.*blkref #1: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$dead_flag_blk"
dd if="$datadir/hash-dead-flag-base.rel" of="$datadir/hash-dead-flag-base.page" bs=8192 skip="$dead_flag_blk" count=1 status=none
dd if="$datadir/hash-dead-flag-final.rel" of="$datadir/hash-dead-flag-final.page" bs=8192 skip="$dead_flag_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/hash-dead-flag-base.page" \
    "$datadir/hash-dead-flag-replayed.page" "1663/$dbid/$relid" "$dead_flag_blk" "$start" "$stop" \
    > "$datadir/hash-dead-flag-replay.log"
grep -F 'applied Hash DELETE ' "$datadir/hash-dead-flag-replay.log" |
    grep -F 'block_ref=1' >/dev/null
if grep -F 'applied FPI' "$datadir/hash-dead-flag-replay.log" >/dev/null; then
    echo 'Hash DELETE dead-flag test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/hash-dead-flag-replayed.page" "$datadir/hash-dead-flag-final.page"
python3 - "$datadir/hash-dead-flag-base.page" "$datadir/hash-dead-flag-replayed.page" "$datadir/hash-dead-flag-final.page" <<'PY'
import struct
import sys

HAS_DEAD_TUPLES = 0x80
flags = []
for path in sys.argv[1:]:
    page = open(path, 'rb').read()
    special = struct.unpack_from('<H', page, 16)[0]
    flags.append(struct.unpack_from('<H', page, special + 12)[0])
assert flags[0] & HAS_DEAD_TUPLES, flags
assert not flags[1] & HAS_DEAD_TUPLES and not flags[2] & HAS_DEAD_TUPLES, flags
print('Hash DELETE clears HAS_DEAD_TUPLES')
PY
tail -2 "$datadir/hash-dead-flag-replay.log"

# A hash index scan marks deleted entries LP_DEAD.  Inserting into its full
# bucket then vacuums one page and updates the metapage in the same WAL record.
sql 'CREATE TABLE replay_hash_one_page (id integer, value integer) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_hash_one_page_idx ON replay_hash_one_page USING hash (value)' >/dev/null
sql 'INSERT INTO replay_hash_one_page SELECT n, 1 FROM generate_series(1, 1200) AS n' >/dev/null
sql 'DELETE FROM replay_hash_one_page WHERE id <= 300' >/dev/null
scan_plan=$(sql 'SET enable_seqscan=off; SET enable_bitmapscan=off; EXPLAIN ANALYZE SELECT id FROM replay_hash_one_page WHERE value=1')
printf '%s\n' "$scan_plan" | grep -F 'Index Scan using replay_hash_one_page_idx' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_hash_one_page_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_hash_one_page_idx')")
cp "$datadir/$relpath" "$datadir/hash-one-page-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_hash_one_page VALUES (1201, 1)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-one-page-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-one-page-records.txt"
one_page_record=$(grep 'rmgr: Hash' "$datadir/hash-one-page-records.txt" |
    grep 'desc: VACUUM_ONE_PAGE ' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$one_page_record" || { echo 'Hash VACUUM_ONE_PAGE WAL was not generated' >&2; exit 1; }
one_page_blk=$(printf '%s\n' "$one_page_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$one_page_blk"
for blk in "$one_page_blk" 0; do
    dd if="$datadir/hash-one-page-base.rel" of="$datadir/hash-one-page-base-$blk.page" bs=8192 skip="$blk" count=1 status=none
    dd if="$datadir/hash-one-page-final.rel" of="$datadir/hash-one-page-final-$blk.page" bs=8192 skip="$blk" count=1 status=none
    replay_case "$datadir/pg_wal" "$datadir/hash-one-page-base-$blk.page" \
        "$datadir/hash-one-page-replayed-$blk.page" "1663/$dbid/$relid" "$blk" "$start" "$stop" \
        > "$datadir/hash-one-page-replay-$blk.log"
    grep -F 'applied Hash VACUUM_ONE_PAGE ' "$datadir/hash-one-page-replay-$blk.log" >/dev/null
    if grep -F 'applied FPI' "$datadir/hash-one-page-replay-$blk.log" >/dev/null; then
        echo 'Hash VACUUM_ONE_PAGE test replayed an FPI' >&2
        exit 1
    fi
    compare_index_pages "$datadir/hash-one-page-replayed-$blk.page" "$datadir/hash-one-page-final-$blk.page"
    tail -3 "$datadir/hash-one-page-replay-$blk.log"
done

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

# Reuse an overflow page freed by SQUEEZE_PAGE.  Its ADD_OVFL_PAGE record
# modifies bitmap block #2 by setting the page's allocation bit.
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-bitmap-reuse-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_hash_overflow SELECT 1 FROM generate_series(1, 1000)' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/hash-bitmap-reuse-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/hash-bitmap-reuse-records.txt"
bitmap_record=$(grep 'rmgr: Hash' "$datadir/hash-bitmap-reuse-records.txt" |
    grep 'desc: ADD_OVFL_PAGE ' | grep 'bmpage_found T' |
    grep -F "rel 1663/$dbid/$relid" | grep 'blkref #2:' | head -1)
test -n "$bitmap_record" || { echo 'Hash bitmap reuse did not generate ADD_OVFL_PAGE block #2' >&2; exit 1; }
bitmap_blk=$(printf '%s\n' "$bitmap_record" |
    sed -n 's/.*blkref #2: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$bitmap_blk"
dd if="$datadir/hash-bitmap-reuse-base.rel" of="$datadir/hash-bitmap-reuse-base.page" bs=8192 skip="$bitmap_blk" count=1 status=none
dd if="$datadir/hash-bitmap-reuse-final.rel" of="$datadir/hash-bitmap-reuse-final.page" bs=8192 skip="$bitmap_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/hash-bitmap-reuse-base.page" \
    "$datadir/hash-bitmap-reuse-replayed.page" "1663/$dbid/$relid" "$bitmap_blk" "$start" "$stop" \
    > "$datadir/hash-bitmap-reuse-replay.log"
grep -F 'applied Hash ADD_OVFL_PAGE ' "$datadir/hash-bitmap-reuse-replay.log" |
    grep -F 'block_ref=2' >/dev/null
if grep -F 'applied FPI' "$datadir/hash-bitmap-reuse-replay.log" >/dev/null; then
    echo 'Hash bitmap reuse test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/hash-bitmap-reuse-replayed.page" "$datadir/hash-bitmap-reuse-final.page"
tail -3 "$datadir/hash-bitmap-reuse-replay.log"

# Exhausting a normal 4096-byte hash bitmap needs more than 32,000 overflow
# pages.  In a separate disposable cluster, narrow only the bitmap-size
# metadata to four bytes while the server is stopped.  PostgreSQL still
# generates the real ADD_OVFL_PAGE WAL that initializes a second bitmap page.
if test -f "$rollover_datadir/postmaster.pid"; then
    "$bindir/pg_ctl" -D "$rollover_datadir" stop -m immediate >/dev/null 2>&1 || true
fi
rm -rf "$rollover_datadir"
# Exercise the checksum transition on every version.  Checksums must be off
# before we deliberately edit the stopped cluster's hash metapage.
"$bindir/initdb" -D "$rollover_datadir" -A trust --no-sync --data-checksums >/dev/null
rollover_port=$((port + 100))
"$bindir/pg_ctl" -D "$rollover_datadir" \
    -o "-k /tmp -p $rollover_port -c fsync=off -c full_page_writes=off -c autovacuum=off" \
    -l "$rollover_datadir/server.log" start >/dev/null
rollover_started=1
rollover_sql() { "$bindir/psql" -X -qAt -h /tmp -p "$rollover_port" -d postgres -c "$1"; }
rollover_sql 'CREATE TABLE replay_bitmap_rollover (id integer, value integer) WITH (autovacuum_enabled=false)' >/dev/null
rollover_sql 'CREATE INDEX replay_bitmap_rollover_idx ON replay_bitmap_rollover USING hash(value)' >/dev/null
rollover_sql 'CHECKPOINT' >/dev/null
rollover_path=$(rollover_sql "SELECT pg_relation_filepath('replay_bitmap_rollover_idx')")
rollover_relid=$(rollover_sql "SELECT pg_relation_filenode('replay_bitmap_rollover_idx')")
rollover_dbid=$(rollover_sql 'SELECT oid FROM pg_database WHERE datname = current_database()')
test "$(rollover_sql 'SHOW data_checksums')" = on
"$bindir/pg_ctl" -D "$rollover_datadir" stop -m fast >/dev/null
rollover_started=0
"$bindir/pg_checksums" --disable -D "$rollover_datadir" >/dev/null
python3 - "$rollover_datadir/$rollover_path" <<'PY'
import struct
import sys

with open(sys.argv[1], 'r+b') as relation:
    page = bytearray(relation.read(8192))
    assert len(page) == 8192
    # PageGetContents begins at byte 24; hashm_bmsize/hashm_bmshift are
    # uint16 fields at offsets 20/22 in HashMetaPageData (PG 14-18).
    assert struct.unpack_from('<HH', page, 44) == (4096, 15)
    struct.pack_into('<HH', page, 44, 4, 5)
    relation.seek(0)
    relation.write(page)
PY
dd if="$rollover_datadir/$rollover_path" \
    of="$datadir/hash-bitmap-rollover-meta-base.page" bs=8192 count=1 status=none
"$bindir/pg_ctl" -D "$rollover_datadir" \
    -o "-k /tmp -p $rollover_port -c fsync=off -c full_page_writes=off -c autovacuum=off" \
    -l "$rollover_datadir/server.log" start >/dev/null
rollover_started=1
test "$(rollover_sql 'SHOW data_checksums')" = off
rollover_start=$(rollover_sql 'SELECT pg_current_wal_insert_lsn()')
rollover_sql 'INSERT INTO replay_bitmap_rollover SELECT n, 1 FROM generate_series(1, 20000) AS n' >/dev/null
rollover_stop=$(rollover_sql 'SELECT pg_current_wal_insert_lsn()')
rollover_sql 'CHECKPOINT' >/dev/null
"$bindir/pg_waldump" -p "$rollover_datadir/pg_wal" -s "$rollover_start" -e "$rollover_stop" \
    > "$datadir/hash-bitmap-rollover-records.txt"
rollover_record=$(grep 'rmgr: Hash' "$datadir/hash-bitmap-rollover-records.txt" |
    grep 'desc: ADD_OVFL_PAGE ' | grep 'bmpage_found F' |
    grep -F "rel 1663/$rollover_dbid/$rollover_relid" | grep 'blkref #3:' |
    grep -F "blkref #4: rel 1663/$rollover_dbid/$rollover_relid blk 0" | head -1)
test -n "$rollover_record" || { echo 'Hash rollover did not update bitmap block #3 and metapage #4' >&2; exit 1; }
rollover_lsn=$(printf '%s\n' "$rollover_record" |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
rollover_blk=$(printf '%s\n' "$rollover_record" |
    sed -n 's/.*blkref #3: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$rollover_lsn" && test -n "$rollover_blk"
make_blank_page "$datadir/hash-bitmap-rollover-base.page"
dd if="$rollover_datadir/$rollover_path" of="$datadir/hash-bitmap-rollover-final.page" \
    bs=8192 skip="$rollover_blk" count=1 status=none
dd if="$rollover_datadir/$rollover_path" \
    of="$datadir/hash-bitmap-rollover-meta-final.page" bs=8192 count=1 status=none
replay_case "$rollover_datadir/pg_wal" "$datadir/hash-bitmap-rollover-base.page" \
    "$datadir/hash-bitmap-rollover-replayed.page" \
    "1663/$rollover_dbid/$rollover_relid" "$rollover_blk" "$rollover_start" "$rollover_stop" \
    > "$datadir/hash-bitmap-rollover-replay.log"
grep -F 'applied Hash ADD_OVFL_PAGE ' "$datadir/hash-bitmap-rollover-replay.log" |
    grep -F 'block_ref=3' >/dev/null
if grep -F 'applied FPI' "$datadir/hash-bitmap-rollover-replay.log" >/dev/null; then
    echo 'Hash bitmap rollover test replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/hash-bitmap-rollover-replayed.page" "$datadir/hash-bitmap-rollover-final.page"
replay_case "$rollover_datadir/pg_wal" "$datadir/hash-bitmap-rollover-meta-base.page" \
    "$datadir/hash-bitmap-rollover-meta-replayed.page" \
    "1663/$rollover_dbid/$rollover_relid" 0 "$rollover_start" "$rollover_stop" \
    > "$datadir/hash-bitmap-rollover-meta-replay.log"
grep -F "applied Hash ADD_OVFL_PAGE lsn=$rollover_lsn " \
    "$datadir/hash-bitmap-rollover-meta-replay.log" |
    grep -F 'block_ref=4' >/dev/null
if grep -F 'applied FPI' "$datadir/hash-bitmap-rollover-meta-replay.log" >/dev/null; then
    echo 'Hash bitmap rollover metapage replayed an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/hash-bitmap-rollover-meta-replayed.page" \
    "$datadir/hash-bitmap-rollover-meta-final.page"
"$bindir/pg_ctl" -D "$rollover_datadir" stop -m fast >/dev/null
rollover_started=0
rm -rf "$rollover_datadir"
tail -2 "$datadir/hash-bitmap-rollover-replay.log"
tail -2 "$datadir/hash-bitmap-rollover-meta-replay.log"

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
    compare_index_pages "$datadir/gist-delete-replayed-$ref.page" "$datadir/gist-delete-final-$ref.page"
    if [ "$ref" -eq 0 ]; then
        grep -F 'applied GiST PAGE_UPDATE' "$datadir/gist-delete-replay-$ref.log" >/dev/null
        python3 - "$datadir/gist-delete-base-$ref.page" \
            "$datadir/gist-delete-replayed-$ref.page" \
            "$datadir/gist-delete-final-$ref.page" <<'PY'
import struct
import sys

def flags(path):
    page = open(path, 'rb').read()
    special = struct.unpack_from('<H', page, 16)[0]
    return struct.unpack_from('<H', page, special + 12)[0]

base, replayed, final = [flags(path) for path in sys.argv[1:]]
assert base & 0x04 == 0 and replayed & 0x04 and final & 0x04, \
    (base, replayed, final)
print('GiST leaf PAGE_UPDATE sets F_TUPLES_DELETED')
PY
    fi
    tail -3 "$datadir/gist-delete-replay-$ref.log"
done

# GiST DELETE is emitted when an index scan marks dead leaf items and a later
# insertion needs space on that leaf.  Check that redo clears HAS_GARBAGE and
# sets F_TUPLES_DELETED, without using a full-page image.
sql 'CREATE TABLE replay_gist_kill (id integer, area box) WITH (autovacuum_enabled=false)' >/dev/null
sql 'INSERT INTO replay_gist_kill SELECT n, box(point(n, 0), point(n + 1, 1)) FROM generate_series(1, 2000) AS n' >/dev/null
sql 'CREATE INDEX replay_gist_kill_idx ON replay_gist_kill USING gist (area)' >/dev/null
sql 'CHECKPOINT' >/dev/null
sql 'DELETE FROM replay_gist_kill WHERE id <= 300' >/dev/null
scan_plan=$(sql "SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_indexonlyscan=off; EXPLAIN (ANALYZE, COSTS OFF) SELECT id FROM replay_gist_kill WHERE area && box(point(0,0), point(5000,2))")
printf '%s\n' "$scan_plan" | grep -F 'Index Scan using replay_gist_kill_idx' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_gist_kill_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_gist_kill_idx')")
cp "$datadir/$relpath" "$datadir/gist-kill-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_gist_kill SELECT n, box(point(n % 300, 0), point((n % 300) + 1, 1)) FROM generate_series(2001, 2500) AS n' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gist-kill-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gist-kill-records.txt"
kill_record=$(grep 'rmgr: Gist' "$datadir/gist-kill-records.txt" |
    grep 'desc: DELETE ' | grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$kill_record" || { echo 'GiST DELETE test did not generate DELETE WAL' >&2; exit 1; }
kill_blk=$(printf '%s\n' "$kill_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$kill_blk"
dd if="$datadir/gist-kill-base.rel" of="$datadir/gist-kill-base.page" bs=8192 skip="$kill_blk" count=1 status=none
dd if="$datadir/gist-kill-final.rel" of="$datadir/gist-kill-final.page" bs=8192 skip="$kill_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gist-kill-base.page" \
    "$datadir/gist-kill-replayed.page" "1663/$dbid/$relid" "$kill_blk" "$start" "$stop" \
    > "$datadir/gist-kill-replay.log"
grep -F 'applied GiST DELETE' "$datadir/gist-kill-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/gist-kill-replay.log" >/dev/null; then
    echo 'GiST DELETE test replayed an FPI' >&2
    exit 1
fi
# PostgreSQL's primary gistprunepage() clears HAS_GARBAGE but does not set
# F_TUPLES_DELETED; gistRedoDeleteRecord() does set it.  Assert that this is
# the only page difference rather than hiding the bit in compare_index_pages.
python3 - "$datadir/gist-kill-base.page" "$datadir/gist-kill-replayed.page" \
    "$datadir/gist-kill-final.page" <<'PY'
import struct
import sys

TUPLES_DELETED = 0x04
HAS_GARBAGE = 0x10

def normalized(path):
    page = bytearray(open(path, 'rb').read())
    assert len(page) == 8192
    lower, upper = struct.unpack_from('<HH', page, 12)
    assert 24 <= lower <= upper <= 8192
    page[8:10] = b'\0\0'  # checksum
    page[lower:upper] = bytes(upper - lower)
    return page

def flags(page):
    special = struct.unpack_from('<H', page, 16)[0]
    return struct.unpack_from('<H', page, special + 12)[0]

base, replayed, final = [normalized(path) for path in sys.argv[1:]]
base_flags, replayed_flags, final_flags = map(flags, (base, replayed, final))
assert base_flags & HAS_GARBAGE and not base_flags & TUPLES_DELETED
assert replayed_flags == final_flags | TUPLES_DELETED, \
    (base_flags, replayed_flags, final_flags)
special = struct.unpack_from('<H', replayed, 16)[0]
replayed[special + 12] &= ~TUPLES_DELETED
assert replayed == final, \
    [i for i, (a, b) in enumerate(zip(replayed, final)) if a != b][:16]
print('GiST DELETE redo sets F_TUPLES_DELETED; all other page bytes match')
PY
tail -3 "$datadir/gist-kill-replay.log"

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

# Long distinct GIN entry keys make the inner tree split.  Its split record
# clears INCOMPLETE_SPLIT on child block reference #3 without an FPI there.
sql 'CREATE TABLE replay_gin_inner_split (id integer, tags text[]) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_gin_inner_split_idx ON replay_gin_inner_split USING gin(tags) WITH (fastupdate=off)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_gin_inner_split_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_gin_inner_split_idx')")
cp "$datadir/$relpath" "$datadir/gin-inner-split-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql "INSERT INTO replay_gin_inner_split SELECT n, ARRAY[repeat('a', 180) || lpad(n::text, 8, '0')] FROM generate_series(1, 2000) AS n" >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/gin-inner-split-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/gin-inner-split-records.txt"
gin_inner_record=$(grep 'rmgr: Gin ' "$datadir/gin-inner-split-records.txt" |
    grep 'desc: SPLIT ' | grep 'isleaf: F' | grep 'blkref #3:' |
    grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$gin_inner_record" || { echo 'GIN inner SPLIT did not include child block #3' >&2; exit 1; }
gin_child_blk=$(printf '%s\n' "$gin_inner_record" |
    sed -n 's/.*blkref #3: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
gin_inner_lsn=$(printf '%s\n' "$gin_inner_record" |
    sed -n 's/.*lsn: \([0-9A-F]*\/[0-9A-F]*\),.*/\1/p')
test -n "$gin_child_blk" && test -n "$gin_inner_lsn"
if test $(( (gin_child_blk + 1) * 8192 )) -le "$(wc -c < "$datadir/gin-inner-split-base.rel")"; then
    dd if="$datadir/gin-inner-split-base.rel" of="$datadir/gin-inner-split-base.page" bs=8192 skip="$gin_child_blk" count=1 status=none
else
    make_blank_page "$datadir/gin-inner-split-base.page"
fi
dd if="$datadir/gin-inner-split-final.rel" of="$datadir/gin-inner-split-final.page" bs=8192 skip="$gin_child_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/gin-inner-split-base.page" \
    "$datadir/gin-inner-split-replayed.page" "1663/$dbid/$relid" "$gin_child_blk" "$start" "$stop" \
    > "$datadir/gin-inner-split-replay.log"
grep -F "applied GIN CLEAR_INCOMPLETE_SPLIT lsn=$gin_inner_lsn" "$datadir/gin-inner-split-replay.log" |
    grep -F 'block_ref=3' >/dev/null
if grep -F "applied FPI lsn=$gin_inner_lsn" "$datadir/gin-inner-split-replay.log" >/dev/null; then
    echo 'GIN inner SPLIT child block #3 used an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/gin-inner-split-replayed.page" "$datadir/gin-inner-split-final.page"
echo 'GIN inner SPLIT clears child INCOMPLETE_SPLIT incrementally'

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

# VACUUM turns an entirely deleted non-root leaf chain into a DEAD head.
# Inserting into that chain replaces the dead tuple at the same offset.
sql 'CREATE TABLE replay_spgist_replace (id integer, pos point) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_spgist_replace_idx ON replay_spgist_replace USING spgist(pos)' >/dev/null
sql 'INSERT INTO replay_spgist_replace SELECT n, point(n % 100, n / 100) FROM generate_series(1, 3000) AS n' >/dev/null
sql 'DELETE FROM replay_spgist_replace' >/dev/null
sql 'VACUUM (INDEX_CLEANUP ON) replay_spgist_replace' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_spgist_replace_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_spgist_replace_idx')")
cp "$datadir/$relpath" "$datadir/spgist-replace-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_spgist_replace VALUES (3001, point(2,0))' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/spgist-replace-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/spgist-replace-records.txt"
replace_record=$(python3 - "$datadir/spgist-replace-records.txt" "$dbid" "$relid" <<'PY'
import re
import sys

target = 'rel 1663/{}/{}'.format(sys.argv[2], sys.argv[3])
for line in open(sys.argv[1]):
    if ('rmgr: SPGist' in line and target in line and
            re.search(r'desc: ADD_LEAF off: (\d+), headoff: \1,', line)):
        print(line.strip())
        break
PY
)
test -n "$replace_record" || { echo 'SP-GiST did not replace a DEAD leaf tuple' >&2; exit 1; }
replace_blk=$(printf '%s\n' "$replace_record" |
    sed -n 's/.*blkref #0: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$replace_blk"
dd if="$datadir/spgist-replace-base.rel" of="$datadir/spgist-replace-base.page" bs=8192 skip="$replace_blk" count=1 status=none
dd if="$datadir/spgist-replace-final.rel" of="$datadir/spgist-replace-final.page" bs=8192 skip="$replace_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/spgist-replace-base.page" \
    "$datadir/spgist-replace-replayed.page" "1663/$dbid/$relid" "$replace_blk" "$start" "$stop" \
    > "$datadir/spgist-replace-replay.log"
grep -F 'applied SP-GiST ADD_LEAF ' "$datadir/spgist-replace-replay.log" >/dev/null
if grep -F 'applied FPI' "$datadir/spgist-replace-replay.log" >/dev/null; then
    echo 'SP-GiST dead-leaf replacement used an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/spgist-replace-replayed.page" "$datadir/spgist-replace-final.page"
echo 'SP-GiST ADD_LEAF replaces a DEAD tuple at the same offset'

# NULL keys use a separate SP-GiST tree.  Its first split marks the inner
# page with SPGIST_NULLS, which must survive incremental replay.
sql 'CREATE TABLE replay_spgist_null (id integer, pos point) WITH (autovacuum_enabled=false)' >/dev/null
sql 'CREATE INDEX replay_spgist_null_idx ON replay_spgist_null USING spgist(pos)' >/dev/null
sql 'CHECKPOINT' >/dev/null
relpath=$(sql "SELECT pg_relation_filepath('replay_spgist_null_idx')")
relid=$(sql "SELECT pg_relation_filenode('replay_spgist_null_idx')")
cp "$datadir/$relpath" "$datadir/spgist-null-base.rel"
start=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'INSERT INTO replay_spgist_null SELECT n, NULL::point FROM generate_series(1, 5000) AS n' >/dev/null
stop=$(sql 'SELECT pg_current_wal_insert_lsn()')
sql 'CHECKPOINT' >/dev/null
cp "$datadir/$relpath" "$datadir/spgist-null-final.rel"
"$bindir/pg_waldump" -p "$datadir/pg_wal" -s "$start" -e "$stop" > "$datadir/spgist-null-records.txt"
null_split_record=$(grep 'rmgr: SPGist' "$datadir/spgist-null-records.txt" |
    grep 'desc: PICKSPLIT ' | grep '(nulls)' | grep 'blkref #2:' |
    grep -F "rel 1663/$dbid/$relid" | head -1)
test -n "$null_split_record" || { echo 'SP-GiST NULL PICKSPLIT WAL was not generated' >&2; exit 1; }
null_inner_blk=$(printf '%s\n' "$null_split_record" |
    sed -n 's/.*blkref #2: rel [0-9]*\/[0-9]*\/[0-9]* blk \([0-9]*\).*/\1/p')
test -n "$null_inner_blk"
if test $(( (null_inner_blk + 1) * 8192 )) -le "$(wc -c < "$datadir/spgist-null-base.rel")"; then
    dd if="$datadir/spgist-null-base.rel" of="$datadir/spgist-null-base.page" bs=8192 skip="$null_inner_blk" count=1 status=none
else
    make_blank_page "$datadir/spgist-null-base.page"
fi
dd if="$datadir/spgist-null-final.rel" of="$datadir/spgist-null-final.page" bs=8192 skip="$null_inner_blk" count=1 status=none
replay_case "$datadir/pg_wal" "$datadir/spgist-null-base.page" \
    "$datadir/spgist-null-replayed.page" "1663/$dbid/$relid" "$null_inner_blk" "$start" "$stop" \
    > "$datadir/spgist-null-replay.log"
grep -F 'applied SP-GiST PICKSPLIT ' "$datadir/spgist-null-replay.log" |
    grep -F 'block_ref=2' >/dev/null
if grep -F 'applied FPI' "$datadir/spgist-null-replay.log" >/dev/null; then
    echo 'SP-GiST NULL PICKSPLIT used an FPI' >&2
    exit 1
fi
compare_index_pages "$datadir/spgist-null-replayed.page" "$datadir/spgist-null-final.page"
echo 'SP-GiST NULL PICKSPLIT inner page matches'

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
report_start_warnings
echo "integration checks passed for PostgreSQL $version"
