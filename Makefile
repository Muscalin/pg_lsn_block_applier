PG_CONFIG ?= pg_config
PG_SRC ?=

ifeq ($(PG_SRC),)
$(error Set PG_SRC to the matching vanilla PostgreSQL source tree)
endif

PG_INCLUDEDIR := $(shell $(PG_CONFIG) --includedir-server)
PG_INTERNALDIR := $(shell $(PG_CONFIG) --includedir)/internal
PG_LIBDIR := $(shell $(PG_CONFIG) --pkglibdir)
PG_CODECS := $(filter -lz -llz4 -lzstd,$(shell $(PG_CONFIG) --libs))
PG_LIBS := -lpgcommon -lpgport $(PG_CODECS)
XLOGREADER := $(PG_SRC)/src/backend/access/transam/xlogreader.c

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -DFRONTEND -I$(PG_INCLUDEDIR) -I$(PG_INTERNALDIR)
LDFLAGS += -L$(PG_LIBDIR)

.PHONY: all clean
all: pg_lsn_block_applier

pg_lsn_block_applier: pg_lsn_block_applier.c $(XLOGREADER)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 $^ $(LDFLAGS) $(PG_LIBS) -o $@

clean:
	rm -f pg_lsn_block_applier
