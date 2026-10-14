# Usage guide

`pg_lsn_block_applier` works offline on one block of one relation's **main fork**. It reads WAL segments and a saved page, then writes a new one-page file. It does not read PostgreSQL's catalogs or infer the relation and block for you.

## Inputs

1. **Base page:** an exact, raw copy of the target block from before the WAL records to replay. The `--fpi` name is historical: the file can be a page extracted from a consistent, earlier relation copy or a restored full-page image. It must contain exactly one page (`BLCKSZ`, normally 8192 bytes) from the same cluster and PostgreSQL major version. Keep the base page's LSN intact.
2. **WAL:** all segment files needed from `--start` through `--stop`, in one directory. Use the timeline and WAL segment size of the source cluster. The utility reads one specified timeline; it does not traverse timeline history.
3. **Target:** `--rel=T/D/R` gives the tablespace OID, database OID, and relation *file number*; `--block=N` gives the zero-based block number. Get these from the WAL block reference or the matching relation file. A SQL table OID can differ from the relation file number, and the file number can change after a rewrite.
4. **Range:** `--start` and `--stop` are inclusive **record start LSNs**. Start at or before the first record that changes the base page, and stop at the desired record. The tool skips a record if the input page's LSN already reaches that record's end LSN. A base page from after the desired state cannot be rolled back.

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

Look for block references such as `rel 1663/24605/2072200 blk 42`. Use the `lsn:` values printed for those records to select the replay range. If the data comes from a non-default timeline or WAL segment size, carry those values into the replay command.

For a known relation in a database, PostgreSQL's `pg_relation_filepath('schema.table')` and `pg_relation_filenode('schema.table')` can help locate its current file. Verify that these identifiers correspond to the WAL interval being examined.

## Run the replay

```sh
./pg_lsn_block_applier \
  --waldir=/path/to/pg_wal \
  --fpi=/path/to/base.page \
  --out=/path/to/replayed.page \
  --rel=1663/24605/2072200 \
  --block=42 \
  --start=2A9C/51FB7280 \
  --stop=2A9C/51FB7D98 \
  --tli=1
```

All options except the last three below are required:

| Option | Meaning |
| --- | --- |
| `--waldir=DIR` | Directory containing the WAL segment files. |
| `--fpi=FILE` | Earlier raw image of exactly one target page. |
| `--out=FILE` | Output page file; an existing file at this path is overwritten. |
| `--rel=T/D/R` | Tablespace/database/relation file identifier. |
| `--block=N` | Zero-based block number in the main fork. |
| `--start=LSN` | First WAL record start LSN to consider. |
| `--stop=LSN` | Last WAL record start LSN to consider, inclusive. |
| `--tli=N` | Timeline ID; defaults to `1`. |
| `--segsize=BYTES` | WAL segment size; defaults to `16777216` (16 MiB). |
| `--data-checksums` | Compute an output checksum even if the input page has none or has an invalid checksum. |

Use an output path outside `PGDATA`. The output is a raw page for inspection; it is not a relation file. The tool prints each applied record and the final count. If no selected record is applied, the output may simply be the base page.

## Replay boundaries

- Heap and Heap2 changes for the selected block are handled by version-specific code. Other resource managers can contribute an applicable full-page image. A matching incremental record from an unsupported resource manager makes the command fail instead of producing a partial result.
- Records can refer to multiple blocks. Replaying one block does not recover related index pages, visibility maps, free-space maps, transaction status, or other cluster state.
- Relation creation or truncation affecting the requested block can invalidate the base page; the tool checks for these WAL events and stops when appropriate.
- The program does not authenticate the page's provenance or prove that the available WAL is complete. Check the result against other evidence before using it in an investigation.

For details on WAL records and their block references, see the [PostgreSQL `pg_waldump` documentation](https://www.postgresql.org/docs/current/pgwaldump.html).
