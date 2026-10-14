# pg_lsn_block_applier

`pg_lsn_block_applier` reconstructs **one PostgreSQL data page** at a chosen WAL position. Give it an earlier image of the page and the WAL for that relation and block; it replays applicable records and writes the resulting page to a separate file.

This is an experimental tool for inspecting a page's history, investigating corruption, and checking what a sequence of WAL records did to a block. It does not perform cluster recovery. Its input and output are single pages, not tables or backups.

This project was developed with the assistance of AI tools.

## What it supports

- Vanilla PostgreSQL **14, 15, 16, 17, and 18**. Build a separate binary with the matching major version's headers, libraries, and source tree.
- Main-fork heap changes, including insert, update, delete, tuple locking, multi-insert, pruning, and freezing where applicable to the PostgreSQL version.
- Selected incremental B-tree, Hash, GiST, GIN, SP-GiST, BRIN, Generic WAL, and Sequence changes without a full-page image. This includes B-tree and GiST page splits and updates to individual pages in multi-block WAL records. See [the coverage table](docs/usage.md#replay-boundaries) for supported variants.
- Applicable full-page images for the selected block, including records from other WAL resource managers.
- Explicit failure when a selected block needs an incremental WAL operation the tool cannot replay. Records for other blocks are ignored.

The tool does not yet replay every WAL record type. For a record that changes several pages, it updates only the selected page; it does not need copies of the other pages. A matching unsupported increment stops with an error. Treat the output as an analysis artifact and verify it before drawing conclusions.

## Build

You need a C compiler, `make`, a vanilla PostgreSQL source tree, and matching PostgreSQL development headers and frontend libraries. `PG_CONFIG` and `PG_SRC` must refer to the same PostgreSQL major version as the WAL; the build checks the versions in `pg_config --version` and `PG_SRC/configure.ac` before compiling. Build in an environment with these dependencies, such as a container:

```sh
make PG_CONFIG=/path/to/pg_config PG_SRC=/path/to/postgresql-source
```

The executable is `./pg_lsn_block_applier`. `make clean` needs no PostgreSQL source path. To stage an installation, run `make install` with the same build variables and `DESTDIR=/path/to/staging`; it installs the binary under `bin` and `LICENSE`/`NOTICE` under `share/doc/pg_lsn_block_applier`. The default prefix is `/usr/local`.

Run the integration test with `make check PG_CONFIG=/path/to/pg_config PG_SRC=/path/to/postgresql-source`. The test uses `pg_config --bindir` and the freshly built executable by default. Set `PG_BINDIR` or `BINARY` to use other paths; both are also accepted by `tests/integration.sh` when run directly.

## Example

Prepare a saved image of the target page from *before* the changes you want to replay. It must contain exactly one PostgreSQL page (normally 8192 bytes). Keep the WAL segments covering the whole interval and identify the tablespace, database, relation file number, block number, and record LSNs. For a page saved by `pg_waldump --save-fullpage`, use the WAL LSN encoded in its filename as `--start`:

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

Here, `--rel` is `tablespace/database/relfilenode`, **not** a table OID. `--start` and `--stop` are inclusive WAL record start LSNs. A page saved by `pg_waldump` can contain an older page-header LSN than the FPI record named in its file. For a raw page copied from an earlier relation file, you may omit `--start` when WAL covering the page-header LSN is available. The output is one raw page. Choose an output path outside the cluster's data directory.

The `--base-page` option (alias `--input`) accepts a raw page image from a saved relation file or a restored full-page image. It does not require the page to originate from WAL. PostgreSQL 16 and newer can produce a base page with `pg_waldump --save-fullpage=DIR` when a suitable full-page image is present in WAL. The saved filename includes the timeline, FPI record LSN, relation and block. See [the usage guide](docs/usage.md#find-the-records) for a command. The utility reads WAL segment size from the segment header. `--data-checksums` requests a checksum on the output page when the input page does not carry a valid one. `--json` writes one JSON object per applied record and a final summary object.

## Documentation

See [docs/usage.md](docs/usage.md) for a step-by-step workflow, input requirements, all options, and replay limitations.

## License

The project is distributed under Apache License 2.0; PostgreSQL-derived portions retain the PostgreSQL License notice. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
