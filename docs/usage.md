# Usage guide

`pg_lsn_block_applier` works offline on one block of one relation's **main fork**. It reads WAL segments and a saved page, then writes a new one-page file. It does not read PostgreSQL's catalogs or infer the relation and block for you.

## Inputs

1. **Base page:** an exact, raw copy of the target block from before the WAL records to replay. The file can be a page extracted from a consistent, earlier relation copy or a restored full-page image. It must contain exactly one page (`BLCKSZ`, normally 8192 bytes) from the same cluster and PostgreSQL major version. Keep the base page's LSN intact.
2. **WAL:** all segment files needed from the base page's LSN (or explicit `--start`) through `--stop`, in one directory. The utility reads the WAL segment size from the segment header. It reads one specified timeline; it does not traverse timeline history.
3. **Target:** `--rel=T/D/R` gives the tablespace OID, database OID, and relation *file number*; `--block=N` gives the zero-based block number. Get these from the WAL block reference or the matching relation file. A SQL table OID can differ from the relation file number, and the file number can change after a rewrite.
4. **Range:** `--stop` is an inclusive **record start LSN**. By default replay starts at the LSN stored in the base page, so WAL must be available from that point. For a page from `pg_waldump --save-fullpage`, pass `--start` using the FPI record LSN encoded in its filename: the page-header LSN can be much older. If `--start` is later than the page's LSN, the utility warns that intervening records may be skipped. An uninitialized page has no LSN and requires `--start`. The tool skips a record if the input page's LSN already reaches that record's end LSN. A base page from after the desired state cannot be rolled back.

Use a stable, saved copy of the page and WAL. Copying a page directly from a running data directory can capture a state that does not match your chosen WAL range.

For example, if the target is block 42 of a default-tablespace relation with an 8 KiB page size, extract it from an earlier, consistent copy of that relation file:

```sh
dd if=/path/to/saved/base/24605/2072200 of=/path/to/base.page bs=8192 skip=42 count=1
```

This example assumes the block is in the first relation segment. Check that `base.page` contains exactly 8192 bytes before replaying it.

## Find the records

Use the `pg_waldump` from the same PostgreSQL major version to inspect an interval:

```sh
/path/to/pg_waldump -p /path/to/pg_wal -s 2A9C/51FB7280 -e 2A9C/51FB7E00
```

Look for block references such as `rel 1663/24605/2072200 blk 42`. Use the `lsn:` values printed for those records to select the replay range. If the data comes from a non-default timeline, pass that timeline to the replay command.

With PostgreSQL 16 or newer, `pg_waldump` can extract a full-page image for the selected relation and block. It is suitable as a base page when its LSN precedes the desired output state:

```sh
/path/to/pg_waldump -p /path/to/pg_wal \
  --relation=1663/24605/2072200 --block=42 \
  -s 2A9C/51FB7280 -e 2A9C/51FB7E00 \
  --save-fullpage=/path/to/fpi
```

Choose the saved `*_main` file for the target block and pass it to `--base-page`. Pass the FPI record LSN in the saved filename as `--start`; do not use the older LSN stored inside the page. Keep the WAL from that record through the requested `--stop`.

For a known relation in a database, PostgreSQL's `pg_relation_filepath('schema.table')` and `pg_relation_filenode('schema.table')` can help locate its current file. Verify that these identifiers correspond to the WAL interval being examined.

## Run the replay

```sh
./pg_lsn_block_applier \
  --waldir=/path/to/pg_wal \
  --base-page=/path/to/fpi/00000001-00002A9C-51FB7280.1663.24605.2072200.42_main \
  --out=/path/to/replayed.page \
  --rel=1663/24605/2072200 \
  --block=42 \
  --start=2A9C/51FB7280 \
  --stop=2A9C/51FB7D98 \
  --tli=1
```

`--waldir`, `--base-page` (or `--input`), `--out`, `--rel`, `--block`, and `--stop` are required:

| Option | Meaning |
| --- | --- |
| `--waldir=DIR` | Directory containing the WAL segment files. |
| `--base-page=FILE`, `--input=FILE` | Earlier raw image of exactly one target page. |
| `--out=FILE` | Output page file; an existing file at this path is overwritten. |
| `--rel=T/D/R` | Tablespace/database/relation file identifier. |
| `--block=N` | Zero-based block number in the main fork. |
| `--start=LSN` | First WAL record start LSN to consider; defaults to the base page's LSN. Use the filename LSN for a `--save-fullpage` page; required for a zero page. |
| `--stop=LSN` | Last WAL record start LSN to consider, inclusive. |
| `--tli=N` | Timeline ID; defaults to `1`. |
| `--data-checksums` | Compute an output checksum even if the input page has none or has an invalid checksum. |
| `--json` | Emit newline-delimited JSON events for applied records and the final summary. |
| `--help`, `--version` | Show usage or version and exit successfully. |

Use an output path outside `PGDATA`. The output is a raw page for inspection; it is not a relation file. The tool prints one line per applied record and a final count. With `--json`, each stdout line is a JSON object with `event`, plus record fields (`rmgr`, `operation`, `lsn`, `end_lsn`, `rmid`, `info`, `details`) or summary fields (`applied`, `output`). Warnings and errors go to stderr. If no selected record is applied, the output may simply be the base page.

## Replay boundaries

The following incremental changes are implemented for a matching main-fork block in PostgreSQL 14–18:

| Resource manager | Supported incremental changes |
| --- | --- |
| Heap, Heap2 | Insert, multi-insert, update, HOT update, delete, tuple locks, confirmation, in-place update, visibility, pruning, and version-specific freeze/vacuum records. |
| B-tree | `INSERT_LEAF`, `INSERT_UPPER`, `INSERT_META`, `INSERT_POST`, `DEDUP`, `SPLIT_L`, `SPLIT_R` (including posting-list rewrites), `NEWROOT`, `META_CLEANUP`, `MARK_PAGE_HALFDEAD`, `UNLINK_PAGE`, `UNLINK_PAGE_META`, and `DELETE`/`VACUUM`, including posting-list updates. |
| Hash | `INIT_META_PAGE`, `INIT_BITMAP_PAGE`, `ADD_OVFL_PAGE`, `INSERT`, `MOVE_PAGE_CONTENTS`, `SQUEEZE_PAGE`, `DELETE`, `SPLIT_CLEANUP`, `UPDATE_META_PAGE`, and `VACUUM_ONE_PAGE`; old-bucket, new-bucket, and metapage changes from `SPLIT_ALLOCATE_PAGE`; bucket flags from `SPLIT_COMPLETE`. `SPLIT_PAGE` supplies the new bucket's full-page image. |
| GiST | `PAGE_UPDATE`, including tuple deletion and replacement; `DELETE`, `PAGE_SPLIT`, and `PAGE_DELETE`, including their referenced parent or child pages. |
| GIN | `INSERT` on entry-tree leaves and internal pages, including tuple replacement; posting-tree leaf segment recompression and internal posting-item insertion; `VACUUM_DATA_LEAF_PAGE`, `CREATE_PTREE`, `INSERT_LISTPAGE`, `UPDATE_META_PAGE`, `DELETE_LISTPAGE`, and `DELETE_PAGE`, including their selected page references. Split pages and entry-tree vacuum pages carry full-page images. |
| SP-GiST | `ADD_LEAF`, `MOVE_LEAFS`, `ADD_NODE`, `SPLIT_TUPLE`, `PICKSPLIT`, `VACUUM_LEAF`, `VACUUM_ROOT`, and `VACUUM_REDIRECT`, including referenced parent and destination pages. |
| BRIN | `CREATE_INDEX`, `INSERT` and `INSERT+INIT`, `UPDATE`, `SAMEPAGE_UPDATE`, `REVMAP_EXTEND`, and `DESUMMARIZE` on the relevant metapage, revmap, or regular page. |
| Generic WAL | Byte-range deltas for the selected page, as used by extensions such as `bloom`. |
| Sequence | `LOG` on the sequence page. PostgreSQL can reserve future values in WAL, so the resulting page can differ from the live page until crash recovery. |

Applicable full-page images can restore a selected block from other resource managers. A matching incremental record outside the supported variants stops with an error rather than producing a partial result. The integration script compares replayed pages with PostgreSQL pages after WAL records without full-page images for representative variants; it does not exercise every variant in this table.

- A WAL record may refer to several blocks. The program applies the change for the selected block only; the other pages are neither read nor written. The result does not include related cluster state.
- Only the main fork is in scope. Visibility-map, free-space-map, and init forks are ignored.
- Relation creation or truncation affecting the requested block can invalidate the base page; the tool checks for these WAL events and stops when appropriate.
- The program does not authenticate the page's provenance or prove that the available WAL is complete. Check the result against other evidence before using it in an investigation.

For details on WAL records and their block references, see the [PostgreSQL `pg_waldump` documentation](https://www.postgresql.org/docs/current/pgwaldump.html).
