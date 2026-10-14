# pg_lsn_block_applier

`pg_lsn_block_applier` reconstructs **one PostgreSQL data page** at a chosen WAL position. Give it an earlier image of the page and the WAL for that relation and block; it replays applicable records and writes the resulting page to a separate file.

This is an experimental tool for inspecting a page's history, investigating corruption, and checking what a sequence of WAL records did to a block. It does not perform cluster recovery. Its input and output are single pages, not tables or backups.

## What it supports

- Vanilla PostgreSQL **14, 15, 16, 17, and 18**. Build a separate binary with the matching major version's headers, libraries, and source tree.
- Main-fork heap changes, including insert, update, delete, tuple locking, multi-insert, pruning, and freezing where applicable to the PostgreSQL version.
- Selected incremental B-tree, Hash, GiST, GIN, SP-GiST, BRIN, Generic WAL, and Sequence changes without a full-page image. This includes B-tree and GiST page splits and updates to individual pages in multi-block WAL records. See [the coverage table](docs/usage.md#replay-boundaries) for supported variants.
- Applicable full-page images for the selected block, including records from other WAL resource managers.
- Explicit failure when a selected block needs an incremental WAL operation the tool cannot replay. Records for other blocks are ignored.

The tool does not yet replay every WAL record type. For a record that changes several pages, it updates only the selected page; it does not need copies of the other pages. A matching unsupported increment stops with an error. Treat the output as an analysis artifact and verify it before drawing conclusions.

## Build

You need a C compiler, `make`, a vanilla PostgreSQL source tree, and the matching PostgreSQL development headers and frontend libraries. `PG_CONFIG` must point to the same PostgreSQL major version as the source tree and WAL. Build in an environment with these dependencies, such as a container:

```sh
make PG_CONFIG=/path/to/pg_config PG_SRC=/path/to/postgresql-source
```

The executable is `./pg_lsn_block_applier`. No files need to be copied into the PostgreSQL source tree.

## Example

Prepare a saved image of the target page from *before* the changes you want to replay. It must contain exactly one PostgreSQL page (normally 8192 bytes). Keep the WAL segments covering the whole interval and identify the tablespace, database, relation file number, block number, and record LSNs. For example:

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

Here, `--rel` is `tablespace/database/relfilenode`, **not** a table OID. `--start` and `--stop` refer to WAL record start LSNs; both endpoints are included. The output is one raw page. Choose an output path outside the cluster's data directory.

The `--fpi` option accepts a raw page image from a saved relation file or a restored full-page image. It does not require the page to originate from WAL. Optional `--segsize=BYTES` overrides the default 16 MiB WAL segment size; `--data-checksums` requests a checksum on the output page when the input page does not carry a valid one.

## Documentation

See [docs/usage.md](docs/usage.md) for a step-by-step workflow, input requirements, all options, and replay limitations.

## License

Apache License 2.0. See [LICENSE](LICENSE).
