PG_CONFIG ?= pg_config
PG_SRC ?=
XLOGREADER = $(PG_SRC)/src/backend/access/transam/xlogreader.c

PG_INCLUDEDIR = $(shell $(PG_CONFIG) --includedir-server)
PG_INTERNALDIR = $(shell $(PG_CONFIG) --includedir)/internal
PG_LIBDIR = $(shell $(PG_CONFIG) --pkglibdir)
PG_BINDIR ?= $(shell $(PG_CONFIG) --bindir)
BINARY ?= $(abspath pg_lsn_block_applier)
PG_CODECS = $(filter -lz -llz4 -lzstd,$(shell $(PG_CONFIG) --libs))
PG_LIBS = -lpgcommon -lpgport $(PG_CODECS)
SOURCES = pg_lsn_block_applier.c heap.c nbtree.c hash.c gist.c gin.c spgist.c brin.c generic.c

CC ?= cc
CFLAGS ?= -O2 -g
WARNFLAGS ?= -Wall -Wextra -Wformat=2
CFLAGS += $(WARNFLAGS)
PG_CPPFLAGS = -DFRONTEND -isystem $(PG_INCLUDEDIR) -isystem $(PG_INTERNALDIR)
PG_LDFLAGS = -L$(PG_LIBDIR)

prefix ?= /usr/local
bindir ?= $(prefix)/bin
docdir ?= $(prefix)/share/doc/pg_lsn_block_applier
INSTALL ?= install

.PHONY: all check clean install layout-check check-pg-source
all: pg_lsn_block_applier

check-pg-source:
	@test -n "$(PG_SRC)" || { \
		echo 'Set PG_SRC to the matching vanilla PostgreSQL source tree' >&2; \
		exit 1; \
	}
	@test -f "$(XLOGREADER)" || { \
		echo 'PostgreSQL source file not found: $(XLOGREADER)' >&2; \
		exit 1; \
	}
	@test -f "$(PG_SRC)/configure.ac" || { \
		echo 'PostgreSQL source version file not found: $(PG_SRC)/configure.ac' >&2; \
		exit 1; \
	}
	@source_major=$$(sed -n 's/^AC_INIT(\[PostgreSQL\],[[:space:]]*\[\([0-9][0-9]*\).*/\1/p' "$(PG_SRC)/configure.ac" | head -n 1); \
		config_version=$$("$(PG_CONFIG)" --version) || { \
			echo 'Could not read PostgreSQL version from PG_CONFIG=$(PG_CONFIG)' >&2; \
			exit 1; \
		}; \
		config_major=$$(printf '%s\n' "$$config_version" | \
			sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p'); \
		if test -z "$$source_major" || test -z "$$config_major"; then \
			echo "Could not determine PostgreSQL major version from PG_SRC/configure.ac or PG_CONFIG ($$config_version)" >&2; \
			exit 1; \
		fi; \
		if test "$$source_major" != "$$config_major"; then \
			echo "PostgreSQL major version mismatch: PG_SRC=$$source_major, PG_CONFIG=$$config_major ($$config_version)" >&2; \
			exit 1; \
		fi

layout-check: layout_check.c pg_lsn_block_applier_internal.h
	$(CC) $(CPPFLAGS) $(PG_CPPFLAGS) $(CFLAGS) -std=gnu11 -fsyntax-only layout_check.c

pg_lsn_block_applier: check-pg-source layout-check $(SOURCES) pg_lsn_block_applier_internal.h $(wildcard $(XLOGREADER))
	@set -eu; \
		tmp_dir=$$(mktemp -d); \
		trap 'rm -rf "$$tmp_dir"' EXIT; \
		$(CC) $(CPPFLAGS) $(PG_CPPFLAGS) $(CFLAGS) -Wno-sign-compare -std=gnu11 \
			-c "$(XLOGREADER)" -o "$$tmp_dir/xlogreader.o"; \
		$(CC) $(CPPFLAGS) $(PG_CPPFLAGS) $(CFLAGS) -std=gnu11 $(SOURCES) \
			"$$tmp_dir/xlogreader.o" $(LDFLAGS) $(PG_LDFLAGS) $(PG_LIBS) $(LDLIBS) -o $@

check: pg_lsn_block_applier
	@set -eu; \
		version=$$("$(PG_CONFIG)" --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p'); \
		test -n "$$version" || { echo 'Could not determine PostgreSQL major version from PG_CONFIG' >&2; exit 1; }; \
		PG_BINDIR="$(PG_BINDIR)" BINARY="$(BINARY)" sh tests/integration.sh "$$version"

install: pg_lsn_block_applier
	$(INSTALL) -d "$(DESTDIR)$(bindir)" "$(DESTDIR)$(docdir)"
	$(INSTALL) -m 755 pg_lsn_block_applier "$(DESTDIR)$(bindir)/pg_lsn_block_applier"
	$(INSTALL) -m 644 LICENSE NOTICE "$(DESTDIR)$(docdir)/"

clean:
	rm -f pg_lsn_block_applier xlogreader.o
