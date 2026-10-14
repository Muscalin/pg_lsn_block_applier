#define FRONTEND 1

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/ginxlog.h"
#include "access/itup.h"
#include "access/hash_xlog.h"
#include "access/nbtxlog.h"
#include "access/rmgr.h"
#include "access/spgxlog.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "common/relpath.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "getopt_long.h"
#include "storage/bufpage.h"
#include "storage/checksum_impl.h"
#include "storage/itemid.h"
#if PG_VERSION_NUM >= 160000
#include "storage/relfilelocator.h"
#else
#include "storage/relfilenode.h"
typedef RelFileNode RelFileLocator;
#endif

#if PG_VERSION_NUM < 140000 || PG_VERSION_NUM >= 190000
#error "Build this utility with vanilla PostgreSQL 14 through 18 headers"
#endif

static int WalSegSz = DEFAULT_XLOG_SEG_SIZE;

/* PostgreSQL 14-16 did not name these block references. */
#ifndef HEAP_UPDATE_BLKREF_HEAP_NEW
#define HEAP_UPDATE_BLKREF_HEAP_NEW 0
#endif

#ifndef HEAP_UPDATE_BLKREF_HEAP_OLD
#define HEAP_UPDATE_BLKREF_HEAP_OLD 1
#endif

#if PG_VERSION_NUM < 150000
#define XLogRecMaxBlockId(record) ((record)->max_block_id)
#endif

#if PG_VERSION_NUM < 160000
#define relNumber relNode
#define spcOid spcNode
#define dbOid dbNode
#endif

/* The access method headers pull backend-only buffer and lock APIs into a
 * frontend program.  Keep only the on-disk fields used by these redo paths. */
typedef struct OfflineBTPageOpaqueData
{
	BlockNumber btpo_prev;
	BlockNumber btpo_next;
	uint32 btpo_level;
	uint16 btpo_flags;
	uint16 btpo_cycleid;
} OfflineBTPageOpaqueData;

#define OfflineBTPageGetOpaque(page) \
	((OfflineBTPageOpaqueData *) PageGetSpecialPointer(page))
#define OFFLINE_BTP_LEAF 0x0001
#define OFFLINE_BTP_ROOT 0x0002
#define OFFLINE_BTP_DELETED 0x0004
#define OFFLINE_BTP_META 0x0008
#define OFFLINE_BTP_HALF_DEAD 0x0010
#define OFFLINE_BTP_HAS_GARBAGE 0x0040
#define OFFLINE_BTP_INCOMPLETE_SPLIT 0x0080
#define OFFLINE_BTP_HAS_FULLXID 0x0100
#define OFFLINE_BTREE_MAGIC 0x053162
#define OFFLINE_P_NONE 0

typedef struct OfflineBTMetaPageData
{
	uint32 btm_magic;
	uint32 btm_version;
	BlockNumber btm_root;
	uint32 btm_level;
	BlockNumber btm_fastroot;
	uint32 btm_fastlevel;
	uint32 btm_last_cleanup_num_delpages;
	float8 btm_last_cleanup_num_heap_tuples;
	bool btm_allequalimage;
} OfflineBTMetaPageData;

typedef struct OfflineHashMetaPrefix
{
	uint32 hashm_magic;
	uint32 hashm_version;
	double hashm_ntuples;
} OfflineHashMetaPrefix;

#define OFFLINE_HASH_MAX_SPLITPOINTS 98
typedef struct OfflineHashMetaPageData
{
	uint32 hashm_magic;
	uint32 hashm_version;
	double hashm_ntuples;
	uint16 hashm_ffactor;
	uint16 hashm_bsize;
	uint16 hashm_bmsize;
	uint16 hashm_bmshift;
	uint32 hashm_maxbucket;
	uint32 hashm_highmask;
	uint32 hashm_lowmask;
	uint32 hashm_ovflpoint;
	uint32 hashm_firstfree;
	uint32 hashm_nmaps;
	RegProcedure hashm_procid;
	uint32 hashm_spares[OFFLINE_HASH_MAX_SPLITPOINTS];
	BlockNumber hashm_mapp[1024];
} OfflineHashMetaPageData;

typedef struct OfflineHashPageOpaqueData
{
	BlockNumber hasho_prevblkno;
	BlockNumber hasho_nextblkno;
	uint32 hasho_bucket;
	uint16 hasho_flag;
	uint16 hasho_page_id;
} OfflineHashPageOpaqueData;

typedef struct OfflineGistPageOpaqueData
{
	PageXLogRecPtr nsn;
	BlockNumber rightlink;
	uint16 flags;
	uint16 gist_page_id;
} OfflineGistPageOpaqueData;

typedef struct OfflineGistPageUpdate
{
	uint16 ntodelete;
	uint16 ntoinsert;
} OfflineGistPageUpdate;

typedef struct OfflineGinInsert
{
	uint16 flags;
} OfflineGinInsert;

typedef struct OfflineGinInsertEntry
{
	OffsetNumber offset;
	bool isDelete;
	IndexTupleData tuple;
} OfflineGinInsertEntry;

typedef struct OfflineSpgAddLeaf
{
	bool newPage;
	bool storesNulls;
	OffsetNumber offnumLeaf;
	OffsetNumber offnumHeadLeaf;
	OffsetNumber offnumParent;
	uint16 nodeI;
} OfflineSpgAddLeaf;

typedef struct OfflineSpgLeafTupleData
{
	unsigned int tupstate:2, size:30;
	uint16 t_info;
	ItemPointerData heapPtr;
} OfflineSpgLeafTupleData;

typedef struct OfflineSpgPageOpaqueData
{
	uint16 flags;
	uint16 nRedirection;
	uint16 nPlaceholder;
	uint16 page_id;
} OfflineSpgPageOpaqueData;

typedef struct OfflineSpgInnerTupleData
{
	unsigned int tupstate:2, allTheSame:1, nNodes:13, prefixSize:16;
	uint16 size;
} OfflineSpgInnerTupleData;

typedef struct OfflineSpgDeadTupleData
{
	unsigned int tupstate:2, size:30;
	uint16 t_info;
	ItemPointerData pointer;
	TransactionId xid;
} OfflineSpgDeadTupleData;

#if PG_VERSION_NUM >= 170000
#define OFFLINE_SPG_REDIRECT_XID(state) ((state).redirectXid)
#else
#define OFFLINE_SPG_REDIRECT_XID(state) ((state).myXid)
#endif

#define OFFLINE_XLOG_GIST_PAGE_UPDATE 0x00
#define OFFLINE_XLOG_GIST_DELETE 0x10
#define OFFLINE_XLOG_GIST_PAGE_SPLIT 0x30
#define OFFLINE_XLOG_GIST_PAGE_DELETE 0x60
#define OFFLINE_GIST_LEAF 0x01
#define OFFLINE_GIST_DELETED 0x02
#define OFFLINE_GIST_FOLLOW_RIGHT 0x08
#define OFFLINE_GIST_HAS_GARBAGE 0x10
#define OFFLINE_GIST_PAGE_ID 0xFF81
#define OFFLINE_HASH_NEEDS_SPLIT_CLEANUP 0x40
#define OFFLINE_HASH_HAS_DEAD_TUPLES 0x80
#define OFFLINE_XLOG_GIN_INSERT 0x20
#define OFFLINE_GIN_INSERT_ISDATA 0x01
#define OFFLINE_GIN_INSERT_ISLEAF 0x02
#define OFFLINE_XLOG_SPGIST_ADD_LEAF 0x10
#define OFFLINE_XLOG_BRIN_INSERT 0x10
#define OFFLINE_XLOG_BRIN_UPDATE 0x20
#define OFFLINE_XLOG_BRIN_SAMEPAGE_UPDATE 0x30
#define OFFLINE_XLOG_BRIN_REVMAP_EXTEND 0x40
#define OFFLINE_XLOG_BRIN_DESUMMARIZE 0x50
#define OFFLINE_XLOG_BRIN_OPMASK 0x70
#define OFFLINE_XLOG_BRIN_INIT_PAGE 0x80
#define OFFLINE_BRIN_META_MAGIC 0xA8109CFA
#define OFFLINE_BRIN_PAGETYPE_META 0xF091
#define OFFLINE_BRIN_PAGETYPE_REVMAP 0xF092
#define OFFLINE_BRIN_PAGETYPE_REGULAR 0xF093
#define OFFLINE_XLOG_SEQ_LOG 0x00
#define OFFLINE_SEQ_MAGIC 0x1717

typedef struct OfflineBrinInsert
{
	BlockNumber heapBlk;
	BlockNumber pagesPerRange;
	OffsetNumber offnum;
} OfflineBrinInsert;

typedef struct OfflineBrinSamepageUpdate
{
	OffsetNumber offnum;
} OfflineBrinSamepageUpdate;

typedef struct OfflineBrinUpdate
{
	OffsetNumber oldOffnum;
	OfflineBrinInsert insert;
} OfflineBrinUpdate;

typedef struct OfflineBrinMetaPageData
{
	uint32 brinMagic;
	uint32 brinVersion;
	BlockNumber pagesPerRange;
	BlockNumber lastRevmapPage;
} OfflineBrinMetaPageData;

typedef struct OfflineBrinCreateIndex
{
	BlockNumber pagesPerRange;
	uint16 version;
} OfflineBrinCreateIndex;

typedef struct OfflineBrinDesummarize
{
	BlockNumber pagesPerRange;
	BlockNumber heapBlk;
	OffsetNumber regOffset;
} OfflineBrinDesummarize;

typedef struct OfflineGistPageSplit
{
	BlockNumber origrlink;
	XLogRecPtr orignsn;
	bool origleaf;
	uint16 npage;
	bool markfollowright;
} OfflineGistPageSplit;

typedef struct OfflineGistPageDelete
{
	FullTransactionId deleteXid;
	OffsetNumber downlinkOffset;
} OfflineGistPageDelete;

static bool
get_block_tag(XLogReaderState *record, uint8 block_id,
			  RelFileLocator *rnode, ForkNumber *forknum, BlockNumber *blkno)
{
#if PG_VERSION_NUM >= 150000
	return XLogRecGetBlockTagExtended(record, block_id,
									   rnode, forknum, blkno, NULL);
#else
	return XLogRecGetBlockTag(record, block_id, rnode, forknum, blkno);
#endif
}

typedef struct ReplayPrivate
{
	TimeLineID	timeline;
} ReplayPrivate;

typedef struct ReplayOptions
{
	char	   *waldir;
	char	   *fpi_path;
	char	   *out_path;

	RelFileLocator rnode;
	BlockNumber blkno;

	XLogRecPtr	start_lsn;
	XLogRecPtr	stop_lsn;

	TimeLineID	timeline;
	int			segsize;
	bool		has_relation;
	bool		force_checksums;
} ReplayOptions;

static bool input_has_checksum;

static void
set_prunable(Page page, TransactionId xid)
{
	PageHeader phdr = (PageHeader) page;

	if (TransactionIdIsNormal(xid) &&
		(!TransactionIdIsValid(phdr->pd_prune_xid) ||
		 (int32) (xid - phdr->pd_prune_xid) < 0))
		phdr->pd_prune_xid = xid;
}

static void die(const char *fmt,...) pg_attribute_printf(1, 2);

static void
die(const char *fmt,...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

static bool
parse_lsn(const char *str, XLogRecPtr *lsn)
{
	uint32		hi;
	uint32		lo;

	if (sscanf(str, "%X/%X", &hi, &lo) != 2)
		return false;

	*lsn = ((uint64) hi << 32) | lo;
	return true;
}

static bool
parse_relation(const char *str, RelFileLocator *rnode)
{
	unsigned int spc;
	unsigned int db;
	unsigned int rel;

	if (sscanf(str, "%u/%u/%u", &spc, &db, &rel) != 3)
		return false;

	rnode->spcOid = spc;
	rnode->dbOid = db;
	rnode->relNumber = rel;

	return true;
}

static bool
same_rnode(RelFileLocator a, RelFileLocator b)
{
	return a.spcOid == b.spcOid &&
		   a.dbOid == b.dbOid &&
		   a.relNumber == b.relNumber;
}

static void
format_lsn(char *buf, size_t buflen, XLogRecPtr lsn)
{
	snprintf(buf, buflen, "%X/%08X",
			 (uint32) (lsn >> 32),
			 (uint32) lsn);
}

static int
open_file_in_directory(const char *directory, const char *fname)
{
	char		path[MAXPGPATH];
	int			fd;

	snprintf(path, sizeof(path), "%s/%s", directory, fname);

	fd = open(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		die("could not open WAL file \"%s\": %s", path, strerror(errno));

	return fd;
}

static void
ReplayOpenSegment(XLogReaderState *state, XLogSegNo nextSegNo,
				  TimeLineID *tli_p)
{
	TimeLineID	tli = *tli_p;
	char		fname[MAXPGPATH];

	XLogFileName(fname, tli, nextSegNo, state->segcxt.ws_segsize);

	state->seg.ws_file = open_file_in_directory(state->segcxt.ws_dir, fname);
	state->seg.ws_segno = nextSegNo;
	state->seg.ws_tli = tli;
}

static void
ReplayCloseSegment(XLogReaderState *state)
{
	if (state->seg.ws_file >= 0)
		close(state->seg.ws_file);

	state->seg.ws_file = -1;
}

static int
ReplayReadPage(XLogReaderState *state,
			   XLogRecPtr targetPagePtr,
			   int reqLen,
			   XLogRecPtr targetRecPtr,
			   char *readBuff)
{
	ReplayPrivate *private = (ReplayPrivate *) state->private_data;
	WALReadError errinfo;
	int			count = XLOG_BLCKSZ;

	if (!WALRead(state, readBuff, targetPagePtr, count,
				 private->timeline, &errinfo))
	{
		WALOpenSegment *seg = &errinfo.wre_seg;
		char		fname[MAXPGPATH];

		XLogFileName(fname, seg->ws_tli, seg->ws_segno,
					 state->segcxt.ws_segsize);

		if (errinfo.wre_errno != 0)
		{
			errno = errinfo.wre_errno;
			die("could not read WAL file \"%s\", offset %d: %s",
				fname, errinfo.wre_off, strerror(errno));
		}
		else
		{
			die("could not read WAL file \"%s\", offset %d: read %d of %d",
				fname, errinfo.wre_off, errinfo.wre_read, errinfo.wre_req);
		}
	}

	return count;
}

static void
read_input_page(const char *path, Page page)
{
	FILE	   *f;

	f = fopen(path, PG_BINARY_R);
	if (f == NULL)
		die("could not open FPI/input page file \"%s\": %s", path, strerror(errno));

	if (fread(page, BLCKSZ, 1, f) != 1 || fgetc(f) != EOF)
		die("input file \"%s\" must contain exactly one %d-byte page",
			path, BLCKSZ);

	if (fclose(f) != 0)
		die("could not close FPI/input page file \"%s\": %s", path, strerror(errno));
}

static void
write_page(const char *path, Page page)
{
	FILE	   *f;

	f = fopen(path, PG_BINARY_W);
	if (f == NULL)
		die("could not open output file \"%s\": %s", path, strerror(errno));

	if (fwrite(page, BLCKSZ, 1, f) != 1)
		die("could not write 8KB page to \"%s\"", path);

	if (fclose(f) != 0)
		die("could not close output file \"%s\": %s", path, strerror(errno));
}

static void
check_page_pointers(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
	{
		die("corrupted page pointers: lower=%u upper=%u special=%u",
			phdr->pd_lower, phdr->pd_upper, phdr->pd_special);
	}
}

static void
init_heap_page_offline(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	memset(page, 0, BLCKSZ);

	phdr->pd_lower = SizeOfPageHeaderData;
	phdr->pd_upper = BLCKSZ;
	phdr->pd_special = BLCKSZ;
	PageSetPageSizeAndVersion(page, BLCKSZ, PG_PAGE_LAYOUT_VERSION);
}

static OffsetNumber
add_heap_item(Page page, Item item, Size size, OffsetNumber offnum)
{
	PageHeader	phdr = (PageHeader) page;
	OffsetNumber limit;
	Size		aligned_size;
	int			lower;
	int			upper;
	ItemId		item_id;

	check_page_pointers(page);

	limit = OffsetNumberNext(PageGetMaxOffsetNumber(page));
	if (offnum < FirstOffsetNumber || offnum > limit)
		die("invalid insertion offset %u (next is %u)", offnum, limit);
	if (offnum < limit && ItemIdIsUsed(PageGetItemId(page, offnum)))
		die("insertion offset %u is already in use", offnum);

	if (offnum > MaxHeapTuplesPerPage)
		die("too many heap tuples on page: offnum=%u", offnum);

	lower = phdr->pd_lower + (offnum == limit ? sizeof(ItemIdData) : 0);
	aligned_size = MAXALIGN(size);
	upper = (int) phdr->pd_upper - (int) aligned_size;

	if (lower > upper)
		die("not enough free space on page: lower=%d upper=%d tuple_size=%zu",
			lower, upper, size);

	item_id = PageGetItemId(page, offnum);

	ItemIdSetNormal(item_id, upper, size);
	memcpy((char *) page + upper, item, size);

	phdr->pd_lower = (LocationIndex) lower;
	phdr->pd_upper = (LocationIndex) upper;

	return offnum;
}

/* B-tree insertions shuffle line pointers instead of reusing heap slots. */
static void
add_btree_item(Page page, Item item, Size size, OffsetNumber offnum)
{
	PageHeader phdr = (PageHeader) page;
	OffsetNumber limit;
	ItemId itemid;
	int lower;
	int upper;

	check_page_pointers(page);
	limit = OffsetNumberNext(PageGetMaxOffsetNumber(page));
	if (offnum < FirstOffsetNumber || offnum > limit)
		die("invalid B-tree insertion offset %u (next is %u)", offnum, limit);
	if (size == 0 || size > BLCKSZ)
		die("invalid B-tree tuple length: %zu", size);

	lower = phdr->pd_lower + sizeof(ItemIdData);
	upper = (int) phdr->pd_upper - (int) MAXALIGN(size);
	if (lower > upper)
		die("not enough free space for B-tree tuple at offset %u", offnum);

	itemid = PageGetItemId(page, offnum);
	if (offnum < limit)
		memmove(itemid + 1, itemid, (limit - offnum) * sizeof(ItemIdData));
	ItemIdSetNormal(itemid, upper, size);
	memcpy((char *) page + upper, item, size);
	phdr->pd_lower = (LocationIndex) lower;
	phdr->pd_upper = (LocationIndex) upper;
}

static void
overwrite_index_item(Page page, OffsetNumber offnum, Item item, Size size)
{
	PageHeader phdr = (PageHeader) page;
	ItemId target;
	int oldsize;
	int size_diff;
	Size newsize = MAXALIGN(size);
	unsigned offset;
	OffsetNumber off;

	check_page_pointers(page);
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		die("invalid index tuple overwrite offset %u", offnum);
	target = PageGetItemId(page, offnum);
	if (!ItemIdHasStorage(target))
		die("index tuple overwrite target has no storage");
	offset = ItemIdGetOffset(target);
	oldsize = MAXALIGN(ItemIdGetLength(target));
	if (offset < phdr->pd_upper || offset + oldsize > phdr->pd_special ||
		offset != MAXALIGN(offset))
		die("corrupted index tuple overwrite target");
	if (newsize > (Size) (oldsize + phdr->pd_upper - phdr->pd_lower))
		die("not enough free space to overwrite index tuple");
	size_diff = oldsize - (int) newsize;
	if (size_diff != 0)
	{
		memmove(page + phdr->pd_upper + size_diff,
				page + phdr->pd_upper, offset - phdr->pd_upper);
		phdr->pd_upper += size_diff;
		for (off = FirstOffsetNumber; off <= PageGetMaxOffsetNumber(page); off++)
		{
			ItemId ii = PageGetItemId(page, off);
			if (ItemIdHasStorage(ii) && ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size_diff;
		}
	}
	target->lp_off = offset + size_diff;
	target->lp_len = size;
	memcpy(PageGetItem(page, target), item, size);
}

typedef struct OfflineIndexItem
{
	OffsetNumber original_offnum;
	LocationIndex original_offset;
	Size aligned_size;
} OfflineIndexItem;

static int
index_item_physical_order(const void *a, const void *b)
{
	const OfflineIndexItem *left = a;
	const OfflineIndexItem *right = b;

	return (int) right->original_offset - (int) left->original_offset;
}

static void
delete_index_items(Page page, const char *offsets, int ndelete)
{
	PageHeader phdr = (PageHeader) page;
	OffsetNumber nline;
	OfflineIndexItem items[BLCKSZ / sizeof(ItemIdData)];
	ItemIdData newids[BLCKSZ / sizeof(ItemIdData)];
	bool remove[BLCKSZ / sizeof(ItemIdData)] = {false};
	char original[BLCKSZ];
	int nused = 0;
	int i;
	int new_upper;

	check_page_pointers(page);
	nline = PageGetMaxOffsetNumber(page);
	if (nline >= lengthof(items) || ndelete < 0 || ndelete > nline)
		die("invalid index deletion count");
	for (i = 0; i < ndelete; i++)
	{
		OffsetNumber off;

		memcpy(&off, offsets + i * sizeof(off), sizeof(off));
		if (off < FirstOffsetNumber || off > nline || remove[off])
			die("invalid or duplicate index deletion offset %u", off);
		remove[off] = true;
	}
	memcpy(original, page, BLCKSZ);
	for (i = FirstOffsetNumber; i <= nline; i++)
	{
		ItemId oldid = PageGetItemId((Page) original, i);
		Size size = ItemIdGetLength(oldid);
		LocationIndex offset = ItemIdGetOffset(oldid);

		if (!ItemIdHasStorage(oldid) ||
			offset < phdr->pd_upper || offset + size > phdr->pd_special ||
			offset != MAXALIGN(offset))
			die("invalid index tuple at offset %d", i);
		if (!remove[i])
		{
			newids[nused] = *oldid;
			items[nused].original_offnum = nused;
			items[nused].original_offset = offset;
			items[nused].aligned_size = MAXALIGN(size);
			nused++;
		}
	}
	/* PostgreSQL uses retail deletion for at most two offsets, preserving
	 * physical order.  The multi-delete path packs tuples in item number
	 * order, which can differ from physical order on a Hash page. */
	if (ndelete <= 2)
		qsort(items, nused, sizeof(items[0]), index_item_physical_order);
	new_upper = phdr->pd_special;
	for (i = 0; i < nused; i++)
	{
		OfflineIndexItem *entry = &items[i];

		new_upper -= entry->aligned_size;
		if (new_upper < phdr->pd_lower)
			die("invalid index tuple lengths during deletion");
		memcpy(page + new_upper,
			   original + entry->original_offset, entry->aligned_size);
		newids[entry->original_offnum].lp_off = new_upper;
	}
	memcpy(phdr->pd_linp, newids, nused * sizeof(ItemIdData));
	phdr->pd_lower = SizeOfPageHeaderData + nused * sizeof(ItemIdData);
	phdr->pd_upper = new_upper;
}

static void
delete_index_item_no_compact(Page page, OffsetNumber offnum)
{
	PageHeader phdr = (PageHeader) page;
	ItemId itemid;
	Size size;
	unsigned offset;
	OffsetNumber maxoff;
	OffsetNumber off;

	check_page_pointers(page);
	maxoff = PageGetMaxOffsetNumber(page);
	if (offnum < FirstOffsetNumber || offnum > maxoff)
		die("invalid BRIN deletion offset %u", offnum);
	itemid = PageGetItemId(page, offnum);
	if (!ItemIdHasStorage(itemid))
		die("BRIN deletion target has no storage");
	size = MAXALIGN(ItemIdGetLength(itemid));
	offset = ItemIdGetOffset(itemid);
	if (offset < phdr->pd_upper || offset + size > phdr->pd_special ||
		offset != MAXALIGN(offset))
		die("invalid BRIN deletion target bounds");
	if (offnum < maxoff)
		ItemIdSetUnused(itemid);
	else
	{
		phdr->pd_lower -= sizeof(ItemIdData);
		maxoff--;
	}
	if (offset > phdr->pd_upper)
		memmove(page + phdr->pd_upper + size,
				page + phdr->pd_upper, offset - phdr->pd_upper);
	phdr->pd_upper += size;
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId ii = PageGetItemId(page, off);
		if (ItemIdHasStorage(ii) && ItemIdGetOffset(ii) <= offset)
			ii->lp_off += size;
	}
}

static void
init_index_page_offline(Page page, Size special_size)
{
	PageHeader phdr = (PageHeader) page;
	Size special = BLCKSZ - MAXALIGN(special_size);

	memset(page, 0, BLCKSZ);
	phdr->pd_lower = SizeOfPageHeaderData;
	phdr->pd_upper = special;
	phdr->pd_special = special;
	PageSetPageSizeAndVersion(page, BLCKSZ, PG_PAGE_LAYOUT_VERSION);
}

static void
restore_btree_items(Page page, const char *data, Size datalen)
{
	const char *items[BLCKSZ / sizeof(IndexTupleData)];
	Size sizes[BLCKSZ / sizeof(IndexTupleData)];
	Size pos = 0;
	int nitems = 0;
	int i;

	while (pos < datalen)
	{
		IndexTupleData itup;
		Size size;

		if (datalen - pos < sizeof(itup) ||
			nitems >= (int) lengthof(items))
			die("invalid B-tree split tuple stream");
		memcpy(&itup, data + pos, sizeof(itup));
		size = MAXALIGN(IndexTupleSize(&itup));
		if (size < sizeof(itup) || size > datalen - pos)
			die("invalid B-tree split tuple length");
		items[nitems] = data + pos;
		sizes[nitems++] = size;
		pos += size;
	}

	for (i = nitems - 1; i >= 0; i--)
		add_btree_item(page, (Item) items[i], sizes[i], nitems - i);
}

static BlockNumber
btree_block_number(XLogReaderState *record, uint8 id)
{
	BlockNumber blkno;

	if (!get_block_tag(record, id, NULL, NULL, &blkno))
		die("B-tree WAL record is missing block reference %u", id);
	return blkno;
}

static void
apply_btree_insert(XLogReaderState *record, Page page, int block_id,
			   const char *name)
{
	xl_btree_insert *xlrec;
	IndexTupleData itup;
	Size mainlen = XLogRecGetDataLen(record);
	Size datalen;
	char *data;
	char lsnbuf[64];

	if (block_id != 0 || mainlen < SizeOfBtreeInsert)
		die("invalid B-tree %s record for target block", name);
	xlrec = (xl_btree_insert *) XLogRecGetData(record);
	data = XLogRecGetBlockData(record, 0, &datalen);
	if (data == NULL || datalen < sizeof(IndexTupleData))
		die("B-tree %s tuple data missing or too short", name);
	memcpy(&itup, data, sizeof(itup));
	if (IndexTupleSize(&itup) != datalen)
		die("B-tree %s tuple length mismatch", name);

	add_btree_item(page, (Item) data, datalen, xlrec->offnum);
	PageSetLSN(page, record->EndRecPtr);
	format_lsn(lsnbuf, sizeof(lsnbuf), record->ReadRecPtr);
	printf("applied Btree %s lsn=%s off=%u tuple_len=%zu\n",
		   name, lsnbuf, (unsigned int) xlrec->offnum, datalen);
}

#define OFFLINE_BT_IS_POSTING 0x2000
#define OFFLINE_BT_OFFSET_MASK 0x0FFF
#define OFFLINE_INDEX_ALT_TID_MASK INDEX_AM_RESERVED_BIT

typedef struct OfflineBTDedupInterval
{
	OffsetNumber baseoff;
	uint16 nitems;
} OfflineBTDedupInterval;

static bool
offline_btree_is_posting(IndexTuple tuple)
{
	return (tuple->t_info & OFFLINE_INDEX_ALT_TID_MASK) != 0 &&
		(ItemPointerGetOffsetNumberNoCheck(&tuple->t_tid) &
		 OFFLINE_BT_IS_POSTING) != 0;
}

static uint16
offline_btree_posting_count(IndexTuple tuple)
{
	return ItemPointerGetOffsetNumberNoCheck(&tuple->t_tid) &
		OFFLINE_BT_OFFSET_MASK;
}

static uint32
offline_btree_posting_offset(IndexTuple tuple)
{
	return ItemPointerGetBlockNumberNoCheck(&tuple->t_tid);
}

static ItemPointer
offline_btree_posting_tids(IndexTuple tuple)
{
	return (ItemPointer) ((char *) tuple +
			offline_btree_posting_offset(tuple));
}

static void
apply_btree_insert_post(XLogReaderState *record, Page page, int block_id)
{
	xl_btree_insert header;
	char *data;
	Size len;
	uint16 postingoff;
	OffsetNumber oldoff;
	ItemId itemid;
	IndexTuple oldtuple;
	IndexTuple newtuple;
	ItemPointer tids;
	ItemPointerData oldmax;
	ItemPointerData newtid;
	uint16 count;
	char *newbuf;

	if (block_id != 0 || XLogRecGetDataLen(record) < SizeOfBtreeInsert)
		die("invalid B-tree INSERT_POST WAL header");
	memcpy(&header, XLogRecGetData(record), SizeOfBtreeInsert);
	data = XLogRecGetBlockData(record, 0, &len);
	if (data == NULL || len < sizeof(uint16) + sizeof(IndexTupleData))
		die("truncated B-tree INSERT_POST payload");
	memcpy(&postingoff, data, sizeof(postingoff));
	oldoff = OffsetNumberPrev(header.offnum);
	if (oldoff < FirstOffsetNumber || oldoff > PageGetMaxOffsetNumber(page))
		die("invalid B-tree INSERT_POST old offset");
	itemid = PageGetItemId(page, oldoff);
	oldtuple = (IndexTuple) PageGetItem(page, itemid);
	if (!offline_btree_is_posting(oldtuple))
		die("B-tree INSERT_POST target is not a posting tuple");
	count = offline_btree_posting_count(oldtuple);
	if (postingoff == 0 || postingoff >= count)
		die("invalid B-tree INSERT_POST split point");
	newbuf = malloc(len - sizeof(uint16));
	if (newbuf == NULL)
		die("out of memory for B-tree INSERT_POST");
	memcpy(newbuf, data + sizeof(uint16), len - sizeof(uint16));
	newtuple = (IndexTuple) newbuf;
	if (IndexTupleSize(newtuple) != len - sizeof(uint16))
		die("B-tree INSERT_POST new tuple length mismatch");
	newtid = newtuple->t_tid;
	tids = offline_btree_posting_tids(oldtuple);
	oldmax = tids[count - 1];
	memmove(&tids[postingoff + 1], &tids[postingoff],
			(count - postingoff - 1) * sizeof(ItemPointerData));
	tids[postingoff] = newtid;
	newtuple->t_tid = oldmax;
	add_btree_item(page, (Item) newtuple, len - sizeof(uint16),
				   header.offnum);
	free(newbuf);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_btree_dedup(XLogReaderState *record, Page page, int block_id)
{
	xl_btree_dedup header;
	OfflineBTDedupInterval *intervals;
	char *data;
	Size len;
	Page newpage;
	Size special;
	OffsetNumber off;
	OffsetNumber minoff;
	OffsetNumber maxoff;
	uint16 current = 0;
	OfflineBTPageOpaqueData *opaque;

	if (block_id != 0 || XLogRecGetDataLen(record) < SizeOfBtreeDedup)
		die("invalid B-tree DEDUP WAL header");
	memcpy(&header, XLogRecGetData(record), SizeOfBtreeDedup);
	data = XLogRecGetBlockData(record, 0, &len);
	if (header.nintervals > MaxIndexTuplesPerPage ||
		len != (Size) header.nintervals * sizeof(OfflineBTDedupInterval) ||
		(len != 0 && data == NULL))
		die("invalid B-tree DEDUP interval payload");
	intervals = (OfflineBTDedupInterval *) data;
	special = BLCKSZ - ((PageHeader) page)->pd_special;
	newpage = malloc(BLCKSZ);
	if (newpage == NULL)
		die("out of memory for B-tree DEDUP");
	init_index_page_offline(newpage, special);
	memcpy(PageGetSpecialPointer(newpage), PageGetSpecialPointer(page),
		   special);
	opaque = OfflineBTPageGetOpaque(page);
	minoff = opaque->btpo_next == OFFLINE_P_NONE ? 1 : 2;
	maxoff = PageGetMaxOffsetNumber(page);
	if (minoff == 2)
	{
		ItemId itemid = PageGetItemId(page, 1);

		add_btree_item(newpage, PageGetItem(page, itemid),
					   ItemIdGetLength(itemid), 1);
	}
	for (off = minoff; off <= maxoff; off++)
	{
		ItemId itemid = PageGetItemId(page, off);
		IndexTuple tuple = (IndexTuple) PageGetItem(page, itemid);

		if (current < header.nintervals && intervals[current].baseoff == off)
		{
			ItemPointerData tids[BLCKSZ / sizeof(ItemPointerData) + 1];
			Size ntids = 0;
			Size keysize;
			Size newsize;
			IndexTuple merged;
			uint16 j;

			if (intervals[current].nitems < 2 ||
				off + intervals[current].nitems - 1 > maxoff)
				die("invalid B-tree DEDUP interval bounds");
			keysize = offline_btree_is_posting(tuple) ?
				offline_btree_posting_offset(tuple) : IndexTupleSize(tuple);
			for (j = 0; j < intervals[current].nitems; j++)
			{
				IndexTuple source = (IndexTuple) PageGetItem(page,
					PageGetItemId(page, off + j));
				uint16 count = offline_btree_is_posting(source) ?
					offline_btree_posting_count(source) : 1;
				ItemPointer source_tids = offline_btree_is_posting(source) ?
					offline_btree_posting_tids(source) : &source->t_tid;

				if (ntids + count > lengthof(tids))
					die("too many B-tree DEDUP heap TIDs");
				memcpy(tids + ntids, source_tids,
					   count * sizeof(ItemPointerData));
				ntids += count;
			}
			newsize = MAXALIGN(keysize + ntids * sizeof(ItemPointerData));
			if (ntids < 2 || ntids > OFFLINE_BT_OFFSET_MASK ||
				newsize > INDEX_SIZE_MASK)
				die("invalid B-tree DEDUP posting tuple size");
			merged = calloc(1, newsize);
			if (merged == NULL)
				die("out of memory for B-tree DEDUP tuple");
			memcpy(merged, tuple, keysize);
			merged->t_info = (merged->t_info & ~INDEX_SIZE_MASK) | newsize;
			merged->t_info |= OFFLINE_INDEX_ALT_TID_MASK;
			ItemPointerSetOffsetNumber(&merged->t_tid,
								   (OffsetNumber) (ntids | OFFLINE_BT_IS_POSTING));
			ItemPointerSetBlockNumber(&merged->t_tid, keysize);
			memcpy((char *) merged + keysize, tids,
				   ntids * sizeof(ItemPointerData));
			add_btree_item(newpage, (Item) merged, newsize,
					   OffsetNumberNext(PageGetMaxOffsetNumber(newpage)));
			free(merged);
			off += intervals[current].nitems - 1;
			current++;
		}
		else
		{
			if (current < header.nintervals &&
				intervals[current].baseoff < off)
				die("overlapping B-tree DEDUP intervals");
			add_btree_item(newpage, (Item) tuple, ItemIdGetLength(itemid),
					   OffsetNumberNext(PageGetMaxOffsetNumber(newpage)));
		}
	}
	if (current != header.nintervals)
		die("unconsumed B-tree DEDUP intervals");
	OfflineBTPageGetOpaque(newpage)->btpo_flags &= ~OFFLINE_BTP_HAS_GARBAGE;
	memcpy(page, newpage, BLCKSZ);
	free(newpage);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_btree_clear_incomplete_split(Page page)
{
	OfflineBTPageOpaqueData *opaque = OfflineBTPageGetOpaque(page);

	if (!(opaque->btpo_flags & OFFLINE_BTP_INCOMPLETE_SPLIT))
		die("B-tree child page lacks INCOMPLETE_SPLIT flag");
	opaque->btpo_flags &= ~OFFLINE_BTP_INCOMPLETE_SPLIT;
}

static void
apply_btree_meta(XLogReaderState *record, Page page, int block_id)
{
	Size len;
	char *data = XLogRecGetBlockData(record, block_id, &len);
	xl_btree_metadata xlrec;
	OfflineBTMetaPageData *meta;
	OfflineBTPageOpaqueData *opaque;

	if (data == NULL || len != sizeof(xlrec))
		die("invalid B-tree metadata WAL payload");
	memcpy(&xlrec, data, sizeof(xlrec));
	init_index_page_offline(page, sizeof(OfflineBTPageOpaqueData));
	meta = (OfflineBTMetaPageData *) PageGetContents(page);
	meta->btm_magic = OFFLINE_BTREE_MAGIC;
	meta->btm_version = xlrec.version;
	meta->btm_root = xlrec.root;
	meta->btm_level = xlrec.level;
	meta->btm_fastroot = xlrec.fastroot;
	meta->btm_fastlevel = xlrec.fastlevel;
	meta->btm_last_cleanup_num_delpages = xlrec.last_cleanup_num_delpages;
	meta->btm_last_cleanup_num_heap_tuples = -1.0;
	meta->btm_allequalimage = xlrec.allequalimage;
	opaque = OfflineBTPageGetOpaque(page);
	opaque->btpo_flags = OFFLINE_BTP_META;
	((PageHeader) page)->pd_lower =
		(char *) (meta + 1) - (char *) page;
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_btree_split(XLogReaderState *record, Page page, int block_id,
				  bool newitemonleft)
{
	xl_btree_split *xlrec;
	Size datalen;
	char *data;
	BlockNumber leftblk;
	BlockNumber rightblk;
	BlockNumber nextblk = OFFLINE_P_NONE;
	BlockNumber actual;
	OfflineBTPageOpaqueData *opaque;

	if (XLogRecGetDataLen(record) < SizeOfBtreeSplit)
		die("invalid B-tree SPLIT WAL header");
	xlrec = (xl_btree_split *) XLogRecGetData(record);
	leftblk = btree_block_number(record, 0);
	rightblk = btree_block_number(record, 1);
	if (get_block_tag(record, 2, NULL, NULL, &actual))
		nextblk = actual;

	if (block_id == 1)
	{
		data = XLogRecGetBlockData(record, 1, &datalen);
		if (data == NULL && datalen != 0)
			die("missing B-tree split right-page tuples");
		init_index_page_offline(page, sizeof(OfflineBTPageOpaqueData));
		opaque = OfflineBTPageGetOpaque(page);
		opaque->btpo_prev = leftblk;
		opaque->btpo_next = nextblk;
		opaque->btpo_level = xlrec->level;
		opaque->btpo_flags = xlrec->level == 0 ? OFFLINE_BTP_LEAF : 0;
		restore_btree_items(page, data, datalen);
	}
	else if (block_id == 0)
	{
		char replacement[BLCKSZ];
		OfflineBTPageOpaqueData *newopaque;
		IndexTupleData itup;
		Size size;
		OffsetNumber off;
		OffsetNumber leftoff = 1;
		OffsetNumber replacepostingoff = InvalidOffsetNumber;
		Size pos = 0;
		Size special;
		Size newitemsz = 0;
		char *newbuf = NULL;
		char *postingbuf = NULL;

		data = XLogRecGetBlockData(record, 0, &datalen);
		if (data == NULL || datalen < sizeof(itup))
			die("missing B-tree split left-page data");
		check_page_pointers(page);
		special = BLCKSZ - ((PageHeader) page)->pd_special;
		if (special != MAXALIGN(sizeof(OfflineBTPageOpaqueData)))
			die("invalid B-tree page special area");
		init_index_page_offline(replacement, special);
		memcpy(replacement + BLCKSZ - special, page + BLCKSZ - special,
			   special);

		if (newitemonleft || xlrec->postingoff != 0)
		{
			memcpy(&itup, data, sizeof(itup));
			size = MAXALIGN(IndexTupleSize(&itup));
			if (size < sizeof(itup) || size > datalen)
				die("invalid B-tree split new tuple length");
			newitemsz = size;
			if (xlrec->postingoff != 0)
			{
				ItemId oldid;
				IndexTuple oldtuple;
				IndexTuple newtuple;
				IndexTuple postingtuple;
				ItemPointer tids;
				ItemPointerData oldmax;
				ItemPointerData newtid;
				uint16 count;

				replacepostingoff = OffsetNumberPrev(xlrec->newitemoff);
				if (replacepostingoff < FirstOffsetNumber ||
					replacepostingoff > PageGetMaxOffsetNumber(page))
					die("invalid B-tree SPLIT posting offset");
				oldid = PageGetItemId(page, replacepostingoff);
				oldtuple = (IndexTuple) PageGetItem(page, oldid);
				if (!offline_btree_is_posting(oldtuple))
					die("B-tree SPLIT source is not a posting tuple");
				count = offline_btree_posting_count(oldtuple);
				if (xlrec->postingoff >= count)
					die("invalid B-tree SPLIT posting split point");
				newbuf = malloc(size);
				postingbuf = malloc(IndexTupleSize(oldtuple));
				if (newbuf == NULL || postingbuf == NULL)
					die("out of memory for B-tree SPLIT posting list");
				memcpy(newbuf, data, size);
				memcpy(postingbuf, oldtuple, IndexTupleSize(oldtuple));
				newtuple = (IndexTuple) newbuf;
				postingtuple = (IndexTuple) postingbuf;
				newtid = newtuple->t_tid;
				tids = offline_btree_posting_tids(postingtuple);
				oldmax = tids[count - 1];
				memmove(&tids[xlrec->postingoff + 1],
						&tids[xlrec->postingoff],
						(count - xlrec->postingoff - 1) * sizeof(ItemPointerData));
				tids[xlrec->postingoff] = newtid;
				newtuple->t_tid = oldmax;
			}
			pos += size;
		}
		if (datalen - pos < sizeof(itup))
			die("missing B-tree split high key");
		memcpy(&itup, data + pos, sizeof(itup));
		size = MAXALIGN(IndexTupleSize(&itup));
		if (size < sizeof(itup) || size != datalen - pos)
			die("invalid B-tree split high key length");
		add_btree_item(replacement, (Item) (data + pos), size, leftoff++);

		opaque = OfflineBTPageGetOpaque(page);
		for (off = opaque->btpo_next == OFFLINE_P_NONE ? 1 : 2;
			 off < xlrec->firstrightoff; off++)
		{
			ItemId itemid;
			if (off == replacepostingoff)
			{
				add_btree_item(replacement, (Item) postingbuf,
							   IndexTupleSize((IndexTuple) postingbuf),
							   leftoff++);
				continue;
			}
			if (newitemonleft && off == xlrec->newitemoff)
			{
				add_btree_item(replacement,
							   (Item) (newbuf != NULL ? newbuf : data),
							   newitemsz, leftoff++);
			}
			itemid = PageGetItemId(page, off);
			if (!ItemIdIsNormal(itemid))
				die("invalid B-tree split source item at offset %u", off);
			add_btree_item(replacement, PageGetItem(page, itemid),
						   ItemIdGetLength(itemid), leftoff++);
		}
		if (newitemonleft && off == xlrec->newitemoff)
			add_btree_item(replacement,
						   (Item) (newbuf != NULL ? newbuf : data),
						   newitemsz, leftoff++);
		free(newbuf);
		free(postingbuf);

		memcpy(page, replacement, BLCKSZ);
		newopaque = OfflineBTPageGetOpaque(page);
		newopaque->btpo_flags = OFFLINE_BTP_INCOMPLETE_SPLIT |
			(xlrec->level == 0 ? OFFLINE_BTP_LEAF : 0);
		newopaque->btpo_next = rightblk;
		newopaque->btpo_cycleid = 0;
	}
	else if (block_id == 2)
	{
		opaque = OfflineBTPageGetOpaque(page);
		opaque->btpo_prev = rightblk;
	}
	else if (block_id == 3 && xlrec->level > 0)
		apply_btree_clear_incomplete_split(page);
	else
		die("invalid B-tree SPLIT target block reference %d", block_id);

	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_btree_newroot(XLogReaderState *record, Page page, int block_id)
{
	xl_btree_newroot *xlrec;
	OfflineBTPageOpaqueData *opaque;
	Size len;
	char *data;

	if (XLogRecGetDataLen(record) < SizeOfBtreeNewroot)
		die("invalid B-tree NEWROOT WAL header");
	xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	if (block_id == 1 && xlrec->level > 0)
		apply_btree_clear_incomplete_split(page);
	else if (block_id == 2)
	{
		apply_btree_meta(record, page, block_id);
		return;
	}
	else if (block_id == 0)
	{
		init_index_page_offline(page, sizeof(OfflineBTPageOpaqueData));
		opaque = OfflineBTPageGetOpaque(page);
		opaque->btpo_flags = OFFLINE_BTP_ROOT |
			(xlrec->level == 0 ? OFFLINE_BTP_LEAF : 0);
		opaque->btpo_prev = opaque->btpo_next = OFFLINE_P_NONE;
		opaque->btpo_level = xlrec->level;
		if (xlrec->level > 0)
		{
			data = XLogRecGetBlockData(record, 0, &len);
			if (data == NULL || len == 0)
				die("B-tree NEWROOT is missing tuples");
			restore_btree_items(page, data, len);
		}
	}
	else
		die("invalid B-tree NEWROOT target block reference %d", block_id);
	PageSetLSN(page, record->EndRecPtr);
}

static void
init_btree_halfdead_page(Page page, BlockNumber left, BlockNumber right,
						BlockNumber topparent)
{
	OfflineBTPageOpaqueData *opaque;
	IndexTupleData highkey;

	init_index_page_offline(page, sizeof(OfflineBTPageOpaqueData));
	opaque = OfflineBTPageGetOpaque(page);
	opaque->btpo_prev = left;
	opaque->btpo_next = right;
	opaque->btpo_flags = OFFLINE_BTP_HALF_DEAD | OFFLINE_BTP_LEAF;
	memset(&highkey, 0, sizeof(highkey));
	highkey.t_info = sizeof(highkey) | INDEX_AM_RESERVED_BIT;
	ItemPointerSetBlockNumber(&highkey.t_tid, topparent);
	ItemPointerSetOffsetNumber(&highkey.t_tid, 0);
	add_btree_item(page, (Item) &highkey, sizeof(highkey),
				   FirstOffsetNumber);
}

static void
apply_btree_mark_halfdead(XLogReaderState *record, Page page, int block_id)
{
	xl_btree_mark_page_halfdead *xlrec;

	if (XLogRecGetDataLen(record) < SizeOfBtreeMarkPageHalfDead)
		die("invalid B-tree MARK_PAGE_HALFDEAD WAL header");
	xlrec = (xl_btree_mark_page_halfdead *) XLogRecGetData(record);
	if (block_id == 0)
		init_btree_halfdead_page(page, xlrec->leftblk,
						 xlrec->rightblk, xlrec->topparent);
	else if (block_id == 1)
	{
		OffsetNumber nextoff = OffsetNumberNext(xlrec->poffset);
		ItemId parentid;
		ItemId nextid;
		IndexTuple parent;
		IndexTuple next;
		BlockNumber rightlink;

		check_page_pointers(page);
		if (xlrec->poffset < FirstOffsetNumber ||
			nextoff > PageGetMaxOffsetNumber(page))
			die("invalid B-tree MARK_PAGE_HALFDEAD parent offsets");
		parentid = PageGetItemId(page, xlrec->poffset);
		nextid = PageGetItemId(page, nextoff);
		if (!ItemIdIsNormal(parentid) || !ItemIdIsNormal(nextid))
			die("invalid B-tree MARK_PAGE_HALFDEAD parent items");
		parent = (IndexTuple) PageGetItem(page, parentid);
		next = (IndexTuple) PageGetItem(page, nextid);
		rightlink = ItemPointerGetBlockNumberNoCheck(&next->t_tid);
		ItemPointerSetBlockNumber(&parent->t_tid, rightlink);
		delete_index_items(page, (char *) &nextoff, 1);
	}
	else
		die("invalid B-tree MARK_PAGE_HALFDEAD block reference %d", block_id);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_btree_unlink_page(XLogReaderState *record, Page page,
						 int block_id, bool with_meta)
{
	xl_btree_unlink_page *xlrec;
	OfflineBTPageOpaqueData *opaque;

	if (XLogRecGetDataLen(record) < SizeOfBtreeUnlinkPage)
		die("invalid B-tree UNLINK_PAGE WAL header");
	xlrec = (xl_btree_unlink_page *) XLogRecGetData(record);
	if (block_id == 4 && with_meta)
	{
		apply_btree_meta(record, page, block_id);
		return;
	}
	if (block_id == 3)
	{
		if (xlrec->level == 0)
			die("invalid B-tree UNLINK_PAGE leaf rewrite");
		init_btree_halfdead_page(page, xlrec->leafleftsib,
						 xlrec->leafrightsib, xlrec->leaftopparent);
	}
	else if (block_id == 0)
	{
		init_index_page_offline(page, sizeof(OfflineBTPageOpaqueData));
		opaque = OfflineBTPageGetOpaque(page);
		opaque->btpo_prev = xlrec->leftsib;
		opaque->btpo_next = xlrec->rightsib;
		opaque->btpo_level = xlrec->level;
		opaque->btpo_flags = OFFLINE_BTP_DELETED |
			OFFLINE_BTP_HAS_FULLXID |
			(xlrec->level == 0 ? OFFLINE_BTP_LEAF : 0);
		((PageHeader) page)->pd_lower =
			MAXALIGN(SizeOfPageHeaderData) + sizeof(FullTransactionId);
		memcpy(PageGetContents(page), &xlrec->safexid,
			   sizeof(FullTransactionId));
	}
	else if (block_id == 1)
	{
		opaque = OfflineBTPageGetOpaque(page);
		opaque->btpo_next = xlrec->rightsib;
	}
	else if (block_id == 2)
	{
		opaque = OfflineBTPageGetOpaque(page);
		opaque->btpo_prev = xlrec->leftsib;
	}
	else
		die("invalid B-tree UNLINK_PAGE block reference %d", block_id);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_btree_posting_updates(Page page, const char *data, Size len,
							Size *position, const char *offsets, uint16 nupdated)
{
	uint16 i;

	for (i = 0; i < nupdated; i++)
	{
		OffsetNumber offnum;
		ItemId itemid;
		IndexTuple oldtuple;
		ItemPointer oldtids;
		uint16 oldcount;
		uint16 deleted;
		uint16 newcount;
		Size keysize;
		Size newsize;
		IndexTuple replacement;
		ItemPointer dest;
		uint16 src = 0;
		uint16 dst = 0;
		uint16 j;

		memcpy(&offnum, offsets + i * sizeof(offnum), sizeof(offnum));
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page) ||
			len - *position < SizeOfBtreeUpdate)
			die("invalid B-tree posting update offset or header");
		itemid = PageGetItemId(page, offnum);
		oldtuple = (IndexTuple) PageGetItem(page, itemid);
		if (!offline_btree_is_posting(oldtuple))
			die("B-tree posting update target is not a posting tuple");
		oldcount = offline_btree_posting_count(oldtuple);
		oldtids = offline_btree_posting_tids(oldtuple);
		memcpy(&deleted, data + *position, sizeof(deleted));
		*position += SizeOfBtreeUpdate;
		if (deleted == 0 || deleted >= oldcount ||
			len - *position < (Size) deleted * sizeof(uint16))
			die("invalid B-tree posting deletion count");
		newcount = oldcount - deleted;
		keysize = offline_btree_posting_offset(oldtuple);
		newsize = newcount > 1 ?
			MAXALIGN(keysize + newcount * sizeof(ItemPointerData)) : keysize;
		if (newsize > INDEX_SIZE_MASK)
			die("B-tree posting update result is too large");
		replacement = calloc(1, newsize);
		if (replacement == NULL)
			die("out of memory for B-tree posting update");
		memcpy(replacement, oldtuple, keysize);
		replacement->t_info =
			(replacement->t_info & ~INDEX_SIZE_MASK) | newsize;
		if (newcount > 1)
		{
			ItemPointerSetOffsetNumber(&replacement->t_tid,
								   (OffsetNumber) (newcount | OFFLINE_BT_IS_POSTING));
			ItemPointerSetBlockNumber(&replacement->t_tid, keysize);
			dest = (ItemPointer) ((char *) replacement + keysize);
		}
		else
		{
			replacement->t_info &= ~OFFLINE_INDEX_ALT_TID_MASK;
			dest = &replacement->t_tid;
		}
		for (j = 0; j < oldcount; j++)
		{
			uint16 delete_at;

			if (src < deleted)
			{
				memcpy(&delete_at, data + *position + src * sizeof(uint16),
					   sizeof(delete_at));
				if (delete_at == j)
				{
					src++;
					continue;
				}
				if (delete_at < j || delete_at >= oldcount)
					die("invalid B-tree posting TID offset");
			}
			dest[dst++] = oldtids[j];
		}
		if (dst != newcount || src != deleted)
			die("B-tree posting update TID count mismatch");
		overwrite_index_item(page, offnum, (Item) replacement, newsize);
		free(replacement);
		*position += (Size) deleted * sizeof(uint16);
	}
}

static void
apply_btree_delete_vacuum(XLogReaderState *record, Page page,
						  int block_id, bool vacuum)
{
	uint16 ndeleted;
	uint16 nupdated;
	Size len;
	Size pos;
	char *data;
	OfflineBTPageOpaqueData *opaque;

	if (block_id != 0 ||
		XLogRecGetDataLen(record) <
		(vacuum ? SizeOfBtreeVacuum : SizeOfBtreeDelete))
		die("invalid B-tree %s WAL header", vacuum ? "VACUUM" : "DELETE");
	if (vacuum)
	{
		xl_btree_vacuum *xlrec = (xl_btree_vacuum *) XLogRecGetData(record);
		ndeleted = xlrec->ndeleted;
		nupdated = xlrec->nupdated;
	}
	else
	{
		xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);
		ndeleted = xlrec->ndeleted;
		nupdated = xlrec->nupdated;
	}
	data = XLogRecGetBlockData(record, 0, &len);
	pos = ((Size) ndeleted + nupdated) * sizeof(OffsetNumber);
	if (len < pos || (len != 0 && data == NULL))
		die("invalid B-tree deletion or update offsets");
	if (nupdated != 0)
		apply_btree_posting_updates(page, data, len, &pos,
							  data + ndeleted * sizeof(OffsetNumber),
							  nupdated);
	if (pos != len)
		die("unexpected B-tree deletion payload length");
	if (ndeleted > 0)
		delete_index_items(page, data, ndeleted);
	opaque = OfflineBTPageGetOpaque(page);
	if (vacuum)
		opaque->btpo_cycleid = 0;
	opaque->btpo_flags &= ~OFFLINE_BTP_HAS_GARBAGE;
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_hash_insert(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_insert *xlrec;
	Size len;
	char *data;

	if (XLogRecGetDataLen(record) < SizeOfHashInsert)
		die("invalid Hash INSERT WAL header");
	xlrec = (xl_hash_insert *) XLogRecGetData(record);
	if (block_id == 0)
	{
		data = XLogRecGetBlockData(record, 0, &len);
		if (data == NULL || len < sizeof(IndexTupleData))
			die("Hash INSERT tuple data missing");
		add_btree_item(page, (Item) data, len, xlrec->offnum);
	}
	else if (block_id == 1)
	{
		OfflineHashMetaPrefix *meta =
			(OfflineHashMetaPrefix *) PageGetContents(page);
		meta->hashm_ntuples += 1;
	}
	else
		die("invalid Hash INSERT target block reference %d", block_id);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_hash_split_meta(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_split_allocate_page *xlrec;
	OfflineHashMetaPageData *meta =
		(OfflineHashMetaPageData *) PageGetContents(page);
	char *data;
	Size len;
	Size pos = 0;
	uint32 value;

	if (block_id != 2 ||
		XLogRecGetDataLen(record) < SizeOfHashSplitAllocPage)
		die("invalid Hash SPLIT_ALLOCATE_PAGE metapage record");
	xlrec = (xl_hash_split_allocate_page *) XLogRecGetData(record);
	data = XLogRecGetBlockData(record, 2, &len);
	if (data == NULL && len != 0)
		die("invalid Hash split metapage payload");
	meta->hashm_maxbucket = xlrec->new_bucket;
	if (xlrec->flags & XLH_SPLIT_META_UPDATE_MASKS)
	{
		if (len - pos < 2 * sizeof(uint32))
			die("truncated Hash split masks");
		memcpy(&value, data + pos, sizeof(value));
		meta->hashm_lowmask = value;
		pos += sizeof(value);
		memcpy(&value, data + pos, sizeof(value));
		meta->hashm_highmask = value;
		pos += sizeof(value);
	}
	if (xlrec->flags & XLH_SPLIT_META_UPDATE_SPLITPOINT)
	{
		uint32 ovflpoint;

		if (len - pos < 2 * sizeof(uint32))
			die("truncated Hash split spare-page data");
		memcpy(&ovflpoint, data + pos, sizeof(ovflpoint));
		pos += sizeof(ovflpoint);
		if (ovflpoint >= OFFLINE_HASH_MAX_SPLITPOINTS)
			die("invalid Hash split point %u", ovflpoint);
		memcpy(&value, data + pos, sizeof(value));
		pos += sizeof(value);
		meta->hashm_spares[ovflpoint] = value;
		meta->hashm_ovflpoint = ovflpoint;
	}
	if (pos != len)
		die("unexpected Hash split metapage payload length");
	PageSetLSN(page, record->EndRecPtr);
}

static bool
apply_hash_delete(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_delete *xlrec;
	char *data;
	Size len;
	OfflineHashPageOpaqueData *opaque;

	if (XLogRecGetDataLen(record) < SizeOfHashDelete)
		die("invalid Hash DELETE WAL header");
	xlrec = (xl_hash_delete *) XLogRecGetData(record);
	if (block_id == 0 && !xlrec->is_primary_bucket_page)
		return false; /* This block reference only obtains a cleanup lock. */
	if (block_id != 1)
		die("invalid Hash DELETE target block reference %d", block_id);
	data = XLogRecGetBlockData(record, 1, &len);
	if (len % sizeof(OffsetNumber) != 0 || (len != 0 && data == NULL))
		die("invalid Hash DELETE offset payload");
	if (len != 0)
		delete_index_items(page, data, len / sizeof(OffsetNumber));
	if (xlrec->clear_dead_marking)
	{
		opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
		opaque->hasho_flag &= ~OFFLINE_HASH_HAS_DEAD_TUPLES;
	}
	PageSetLSN(page, record->EndRecPtr);
	return true;
}

static void
apply_hash_split_cleanup(XLogReaderState *record, Page page, int block_id)
{
	OfflineHashPageOpaqueData *opaque;

	if (block_id != 0)
		die("invalid Hash SPLIT_CLEANUP target block reference %d", block_id);
	opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->hasho_flag &= ~OFFLINE_HASH_NEEDS_SPLIT_CLEANUP;
	PageSetLSN(page, record->EndRecPtr);
}

static uint32
offline_hash_spareindex(uint32 buckets)
{
	uint32 group = 0;
	uint32 value = buckets - 1;

	while (value != 0)
	{
		value >>= 1;
		group++;
	}
	if (group < 10)
		return group;
	return 10 + ((group - 10) << 2) +
		(((buckets - 1) >> (group - 3)) & 3);
}

static uint32
offline_hash_totalbuckets(uint32 phase)
{
	uint32 group;
	uint32 within;

	if (phase < 10)
		return 1U << phase;
	group = 10 + ((phase - 10) >> 2);
	within = ((phase - 10) & 3) + 1;
	return (1U << (group - 1)) +
		(((1U << (group - 1)) >> 2) * within);
}

static void
init_hash_bitmap_page(Page page, uint16 bmsize)
{
	OfflineHashPageOpaqueData *opaque;

	if (bmsize > BLCKSZ - MAXALIGN(SizeOfPageHeaderData) -
		MAXALIGN(sizeof(OfflineHashPageOpaqueData)))
		die("invalid Hash bitmap size %u", bmsize);
	init_index_page_offline(page, sizeof(OfflineHashPageOpaqueData));
	opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->hasho_prevblkno = InvalidBlockNumber;
	opaque->hasho_nextblkno = InvalidBlockNumber;
	opaque->hasho_bucket = InvalidBlockNumber;
	opaque->hasho_flag = 0x04;
	opaque->hasho_page_id = 0xFF80;
	memset(PageGetContents(page), 0xFF, bmsize);
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + bmsize;
}

static void
apply_hash_init_meta(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_init_meta_page header;
	OfflineHashMetaPageData *meta;
	OfflineHashPageOpaqueData *opaque;
	double estimate;
	uint32 buckets;
	uint32 phase;
	uint32 high;
	uint32 bmsize;

	if (block_id != 0 || XLogRecGetDataLen(record) < SizeOfHashInitMetaPage)
		die("invalid Hash INIT_META_PAGE WAL header");
	memcpy(&header, XLogRecGetData(record), SizeOfHashInitMetaPage);
	if (header.ffactor == 0)
		die("invalid Hash fill factor");
	estimate = header.num_tuples / header.ffactor;
	if (estimate <= 2.0)
		buckets = 2;
	else if (estimate >= (double) 0x40000000)
		buckets = 0x40000000;
	else
		buckets = offline_hash_totalbuckets(
			offline_hash_spareindex((uint32) estimate));
	phase = offline_hash_spareindex(buckets);
	if (phase >= OFFLINE_HASH_MAX_SPLITPOINTS)
		die("invalid Hash initial split phase");
	init_index_page_offline(page, sizeof(OfflineHashPageOpaqueData));
	opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->hasho_prevblkno = InvalidBlockNumber;
	opaque->hasho_nextblkno = InvalidBlockNumber;
	opaque->hasho_bucket = InvalidBlockNumber;
	opaque->hasho_flag = 0x08;
	opaque->hasho_page_id = 0xFF80;
	meta = (OfflineHashMetaPageData *) PageGetContents(page);
	meta->hashm_magic = 0x6440640;
	meta->hashm_version = 4;
	meta->hashm_ffactor = header.ffactor;
	meta->hashm_bsize = BLCKSZ - MAXALIGN(SizeOfPageHeaderData) -
		MAXALIGN(sizeof(OfflineHashPageOpaqueData));
	bmsize = 1;
	while ((bmsize << 1) <= meta->hashm_bsize)
		bmsize <<= 1;
	meta->hashm_bmsize = bmsize;
	meta->hashm_bmshift = 3;
	while ((1U << (meta->hashm_bmshift - 3)) < bmsize)
		meta->hashm_bmshift++;
	meta->hashm_maxbucket = buckets - 1;
	high = 1;
	while (high < buckets + 1)
		high <<= 1;
	meta->hashm_highmask = high - 1;
	meta->hashm_lowmask = meta->hashm_highmask >> 1;
	meta->hashm_ovflpoint = phase;
	meta->hashm_spares[phase] = 1;
	meta->hashm_procid = header.procid;
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) +
		sizeof(OfflineHashMetaPageData);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_hash_init_bitmap(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_init_bitmap_page header;

	if (XLogRecGetDataLen(record) < SizeOfHashInitBitmapPage)
		die("invalid Hash INIT_BITMAP_PAGE WAL header");
	memcpy(&header, XLogRecGetData(record), SizeOfHashInitBitmapPage);
	if (block_id == 0)
		init_hash_bitmap_page(page, header.bmsize);
	else if (block_id == 1)
	{
		OfflineHashMetaPageData *meta =
			(OfflineHashMetaPageData *) PageGetContents(page);

		if (meta->hashm_nmaps >= 1024)
			die("too many Hash bitmap pages");
		meta->hashm_mapp[meta->hashm_nmaps++] = meta->hashm_maxbucket + 2;
	}
	else
		die("invalid Hash INIT_BITMAP_PAGE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_hash_add_overflow(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_add_ovfl_page header;
	BlockNumber rightblk;
	BlockNumber leftblk;
	Size len;
	char *data;
	uint32 value;

	if (XLogRecGetDataLen(record) < SizeOfHashAddOvflPage ||
		!get_block_tag(record, 0, NULL, NULL, &rightblk) ||
		!get_block_tag(record, 1, NULL, NULL, &leftblk))
		die("invalid Hash ADD_OVFL_PAGE WAL header");
	memcpy(&header, XLogRecGetData(record), SizeOfHashAddOvflPage);
	if (block_id == 0)
	{
		OfflineHashPageOpaqueData *opaque;

		data = XLogRecGetBlockData(record, 0, &len);
		if (data == NULL || len != sizeof(uint32))
			die("invalid Hash overflow bucket payload");
		memcpy(&value, data, sizeof(value));
		init_index_page_offline(page, sizeof(OfflineHashPageOpaqueData));
		opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
		opaque->hasho_prevblkno = leftblk;
		opaque->hasho_nextblkno = InvalidBlockNumber;
		opaque->hasho_bucket = value;
		opaque->hasho_flag = 0x01;
		opaque->hasho_page_id = 0xFF80;
	}
	else if (block_id == 1)
	{
		OfflineHashPageOpaqueData *opaque =
			(OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);

		opaque->hasho_nextblkno = rightblk;
	}
	else if (block_id == 2)
	{
		uint32 *bits = (uint32 *) PageGetContents(page);

		data = XLogRecGetBlockData(record, 2, &len);
		if (data == NULL || len != sizeof(uint32))
			die("invalid Hash bitmap bit payload");
		memcpy(&value, data, sizeof(value));
		if (value >= (uint32) (((PageHeader) page)->pd_lower -
			MAXALIGN(SizeOfPageHeaderData)) * 8)
			die("Hash bitmap bit out of range");
		bits[value / 32] |= 1U << (value % 32);
	}
	else if (block_id == 3)
		init_hash_bitmap_page(page, header.bmsize);
	else if (block_id == 4)
	{
		OfflineHashMetaPageData *meta =
			(OfflineHashMetaPageData *) PageGetContents(page);

		data = XLogRecGetBlockData(record, 4, &len);
		if (data == NULL || len != sizeof(uint32))
			die("invalid Hash overflow metapage payload");
		memcpy(&meta->hashm_firstfree, data, sizeof(uint32));
		if (!header.bmpage_found)
		{
			if (meta->hashm_ovflpoint >= OFFLINE_HASH_MAX_SPLITPOINTS)
				die("invalid Hash overflow split point");
			meta->hashm_spares[meta->hashm_ovflpoint]++;
			if (XLogRecHasBlockRef(record, 3))
			{
				BlockNumber bitmap_blk;

				if (meta->hashm_nmaps >= 1024 ||
					!get_block_tag(record, 3, NULL, NULL, &bitmap_blk))
					die("invalid Hash overflow bitmap reference");
				meta->hashm_mapp[meta->hashm_nmaps++] = bitmap_blk;
				meta->hashm_spares[meta->hashm_ovflpoint]++;
			}
		}
	}
	else
		die("invalid Hash ADD_OVFL_PAGE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_hash_moved_tuples(XLogReaderState *record, Page page, int block_id,
						uint16 ntups)
{
	char *data;
	Size len;
	Size pos = (Size) ntups * sizeof(OffsetNumber);
	int i;

	data = XLogRecGetBlockData(record, block_id, &len);
	if (data == NULL || len < pos)
		die("invalid Hash moved-tuple WAL payload");
	for (i = 0; i < ntups; i++)
	{
		OffsetNumber offnum;
		IndexTupleData tuple;
		Size size;

		if (len - pos < sizeof(tuple))
			die("truncated Hash moved tuple");
		memcpy(&offnum, data + i * sizeof(offnum), sizeof(offnum));
		memcpy(&tuple, data + pos, sizeof(tuple));
		size = MAXALIGN(IndexTupleSize(&tuple));
		if (size < sizeof(tuple) || size > len - pos)
			die("invalid Hash moved tuple length");
		add_btree_item(page, (Item) (data + pos), size, offnum);
		pos += size;
	}
	if (pos != len)
		die("Hash moved-tuple payload length mismatch");
}

static bool
apply_hash_move_contents(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_move_page_contents header;
	char *data;
	Size len;

	if (XLogRecGetDataLen(record) < SizeOfHashMovePageContents)
		die("invalid Hash MOVE_PAGE_CONTENTS WAL header");
	memcpy(&header, XLogRecGetData(record), SizeOfHashMovePageContents);
	if (block_id == 0)
		return false; /* Cleanup lock only. */
	if (block_id == 1)
		apply_hash_moved_tuples(record, page, 1, header.ntups);
	else if (block_id == 2)
	{
		data = XLogRecGetBlockData(record, 2, &len);
		if (len % sizeof(OffsetNumber) != 0 || (len != 0 && data == NULL))
			die("invalid Hash MOVE_PAGE_CONTENTS deletion offsets");
		if (len != 0)
			delete_index_items(page, data, len / sizeof(OffsetNumber));
	}
	else
		die("invalid Hash MOVE_PAGE_CONTENTS block reference");
	PageSetLSN(page, record->EndRecPtr);
	return true;
}

static bool
apply_hash_squeeze_page(XLogReaderState *record, Page page, int block_id)
{
	xl_hash_squeeze_page header;
	OfflineHashPageOpaqueData *opaque;
	char *data;
	Size len;
	uint32 bit;

	if (XLogRecGetDataLen(record) < SizeOfHashSqueezePage)
		die("invalid Hash SQUEEZE_PAGE WAL header");
	memcpy(&header, XLogRecGetData(record), SizeOfHashSqueezePage);
	if (block_id == 0)
		return false; /* Cleanup lock only. */
	if (block_id == 1)
	{
		if (header.ntups != 0)
			apply_hash_moved_tuples(record, page, 1, header.ntups);
		if (header.is_prev_bucket_same_wrt)
		{
			opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
			opaque->hasho_nextblkno = header.nextblkno;
		}
		if (header.ntups == 0 && !header.is_prev_bucket_same_wrt)
			return false;
	}
	else if (block_id == 2)
	{
		init_index_page_offline(page, sizeof(OfflineHashPageOpaqueData));
		opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
		opaque->hasho_prevblkno = InvalidBlockNumber;
		opaque->hasho_nextblkno = InvalidBlockNumber;
		opaque->hasho_bucket = InvalidBlockNumber;
		opaque->hasho_flag = 0;
		opaque->hasho_page_id = 0xFF80;
	}
	else if (block_id == 3)
	{
		opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
		opaque->hasho_nextblkno = header.nextblkno;
	}
	else if (block_id == 4)
	{
		opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
		opaque->hasho_prevblkno = header.prevblkno;
	}
	else if (block_id == 5)
	{
		uint32 *bits = (uint32 *) PageGetContents(page);

		data = XLogRecGetBlockData(record, 5, &len);
		if (data == NULL || len != sizeof(bit))
			die("invalid Hash SQUEEZE_PAGE bitmap payload");
		memcpy(&bit, data, sizeof(bit));
		if (bit >= (uint32) (((PageHeader) page)->pd_lower -
			MAXALIGN(SizeOfPageHeaderData)) * 8)
			die("Hash SQUEEZE_PAGE bitmap bit out of range");
		bits[bit / 32] &= ~(1U << (bit % 32));
	}
	else if (block_id == 6)
	{
		OfflineHashMetaPageData *meta =
			(OfflineHashMetaPageData *) PageGetContents(page);

		data = XLogRecGetBlockData(record, 6, &len);
		if (data == NULL || len != sizeof(uint32))
			die("invalid Hash SQUEEZE_PAGE metapage payload");
		memcpy(&meta->hashm_firstfree, data, sizeof(uint32));
	}
	else
		die("invalid Hash SQUEEZE_PAGE block reference");
	PageSetLSN(page, record->EndRecPtr);
	return true;
}

static void
apply_hash_update_meta(XLogReaderState *record, Page page, int block_id)
{
	OfflineHashMetaPageData *meta = (OfflineHashMetaPageData *) PageGetContents(page);

	if (block_id != 0 || XLogRecGetDataLen(record) < sizeof(double))
		die("invalid Hash UPDATE_META_PAGE WAL header");
	memcpy(&meta->hashm_ntuples, XLogRecGetData(record), sizeof(double));
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_hash_vacuum_one_page(XLogReaderState *record, Page page, int block_id)
{
	const char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	const char *offsets;
	Size offsets_len;
	int ntuples;

	if (len < 8)
		die("invalid Hash VACUUM_ONE_PAGE WAL header");
#if PG_VERSION_NUM >= 160000
	{
		uint16 count;
		memcpy(&count, data + sizeof(TransactionId), sizeof(count));
		ntuples = count;
		offsets = data + 8;
		offsets_len = len - 8;
	}
#else
	memcpy(&ntuples, data + sizeof(TransactionId), sizeof(ntuples));
	offsets = data + 8;
	offsets_len = len - 8;
#endif
	if (ntuples < 0 || ntuples > MaxIndexTuplesPerPage ||
		(offsets == NULL && ntuples != 0) ||
		offsets_len < (Size) ntuples * sizeof(OffsetNumber))
		die("invalid Hash VACUUM_ONE_PAGE offsets");
	if (block_id == 0)
	{
		OfflineHashPageOpaqueData *opaque =
			(OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);

		if (ntuples != 0)
			delete_index_items(page, offsets, ntuples);
		opaque->hasho_flag &= ~OFFLINE_HASH_HAS_DEAD_TUPLES;
	}
	else if (block_id == 1)
	{
		OfflineHashMetaPageData *meta =
			(OfflineHashMetaPageData *) PageGetContents(page);

		meta->hashm_ntuples -= ntuples;
	}
	else
		die("invalid Hash VACUUM_ONE_PAGE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

/* Some redo routines change special space even after restoring an FPI. */
static bool
apply_after_block_image(XLogReaderState *record, Page page, int block_id)
{
	RmgrId rmid = XLogRecGetRmid(record);
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char lsnbuf[64];

	format_lsn(lsnbuf, sizeof(lsnbuf), record->ReadRecPtr);

	if (rmid == RM_HASH_ID && info == XLOG_HASH_SPLIT_ALLOCATE_PAGE &&
		block_id == 0)
	{
		xl_hash_split_allocate_page *xlrec;
		OfflineHashPageOpaqueData *opaque =
			(OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);

		if (XLogRecGetDataLen(record) < SizeOfHashSplitAllocPage)
			die("invalid Hash SPLIT_ALLOCATE_PAGE WAL header");
		xlrec = (xl_hash_split_allocate_page *) XLogRecGetData(record);
		opaque->hasho_flag = xlrec->old_bucket_flag;
		opaque->hasho_prevblkno = xlrec->new_bucket;
		PageSetLSN(page, record->EndRecPtr);
		printf("applied Hash SPLIT_ALLOCATE_PAGE lsn=%s block_ref=%d\n",
			   lsnbuf, block_id);
		return true;
	}
	if (rmid == RM_HASH_ID && info == XLOG_HASH_SPLIT_COMPLETE &&
		(block_id == 0 || block_id == 1))
	{
		xl_hash_split_complete *xlrec;
		OfflineHashPageOpaqueData *opaque =
			(OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);

		if (XLogRecGetDataLen(record) < SizeOfHashSplitComplete)
			die("invalid Hash SPLIT_COMPLETE WAL header");
		xlrec = (xl_hash_split_complete *) XLogRecGetData(record);
		opaque->hasho_flag = block_id == 0 ? xlrec->old_bucket_flag :
			xlrec->new_bucket_flag;
		PageSetLSN(page, record->EndRecPtr);
		printf("applied Hash SPLIT_COMPLETE lsn=%s block_ref=%d\n",
			   lsnbuf, block_id);
		return true;
	}
	if (rmid == RM_GIST_ID &&
		((info == OFFLINE_XLOG_GIST_PAGE_UPDATE && block_id == 1) ||
		 (info == OFFLINE_XLOG_GIST_PAGE_SPLIT && block_id == 0)))
	{
		OfflineGistPageOpaqueData *opaque =
			(OfflineGistPageOpaqueData *) PageGetSpecialPointer(page);

		PageXLogRecPtrSet(opaque->nsn, record->EndRecPtr);
		opaque->flags &= ~OFFLINE_GIST_FOLLOW_RIGHT;
		PageSetLSN(page, record->EndRecPtr);
		printf("applied GiST CLEAR_FOLLOW_RIGHT lsn=%s block_ref=%d\n",
			   lsnbuf, block_id);
		return true;
	}
	return false;
}

static void
apply_gist_page_update(XLogReaderState *record, Page page, int block_id)
{
	OfflineGistPageUpdate header;
	Size len;
	char *data;
	Size pos = 0;
	uint16 i;
	Size offsets_len;

	if (block_id != 0 || XLogRecGetDataLen(record) < sizeof(header))
		die("invalid GiST PAGE_UPDATE target block or header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	data = XLogRecGetBlockData(record, 0, &len);
	offsets_len = (Size) header.ntodelete * sizeof(OffsetNumber);
	if (len < offsets_len || (len != 0 && data == NULL))
		die("invalid GiST PAGE_UPDATE block data");
	if (header.ntodelete == 1 && header.ntoinsert == 1)
	{
		OffsetNumber off;
		IndexTupleData itup;
		Size size;

		memcpy(&off, data, sizeof(off));
		if (len - offsets_len < sizeof(itup))
			die("truncated GiST replacement tuple");
		memcpy(&itup, data + offsets_len, sizeof(itup));
		size = IndexTupleSize(&itup);
		if (size != len - offsets_len)
			die("invalid GiST replacement tuple length");
		overwrite_index_item(page, off,
					 (Item) (data + offsets_len), size);
		PageSetLSN(page, record->EndRecPtr);
		return;
	}
	if (header.ntodelete != 0)
		delete_index_items(page, data, header.ntodelete);
	pos = offsets_len;
	for (i = 0; i < header.ntoinsert; i++)
	{
		IndexTupleData itup;
		Size size;

		if (len - pos < sizeof(itup))
			die("truncated GiST PAGE_UPDATE tuple stream");
		memcpy(&itup, data + pos, sizeof(itup));
		size = IndexTupleSize(&itup);
		if (size < sizeof(itup) || size > len - pos)
			die("invalid GiST PAGE_UPDATE tuple length");
		add_btree_item(page, (Item) (data + pos), size,
					   OffsetNumberNext(PageGetMaxOffsetNumber(page)));
		pos += size;
	}
	if (pos != len)
		die("GiST PAGE_UPDATE tuple stream length mismatch");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_gist_delete(XLogReaderState *record, Page page, int block_id)
{
	const char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	uint16 ndelete;
	const char *offsets;
	Size offsets_len;
	Size offsets_start;
	OfflineGistPageOpaqueData *opaque;

	if (block_id != 0 || len < sizeof(TransactionId) + sizeof(uint16))
		die("invalid GiST DELETE WAL header");
	memcpy(&ndelete, data + sizeof(TransactionId), sizeof(ndelete));
#if PG_VERSION_NUM >= 160000
	offsets_start = 8;
#else
	offsets_start = 6;
#endif
	if (len < offsets_start)
		die("truncated GiST DELETE header");
	offsets = data + offsets_start;
	offsets_len = len - offsets_start;
	if (offsets == NULL || offsets_len < (Size) ndelete * sizeof(OffsetNumber))
		die("truncated GiST DELETE offsets");
	delete_index_items(page, offsets, ndelete);
	opaque = (OfflineGistPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->flags &= ~OFFLINE_GIST_HAS_GARBAGE;
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_gist_page_split(XLogReaderState *record, Page page, int block_id,
					  BlockNumber target_blkno)
{
	OfflineGistPageSplit header;
	OfflineGistPageOpaqueData *opaque;
	char *data;
	Size len;
	Size pos;
	int ntuples;
	int i;
	int index = block_id - 1;
	BlockNumber nextblkno;
	bool root_split;

	if (block_id < 1 || XLogRecGetDataLen(record) < sizeof(header))
		die("invalid GiST PAGE_SPLIT WAL header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	if (index >= header.npage || !XLogRecHasBlockRef(record, block_id))
		die("invalid GiST PAGE_SPLIT block reference");
	data = XLogRecGetBlockData(record, block_id, &len);
	if (data == NULL || len < sizeof(ntuples))
		die("truncated GiST PAGE_SPLIT data");
	memcpy(&ntuples, data, sizeof(ntuples));
	if (ntuples < 0 || ntuples > MaxIndexTuplesPerPage)
		die("invalid GiST PAGE_SPLIT tuple count");
	init_index_page_offline(page, sizeof(OfflineGistPageOpaqueData));
	opaque = (OfflineGistPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->gist_page_id = OFFLINE_GIST_PAGE_ID;
	if (header.origleaf && target_blkno != 0)
		opaque->flags |= OFFLINE_GIST_LEAF;
	pos = sizeof(ntuples);
	for (i = 0; i < ntuples; i++)
	{
		IndexTupleData tuple;
		Size size;

		if (len - pos < sizeof(tuple))
			die("truncated GiST PAGE_SPLIT tuple");
		memcpy(&tuple, data + pos, sizeof(tuple));
		size = IndexTupleSize(&tuple);
		if (size < sizeof(tuple) || size > len - pos)
			die("invalid GiST PAGE_SPLIT tuple size");
		add_btree_item(page, (Item) (data + pos), size,
					   OffsetNumberNext(PageGetMaxOffsetNumber(page)));
		pos += size;
	}
	if (pos != len)
		die("GiST PAGE_SPLIT tuple stream length mismatch");
	root_split = false;
	if (XLogRecHasBlockRef(record, 1))
	{
		BlockNumber firstblkno;
		get_block_tag(record, 1, NULL, NULL, &firstblkno);
		root_split = firstblkno == 0;
	}
	if (target_blkno == 0)
		opaque->rightlink = InvalidBlockNumber;
	else if (index + 1 < header.npage)
	{
		if (!get_block_tag(record, block_id + 1, NULL, NULL, &nextblkno))
			die("missing GiST PAGE_SPLIT next page");
		opaque->rightlink = nextblkno;
		if (!root_split && header.markfollowright)
			opaque->flags |= OFFLINE_GIST_FOLLOW_RIGHT;
	}
	else
		opaque->rightlink = header.origrlink;
	PageXLogRecPtrSet(opaque->nsn, header.orignsn);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_gist_page_delete(XLogReaderState *record, Page page, int block_id)
{
	OfflineGistPageDelete header;

	if (XLogRecGetDataLen(record) < offsetof(OfflineGistPageDelete, downlinkOffset) +
		sizeof(OffsetNumber))
		die("invalid GiST PAGE_DELETE WAL header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	if (block_id == 0)
	{
		OfflineGistPageOpaqueData *opaque =
			(OfflineGistPageOpaqueData *) PageGetSpecialPointer(page);

		if (!PageIsEmpty(page))
			die("GiST PAGE_DELETE target leaf is not empty");
		opaque->flags |= OFFLINE_GIST_DELETED;
		((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) +
			sizeof(FullTransactionId);
		memcpy(PageGetContents(page), &header.deleteXid,
			   sizeof(header.deleteXid));
	}
	else if (block_id == 1)
		delete_index_items(page, (const char *) &header.downlinkOffset, 1);
	else
		die("invalid GiST PAGE_DELETE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

/* Gin posting list varbyte encoding orders TIDs as block<<11 | offset. */
static int
compare_uint64_values(const void *left, const void *right)
{
	uint64 a = *(const uint64 *) left;
	uint64 b = *(const uint64 *) right;

	return (a > b) - (a < b);
}

static void
gin_recompress_page(Page page, const char *wal, Size wal_len)
{
	ginxlogRecompressDataLeaf header;
	char old[BLCKSZ];
	char result[BLCKSZ];
	Size old_len;
	Size old_pos = 0;
	Size result_len = 0;
	Size wal_pos = sizeof(header);
	uint16 segno = 0;
	int i;

	if (wal == NULL || wal_len < sizeof(header))
		die("invalid GIN recompress payload");
	memcpy(&header, wal, sizeof(header));
	if (GinPageIsCompressed(page))
	{
		old_len = GinDataLeafPageGetPostingListSize(page);
		if (old_len > GinDataPageMaxDataSize)
			die("invalid GIN compressed page size");
		memcpy(old, GinDataLeafPageGetPostingList(page), old_len);
	}
	else
	{
		GinPageOpaqueData *opaque =
			(GinPageOpaqueData *) PageGetSpecialPointer(page);
		uint64 values[BLCKSZ / sizeof(ItemPointerData)];
		int count = opaque->maxoff;
		Size outpos = offsetof(GinPostingList, bytes);
		uint64 previous;

		if (count < 0 || count > lengthof(values))
			die("invalid uncompressed GIN posting count");
		old_len = 0;
		if (count > 0)
		{
			ItemPointer items = (ItemPointer) GinDataPageGetData(page);
			GinPostingList *segment = (GinPostingList *) old;

			for (i = 0; i < count; i++)
				values[i] = ((uint64) ItemPointerGetBlockNumber(&items[i]) << 11) |
					ItemPointerGetOffsetNumber(&items[i]);
			segment->first = items[0];
			previous = values[0];
			for (i = 1; i < count; i++)
			{
				uint64 delta = values[i] - previous;

				if (values[i] <= previous)
					die("unsorted uncompressed GIN posting list");
				while (delta > 0x7F)
				{
					if (outpos >= sizeof(old))
						die("GIN posting list exceeds page");
					old[outpos++] = 0x80 | (delta & 0x7F);
					delta >>= 7;
				}
				if (outpos >= sizeof(old))
					die("GIN posting list exceeds page");
				old[outpos++] = delta;
				previous = values[i];
			}
			segment->nbytes = outpos - offsetof(GinPostingList, bytes);
			old_len = SHORTALIGN(outpos);
			if (old_len > outpos)
				old[outpos] = 0;
		}
		opaque->flags |= GIN_COMPRESSED;
		opaque->maxoff = InvalidOffsetNumber;
	}

	for (i = 0; i < header.nactions; i++)
	{
		uint8 target;
		uint8 action;
		Size seg_size = 0;
		Size new_size = 0;
		const char *new_segment = NULL;
		char encoded[BLCKSZ];

		if (wal_len - wal_pos < 2)
			die("truncated GIN recompress action");
		target = (uint8) wal[wal_pos++];
		action = (uint8) wal[wal_pos++];
		if (target < segno)
			die("unordered GIN recompress action");
		while (segno < target)
		{
			GinPostingList old_header;

			if (old_len - old_pos < offsetof(GinPostingList, bytes))
				die("GIN recompress segment number out of range");
			memcpy(&old_header, old + old_pos, offsetof(GinPostingList, bytes));
			seg_size = SizeOfGinPostingList(&old_header);
			if (seg_size > old_len - old_pos || seg_size > sizeof(result) - result_len)
				die("invalid GIN recompress source segment");
			memcpy(result + result_len, old + old_pos, seg_size);
			result_len += seg_size;
			old_pos += seg_size;
			segno++;
		}
		if (old_pos < old_len)
		{
			GinPostingList old_header;

			if (old_len - old_pos < offsetof(GinPostingList, bytes))
				die("truncated GIN recompress source");
			memcpy(&old_header, old + old_pos, offsetof(GinPostingList, bytes));
			seg_size = SizeOfGinPostingList(&old_header);
			if (seg_size > old_len - old_pos)
				die("invalid GIN recompress source length");
		}
		if (action == GIN_SEGMENT_INSERT || action == GIN_SEGMENT_REPLACE)
		{
			GinPostingList new_header;

			if (wal_len - wal_pos < offsetof(GinPostingList, bytes))
				die("truncated GIN recompress replacement");
			memcpy(&new_header, wal + wal_pos, offsetof(GinPostingList, bytes));
			new_size = SizeOfGinPostingList(&new_header);
			if (new_size > wal_len - wal_pos)
				die("invalid GIN recompress replacement length");
			new_segment = wal + wal_pos;
			wal_pos += new_size;
		}
		else if (action == GIN_SEGMENT_ADDITEMS)
		{
			uint64 values[BLCKSZ / sizeof(ItemPointerData) + 1];
			GinPostingList *segment;
			uint64 previous;
			Size item_pos;
			Size encoded_pos;
			uint16 nitems;
			int count = 0;
			int j;

			if (seg_size < offsetof(GinPostingList, bytes) ||
				wal_len - wal_pos < sizeof(nitems))
				die("invalid GIN ADDITEMS source");
			segment = (GinPostingList *) (old + old_pos);
			memcpy(&nitems, wal + wal_pos, sizeof(nitems));
			wal_pos += sizeof(nitems);
			if ((Size) nitems > (wal_len - wal_pos) / sizeof(ItemPointerData))
				die("truncated GIN ADDITEMS items");
			values[count++] = ((uint64) ItemPointerGetBlockNumber(&segment->first) << 11) |
				ItemPointerGetOffsetNumber(&segment->first);
			previous = values[0];
			item_pos = offsetof(GinPostingList, bytes);
			while (item_pos < offsetof(GinPostingList, bytes) + segment->nbytes)
			{
				uint64 delta = 0;
				int shift = 0;
				uint8 byte;

				do
				{
					if (item_pos >= seg_size || shift > 42)
						die("invalid GIN posting varbyte");
					byte = (uint8) old[old_pos + item_pos++];
					delta |= ((uint64) (byte & 0x7F)) << shift;
					shift += 7;
				} while (byte & 0x80);
				if (count >= lengthof(values))
					die("too many GIN posting items");
				previous += delta;
				values[count++] = previous;
			}
			for (j = 0; j < nitems; j++)
			{
				ItemPointerData item;

				if (count >= lengthof(values))
					die("too many GIN ADDITEMS items");
				memcpy(&item, wal + wal_pos, sizeof(item));
				wal_pos += sizeof(item);
				values[count++] = ((uint64) ItemPointerGetBlockNumber(&item) << 11) |
					ItemPointerGetOffsetNumber(&item);
			}
			qsort(values, count, sizeof(values[0]), compare_uint64_values);
			segment = (GinPostingList *) encoded;
			ItemPointerSet(&segment->first, values[0] >> 11,
						   values[0] & 0x7FF);
			encoded_pos = offsetof(GinPostingList, bytes);
			previous = values[0];
			for (j = 1; j < count; j++)
			{
				uint64 delta;

				if (values[j] == previous)
					continue;
				delta = values[j] - previous;
				while (delta > 0x7F)
				{
					if (encoded_pos >= sizeof(encoded))
						die("GIN ADDITEMS encoded segment too large");
					encoded[encoded_pos++] = 0x80 | (delta & 0x7F);
					delta >>= 7;
				}
				if (encoded_pos >= sizeof(encoded))
					die("GIN ADDITEMS encoded segment too large");
				encoded[encoded_pos++] = delta;
				previous = values[j];
			}
			segment->nbytes = encoded_pos - offsetof(GinPostingList, bytes);
			new_size = SHORTALIGN(encoded_pos);
			if (new_size > encoded_pos)
				encoded[encoded_pos] = 0;
			new_segment = encoded;
			action = GIN_SEGMENT_REPLACE;
		}
		if (action == GIN_SEGMENT_DELETE || action == GIN_SEGMENT_REPLACE)
		{
			if (seg_size == 0)
				die("GIN recompress source segment missing");
			old_pos += seg_size;
			segno++;
		}
		else if (action != GIN_SEGMENT_INSERT)
			die("unsupported GIN recompress action %u", action);
		if (new_segment != NULL)
		{
			if (new_size > sizeof(result) - result_len)
				die("GIN recompress result exceeds page");
			memcpy(result + result_len, new_segment, new_size);
			result_len += new_size;
		}
	}
	if (wal_pos != wal_len || old_len - old_pos > sizeof(result) - result_len)
		die("GIN recompress payload or page size mismatch");
	memcpy(result + result_len, old + old_pos, old_len - old_pos);
	result_len += old_len - old_pos;
	if (result_len > GinDataPageMaxDataSize)
		die("GIN recompress result exceeds data area");
	memcpy(GinDataLeafPageGetPostingList(page), result, result_len);
	GinDataPageSetDataSize(page, result_len);
}

static void
apply_gin_insert(XLogReaderState *record, Page page, int block_id)
{
	OfflineGinInsert header;
	OfflineGinInsertEntry entry;
	Size len;
	char *data;
	Size prefix = offsetof(OfflineGinInsertEntry, tuple);
	Size tuple_size;
	BlockNumber right_child = InvalidBlockNumber;

	if (block_id != 0 || XLogRecGetDataLen(record) < sizeof(header))
		die("invalid GIN INSERT target block or header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	if (!(header.flags & GIN_INSERT_ISLEAF))
	{
		char *main = XLogRecGetData(record);
		BlockIdData id;

		if (XLogRecGetDataLen(record) < sizeof(header) + 2 * sizeof(id))
			die("invalid GIN internal INSERT child payload");
		memcpy(&id, main + sizeof(header) + sizeof(id), sizeof(id));
		right_child = BlockIdGetBlockNumber(&id);
	}
	data = XLogRecGetBlockData(record, 0, &len);
	if (data == NULL)
		die("GIN INSERT block payload missing");
	if (header.flags & GIN_INSERT_ISDATA)
	{
		if (header.flags & GIN_INSERT_ISLEAF)
			gin_recompress_page(page, data, len);
		else
		{
			ginxlogInsertDataInternal internal;
			GinPageOpaqueData *opaque =
				(GinPageOpaqueData *) PageGetSpecialPointer(page);
			PostingItem *item;
			Size pos;

			if (len != sizeof(internal))
				die("invalid GIN internal data INSERT payload");
			memcpy(&internal, data, sizeof(internal));
			if (internal.offset < FirstOffsetNumber ||
				internal.offset > opaque->maxoff)
				die("invalid GIN internal data offset");
			item = GinDataPageGetPostingItem(page, internal.offset);
			PostingItemSetBlockNumber(item, right_child);
			pos = (char *) item - page;
			if (opaque->maxoff >= GinDataPageMaxDataSize / sizeof(PostingItem))
				die("GIN internal data page is full");
			memmove(page + pos + sizeof(PostingItem), page + pos,
					(opaque->maxoff - internal.offset + 1) * sizeof(PostingItem));
			memcpy(page + pos, &internal.newitem, sizeof(PostingItem));
			opaque->maxoff++;
			GinDataPageSetDataSize(page, opaque->maxoff * sizeof(PostingItem));
		}
		PageSetLSN(page, record->EndRecPtr);
		return;
	}
	if (len < sizeof(entry))
		die("GIN INSERT entry data missing");
	memcpy(&entry, data, sizeof(entry));
	tuple_size = IndexTupleSize(&entry.tuple);
	if (tuple_size < sizeof(IndexTupleData) ||
		tuple_size != len - prefix)
		die("GIN INSERT entry tuple length mismatch");
	if (!(header.flags & GIN_INSERT_ISLEAF))
	{
		IndexTuple old;

		if (entry.offset < FirstOffsetNumber ||
			entry.offset > PageGetMaxOffsetNumber(page))
			die("invalid GIN internal entry offset");
		old = (IndexTuple) PageGetItem(page, PageGetItemId(page, entry.offset));
		GinSetDownlink(old, right_child);
	}
	if (entry.isDelete)
		delete_index_items(page, (const char *) &entry.offset, 1);
	add_btree_item(page, (Item) (data + prefix), tuple_size, entry.offset);
	PageSetLSN(page, record->EndRecPtr);
}

static void
init_gin_page(Page page, uint16 flags)
{
	GinPageOpaqueData *opaque;

	init_index_page_offline(page, sizeof(GinPageOpaqueData));
	opaque = (GinPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->flags = flags;
	opaque->rightlink = InvalidBlockNumber;
}

static void
init_gin_meta(Page page, const GinMetaPageData *metadata)
{
	init_gin_page(page, GIN_META);
	memcpy(PageGetContents(page), metadata, sizeof(*metadata));
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) +
		sizeof(*metadata);
}

static void
apply_gin_tuple_stream(Page page, const char *data, Size len, int ntuples)
{
	Size pos = 0;
	int i;

	if (ntuples < 0 || ntuples > MaxIndexTuplesPerPage)
		die("invalid GIN tuple count");
	for (i = 0; i < ntuples; i++)
	{
		IndexTupleData tuple;
		Size size;

		if (len - pos < sizeof(tuple))
			die("truncated GIN tuple stream");
		memcpy(&tuple, data + pos, sizeof(tuple));
		size = IndexTupleSize(&tuple);
		if (size < sizeof(tuple) || size > len - pos)
			die("invalid GIN tuple size");
		add_btree_item(page, (Item) (data + pos), size,
					   OffsetNumberNext(PageGetMaxOffsetNumber(page)));
		pos += size;
	}
	if (pos != len)
		die("GIN tuple stream length mismatch");
}

static void
apply_gin_create_ptree(XLogReaderState *record, Page page, int block_id)
{
	ginxlogCreatePostingTree header;
	Size len = XLogRecGetDataLen(record);

	if (block_id != 0 || len < sizeof(header))
		die("invalid GIN CREATE_PTREE WAL header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	if (header.size > len - sizeof(header) || header.size > GinDataPageMaxDataSize)
		die("invalid GIN posting tree size");
	init_gin_page(page, GIN_DATA | GIN_LEAF | GIN_COMPRESSED);
	memcpy(GinDataLeafPageGetPostingList(page),
		   XLogRecGetData(record) + sizeof(header), header.size);
	GinDataPageSetDataSize(page, header.size);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_gin_insert_listpage(XLogReaderState *record, Page page, int block_id)
{
	ginxlogInsertListPage header;
	Size len;
	char *data;
	GinPageOpaqueData *opaque;

	if (block_id != 0 || XLogRecGetDataLen(record) < sizeof(header))
		die("invalid GIN INSERT_LISTPAGE WAL header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	init_gin_page(page, GIN_LIST);
	opaque = (GinPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->rightlink = header.rightlink;
	if (header.rightlink == InvalidBlockNumber)
	{
		opaque->flags |= GIN_LIST_FULLROW;
		opaque->maxoff = 1;
	}
	data = XLogRecGetBlockData(record, 0, &len);
	if (data == NULL && len != 0)
		die("missing GIN INSERT_LISTPAGE tuples");
	apply_gin_tuple_stream(page, data, len, header.ntuples);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_gin_update_meta(XLogReaderState *record, Page page, int block_id)
{
	ginxlogUpdateMeta header;
	Size minlen = offsetof(ginxlogUpdateMeta, ntuples) + sizeof(int32);
	Size len;
	char *data;

	if (XLogRecGetDataLen(record) < minlen)
		die("invalid GIN UPDATE_META_PAGE WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, XLogRecGetData(record), minlen);
	if (block_id == 0)
		init_gin_meta(page, &header.metadata);
	else if (block_id == 1)
	{
		GinPageOpaqueData *opaque =
			(GinPageOpaqueData *) PageGetSpecialPointer(page);

		if (header.ntuples > 0)
		{
			data = XLogRecGetBlockData(record, 1, &len);
			if (data == NULL && len != 0)
				die("missing GIN UPDATE_META_PAGE tuples");
			apply_gin_tuple_stream(page, data, len, header.ntuples);
			opaque->maxoff++;
		}
		else if (header.prevTail != InvalidBlockNumber)
			opaque->rightlink = header.newRightlink;
		else
			die("invalid GIN UPDATE_META_PAGE tail reference");
	}
	else
		die("invalid GIN UPDATE_META_PAGE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_gin_delete_listpage(XLogReaderState *record, Page page, int block_id)
{
	ginxlogDeleteListPages header;
	Size minlen = offsetof(ginxlogDeleteListPages, ndeleted) + sizeof(int32);

	if (XLogRecGetDataLen(record) < minlen)
		die("invalid GIN DELETE_LISTPAGE WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, XLogRecGetData(record), minlen);
	if (header.ndeleted < 0 || header.ndeleted > XLR_MAX_BLOCK_ID - 1)
		die("invalid GIN deleted-list-page count");
	if (block_id == 0)
		init_gin_meta(page, &header.metadata);
	else if (block_id <= header.ndeleted)
		init_gin_page(page, GIN_DELETED);
	else
		die("invalid GIN DELETE_LISTPAGE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_gin_delete_page(XLogReaderState *record, Page page, int block_id)
{
	ginxlogDeletePage header;
	GinPageOpaqueData *opaque =
		(GinPageOpaqueData *) PageGetSpecialPointer(page);

	if (XLogRecGetDataLen(record) < sizeof(header))
		die("invalid GIN DELETE_PAGE WAL header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	if (block_id == 0)
	{
		opaque->flags |= GIN_DELETED;
		((PageHeader) page)->pd_prune_xid = header.deleteXid;
	}
	else if (block_id == 1)
	{
		/* Internal data pages store a compact PostingItem array. */
		Size start = MAXALIGN(SizeOfPageHeaderData) +
			MAXALIGN(sizeof(ItemPointerData));
		Size size = sizeof(PostingItem);
		Size pos = start + (header.parentOffset - 1) * size;

		if (header.parentOffset < 1 || header.parentOffset > opaque->maxoff)
			die("invalid GIN DELETE_PAGE parent offset");
		memmove(page + pos, page + pos + size,
				(opaque->maxoff - header.parentOffset) * size);
		opaque->maxoff--;
		((PageHeader) page)->pd_lower = start + opaque->maxoff * size;
	}
	else if (block_id == 2)
		opaque->rightlink = header.rightLink;
	else
		die("invalid GIN DELETE_PAGE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
spgist_init_page(Page page, uint16 flags)
{
	OfflineSpgPageOpaqueData *opaque;

	init_index_page_offline(page, sizeof(*opaque));
	opaque = (OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->flags = flags;
	opaque->page_id = 0xFF82;
}

static int
compare_offset_numbers(const void *left, const void *right)
{
	OffsetNumber a = *(const OffsetNumber *) left;
	OffsetNumber b = *(const OffsetNumber *) right;

	return (a > b) - (a < b);
}

static void
spgist_replace_with_dead(Page page, const char *offsets, int nitems,
					 int first_state, int rest_state,
					 BlockNumber block, OffsetNumber offset,
					 spgxlogState state)
{
	OffsetNumber sorted[BLCKSZ / sizeof(ItemIdData)];
	OfflineSpgPageOpaqueData *opaque =
		(OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);
	OffsetNumber first;
	int i;

	if (nitems == 0)
		return;
	if (nitems < 0 || nitems >= lengthof(sorted))
		die("invalid SP-GiST replacement count");
	memcpy(sorted, offsets, nitems * sizeof(OffsetNumber));
	first = sorted[0];
	qsort(sorted, nitems, sizeof(sorted[0]), compare_offset_numbers);
	delete_index_items(page, (const char *) sorted, nitems);
	for (i = 0; i < nitems; i++)
	{
		OfflineSpgDeadTupleData tuple;
		int new_state = sorted[i] == first ? first_state : rest_state;

		memset(&tuple, 0, sizeof(tuple));
		tuple.tupstate = new_state;
		tuple.size = MAXALIGN(sizeof(tuple));
		if (new_state == 1)
		{
			ItemPointerSet(&tuple.pointer, block, offset);
			tuple.xid = OFFLINE_SPG_REDIRECT_XID(state);
			opaque->nRedirection++;
		}
		else
		{
			ItemPointerSetInvalid(&tuple.pointer);
			if (new_state == 3)
				opaque->nPlaceholder++;
		}
		add_btree_item(page, (Item) &tuple, tuple.size, sorted[i]);
	}
}

static void
apply_spgist_vacuum_root(XLogReaderState *record, Page page, int block_id)
{
	spgxlogVacuumRoot header;
	Size len = XLogRecGetDataLen(record);
	Size prefix = SizeOfSpgxlogVacuumRoot;

	if (block_id != 0 || len < prefix)
		die("invalid SP-GiST VACUUM_ROOT WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, XLogRecGetData(record), prefix);
	if (len - prefix < (Size) header.nDelete * sizeof(OffsetNumber))
		die("truncated SP-GiST VACUUM_ROOT offsets");
	if (header.nDelete != 0)
		delete_index_items(page, XLogRecGetData(record) + prefix,
					   header.nDelete);
	PageSetLSN(page, record->EndRecPtr);
}

static void
spgist_add_or_replace(Page page, const char *tuple, Size size,
					  OffsetNumber offnum)
{
	if (offnum <= PageGetMaxOffsetNumber(page))
	{
		ItemId id = PageGetItemId(page, offnum);
		uint32 state;
		OfflineSpgPageOpaqueData *opaque =
			(OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);

		if (!ItemIdIsNormal(id) || ItemIdGetLength(id) < sizeof(state))
			die("invalid SP-GiST placeholder");
		memcpy(&state, PageGetItem(page, id), sizeof(state));
		if ((state & 3) != 3 || opaque->nPlaceholder == 0)
			die("SP-GiST tuple to replace is not a placeholder");
		opaque->nPlaceholder--;
		delete_index_items(page, (const char *) &offnum, 1);
	}
	add_btree_item(page, (Item) tuple, size, offnum);
}

static void
spgist_update_parent_link(Page page, OffsetNumber offnum, uint16 node_index,
					 BlockNumber block, OffsetNumber leaf_offset)
{
	ItemId id;
	OfflineSpgInnerTupleData inner;
	char *tuple;
	Size pos;
	int i;

	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		die("invalid SP-GiST parent offset");
	id = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(id) || ItemIdGetLength(id) < sizeof(inner))
		die("invalid SP-GiST parent tuple");
	tuple = PageGetItem(page, id);
	memcpy(&inner, tuple, sizeof(inner));
	if (inner.size > ItemIdGetLength(id) || node_index >= inner.nNodes)
		die("invalid SP-GiST parent node");
	pos = MAXALIGN(sizeof(inner)) + inner.prefixSize;
	for (i = 0; i <= node_index; i++)
	{
		IndexTupleData node;
		Size size;

		if (pos + sizeof(node) > inner.size)
			die("truncated SP-GiST parent node");
		memcpy(&node, tuple + pos, sizeof(node));
		size = IndexTupleSize(&node);
		if (size < sizeof(node) || pos + size > inner.size)
			die("invalid SP-GiST parent node length");
		if (i == node_index)
		{
			ItemPointerSet(&((IndexTuple) (tuple + pos))->t_tid,
						   block, leaf_offset);
			return;
		}
		pos += size;
	}
}

static void
apply_spgist_add_leaf(XLogReaderState *record, Page page, int block_id)
{
	OfflineSpgAddLeaf header;
	OfflineSpgLeafTupleData tuple;
	char *data;
	Size mainlen = XLogRecGetDataLen(record);
	Size pos = sizeof(header);

	if (mainlen < pos + sizeof(tuple))
		die("invalid SP-GiST ADD_LEAF target block or WAL payload");
	data = XLogRecGetData(record);
	memcpy(&header, data, sizeof(header));
	memcpy(&tuple, data + pos, sizeof(tuple));
	if (tuple.size < sizeof(tuple) || tuple.size != mainlen - pos)
		die("invalid SP-GiST ADD_LEAF tuple length");
	if (block_id == 0)
	{
		if (header.newPage)
		{
			OfflineSpgPageOpaqueData *opaque;

			init_index_page_offline(page, sizeof(*opaque));
			opaque = (OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);
			opaque->flags = 0x04 | (header.storesNulls ? 0x08 : 0);
			opaque->page_id = 0xFF82;
		}
		if (header.offnumLeaf != header.offnumHeadLeaf)
		{
			spgist_add_or_replace(page, data + pos, tuple.size,
						  header.offnumLeaf);
			if (header.offnumHeadLeaf != InvalidOffsetNumber)
			{
				ItemId id;
				OfflineSpgLeafTupleData *head;

				if (header.offnumHeadLeaf > PageGetMaxOffsetNumber(page))
					die("invalid SP-GiST leaf chain offset");
				id = PageGetItemId(page, header.offnumHeadLeaf);
				if (!ItemIdIsNormal(id) || ItemIdGetLength(id) < sizeof(*head))
					die("invalid SP-GiST leaf chain head");
				head = (OfflineSpgLeafTupleData *) PageGetItem(page, id);
				head->t_info = (head->t_info & 0xC000) |
					(header.offnumLeaf & 0x3FFF);
			}
		}
		else
		{
			delete_index_items(page, (const char *) &header.offnumLeaf, 1);
			add_btree_item(page, (Item) (data + pos), tuple.size,
						   header.offnumLeaf);
		}
	}
	else if (block_id == 1 && header.offnumParent != InvalidOffsetNumber)
	{
		BlockNumber leaf_block;

		if (!get_block_tag(record, 0, NULL, NULL, &leaf_block))
			die("SP-GiST ADD_LEAF missing leaf block reference");
		spgist_update_parent_link(page, header.offnumParent, header.nodeI,
						  leaf_block, header.offnumLeaf);
	}
	else
		die("invalid SP-GiST ADD_LEAF block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_spgist_move_leafs(XLogReaderState *record, Page page, int block_id)
{
	spgxlogMoveLeafs header;
	char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	Size pos = SizeOfSpgxlogMoveLeafs;
	Size tuple_pos;
	int ninsert;
	int i;
	BlockNumber dest_block;

	if (len < pos || !get_block_tag(record, 1, NULL, NULL, &dest_block))
		die("invalid SP-GiST MOVE_LEAFS WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, data, pos);
	ninsert = header.replaceDead ? 1 : header.nMoves + 1;
	if (header.nMoves >= BLCKSZ / sizeof(OffsetNumber) ||
		ninsert >= BLCKSZ / sizeof(OffsetNumber) ||
		len - pos < (Size) (header.nMoves + ninsert) * sizeof(OffsetNumber))
		die("invalid SP-GiST MOVE_LEAFS offsets");
	tuple_pos = pos + (Size) (header.nMoves + ninsert) * sizeof(OffsetNumber);
	if (block_id == 1)
	{
		if (header.newPage)
			spgist_init_page(page, 0x04 | (header.storesNulls ? 0x08 : 0));
		for (i = 0; i < ninsert; i++)
		{
			OfflineSpgLeafTupleData tuple;
			OffsetNumber offnum;

			if (len - tuple_pos < sizeof(tuple))
				die("truncated SP-GiST MOVE_LEAFS tuple");
			memcpy(&tuple, data + tuple_pos, sizeof(tuple));
			if (tuple.size < sizeof(tuple) || tuple.size > len - tuple_pos)
				die("invalid SP-GiST MOVE_LEAFS tuple length");
			memcpy(&offnum, data + pos +
				   (header.nMoves + i) * sizeof(offnum), sizeof(offnum));
			spgist_add_or_replace(page, data + tuple_pos, tuple.size, offnum);
			tuple_pos += tuple.size;
		}
	}
	else if (block_id == 0)
	{
		OffsetNumber dest_offset;

		memcpy(&dest_offset, data + pos +
			   (header.nMoves + ninsert - 1) * sizeof(dest_offset),
			   sizeof(dest_offset));
		spgist_replace_with_dead(page, data + pos, header.nMoves,
						 header.stateSrc.isBuild ? 3 : 1, 3,
						 dest_block, dest_offset, header.stateSrc);
	}
	else if (block_id == 2)
	{
		OffsetNumber dest_offset;

		memcpy(&dest_offset, data + pos +
			   (header.nMoves + ninsert - 1) * sizeof(dest_offset),
			   sizeof(dest_offset));
		spgist_update_parent_link(page, header.offnumParent, header.nodeI,
						  dest_block, dest_offset);
	}
	else
		die("invalid SP-GiST MOVE_LEAFS block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_spgist_picksplit(XLogReaderState *record, Page page, int block_id)
{
	spgxlogPickSplit header;
	OfflineSpgInnerTupleData inner;
	char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	Size pos = SizeOfSpgxlogPickSplit;
	Size array_size;
	Size inner_pos;
	Size leaf_pos;
	BlockNumber inner_block;
	int i;

	if (len < pos || !get_block_tag(record, 2, NULL, NULL, &inner_block))
		die("invalid SP-GiST PICKSPLIT WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, data, pos);
	array_size = (Size) header.nDelete * sizeof(OffsetNumber) +
		(Size) header.nInsert * (sizeof(OffsetNumber) + 1);
	if (header.nDelete >= BLCKSZ / sizeof(OffsetNumber) ||
		header.nInsert >= BLCKSZ / sizeof(OffsetNumber) ||
		array_size > len - pos)
		die("invalid SP-GiST PICKSPLIT offsets");
	inner_pos = pos + array_size;
	if (len - inner_pos < sizeof(inner))
		die("missing SP-GiST PICKSPLIT inner tuple");
	memcpy(&inner, data + inner_pos, sizeof(inner));
	if (inner.size < sizeof(inner) || inner.size > len - inner_pos)
		die("invalid SP-GiST PICKSPLIT inner tuple");
	leaf_pos = inner_pos + inner.size;
	if (block_id == 0)
	{
		if (header.isRootSplit)
			die("invalid SP-GiST root split source reference");
		if (header.initSrc)
			spgist_init_page(page, 0x04 | (header.storesNulls ? 0x08 : 0));
		else
			spgist_replace_with_dead(page, data + pos, header.nDelete,
						 header.stateSrc.isBuild ? 3 : 1, 3,
						 inner_block, header.offnumInner,
						 header.stateSrc);
	}
	else if (block_id == 1)
	{
		if (header.initDest)
			spgist_init_page(page, 0x04 | (header.storesNulls ? 0x08 : 0));
	}
	else if (block_id == 2)
	{
		if (header.initInner)
			spgist_init_page(page, header.storesNulls ? 0x08 : 0);
		spgist_add_or_replace(page, data + inner_pos, inner.size,
						  header.offnumInner);
		if (header.innerIsParent)
			spgist_update_parent_link(page, header.offnumParent,
							  header.nodeI, inner_block,
							  header.offnumInner);
	}
	else if (block_id == 3)
		spgist_update_parent_link(page, header.offnumParent, header.nodeI,
						  inner_block, header.offnumInner);
	else
		die("invalid SP-GiST PICKSPLIT block reference");
	if (block_id == 0 || block_id == 1)
	{
		Size offsets_pos = pos + (Size) header.nDelete * sizeof(OffsetNumber);
		Size select_pos = offsets_pos + (Size) header.nInsert * sizeof(OffsetNumber);

		for (i = 0; i < header.nInsert; i++)
		{
			OfflineSpgLeafTupleData tuple;
			OffsetNumber offnum;

			if (len - leaf_pos < sizeof(tuple))
				die("truncated SP-GiST PICKSPLIT leaf tuple");
			memcpy(&tuple, data + leaf_pos, sizeof(tuple));
			if (tuple.size < sizeof(tuple) || tuple.size > len - leaf_pos)
				die("invalid SP-GiST PICKSPLIT leaf length");
			if ((data[select_pos + i] != 0) == (block_id == 1))
			{
				memcpy(&offnum, data + offsets_pos + i * sizeof(offnum),
					   sizeof(offnum));
				spgist_add_or_replace(page, data + leaf_pos, tuple.size,
							  offnum);
			}
			leaf_pos += tuple.size;
		}
	}
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_spgist_add_node(XLogReaderState *record, Page page, int block_id)
{
	spgxlogAddNode header;
	OfflineSpgInnerTupleData tuple;
	char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	Size pos = sizeof(header);
	BlockNumber new_block;
	bool moved;

	if (len < pos + sizeof(tuple))
		die("invalid SP-GiST ADD_NODE WAL header");
	memcpy(&header, data, sizeof(header));
	memcpy(&tuple, data + pos, sizeof(tuple));
	if (tuple.size < sizeof(tuple) || tuple.size != len - pos)
		die("invalid SP-GiST ADD_NODE tuple");
	moved = XLogRecHasBlockRef(record, 1);
	if (!moved)
	{
		if (block_id != 0 || header.parentBlk != -1)
			die("invalid SP-GiST ADD_NODE in-place block reference");
		delete_index_items(page, (const char *) &header.offnum, 1);
		add_btree_item(page, (Item) (data + pos), tuple.size, header.offnum);
	}
	else
	{
		OfflineSpgDeadTupleData dead;
		OfflineSpgPageOpaqueData *opaque =
			(OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);

		if (!get_block_tag(record, 1, NULL, NULL, &new_block))
			die("SP-GiST ADD_NODE missing destination block");
		if (block_id == 1)
		{
			if (header.newPage)
				spgist_init_page(page, 0);
			spgist_add_or_replace(page, data + pos, tuple.size,
							  header.offnumNew);
		}
		else if (block_id == 0)
		{
			memset(&dead, 0, sizeof(dead));
			dead.tupstate = header.stateSrc.isBuild ? 3 : 1;
			dead.size = MAXALIGN(sizeof(dead));
			if (!header.stateSrc.isBuild)
			{
				ItemPointerSet(&dead.pointer, new_block, header.offnumNew);
				dead.xid = OFFLINE_SPG_REDIRECT_XID(header.stateSrc);
				opaque->nRedirection++;
			}
			else
			{
				ItemPointerSetInvalid(&dead.pointer);
				opaque->nPlaceholder++;
			}
			delete_index_items(page, (const char *) &header.offnum, 1);
			add_btree_item(page, (Item) &dead, dead.size, header.offnum);
		}
		else if (block_id != 2)
			die("invalid SP-GiST ADD_NODE block reference");
		if (header.parentBlk == block_id)
			spgist_update_parent_link(page, header.offnumParent,
							  header.nodeI, new_block,
							  header.offnumNew);
	}
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_spgist_vacuum_leaf(XLogReaderState *record, Page page, int block_id)
{
	spgxlogVacuumLeaf header;
	char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	Size pos = SizeOfSpgxlogVacuumLeaf;
	Size count;
	Size dead_pos;
	Size placeholder_pos;
	Size move_src_pos;
	Size move_dest_pos;
	Size chain_src_pos;
	Size chain_dest_pos;
	int i;

	if (block_id != 0 || len < pos)
		die("invalid SP-GiST VACUUM_LEAF WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, data, pos);
	count = (Size) header.nDead + header.nPlaceholder +
		2 * (Size) header.nMove + 2 * (Size) header.nChain;
	if (count > (len - pos) / sizeof(OffsetNumber))
		die("invalid SP-GiST VACUUM_LEAF offset arrays");
	dead_pos = pos;
	placeholder_pos = dead_pos + header.nDead * sizeof(OffsetNumber);
	move_src_pos = placeholder_pos + header.nPlaceholder * sizeof(OffsetNumber);
	move_dest_pos = move_src_pos + header.nMove * sizeof(OffsetNumber);
	chain_src_pos = move_dest_pos + header.nMove * sizeof(OffsetNumber);
	chain_dest_pos = chain_src_pos + header.nChain * sizeof(OffsetNumber);
	spgist_replace_with_dead(page, data + dead_pos, header.nDead,
						  2, 2, InvalidBlockNumber,
						  InvalidOffsetNumber, header.stateSrc);
	spgist_replace_with_dead(page, data + placeholder_pos,
						  header.nPlaceholder, 3, 3,
						  InvalidBlockNumber, InvalidOffsetNumber,
						  header.stateSrc);
	for (i = 0; i < header.nMove; i++)
	{
		OffsetNumber src;
		OffsetNumber dest;
		ItemIdData tmp;
		ItemId src_id;
		ItemId dest_id;

		memcpy(&src, data + move_src_pos + i * sizeof(src), sizeof(src));
		memcpy(&dest, data + move_dest_pos + i * sizeof(dest), sizeof(dest));
		if (src < FirstOffsetNumber || src > PageGetMaxOffsetNumber(page) ||
			dest < FirstOffsetNumber || dest > PageGetMaxOffsetNumber(page))
			die("invalid SP-GiST VACUUM_LEAF move offsets");
		src_id = PageGetItemId(page, src);
		dest_id = PageGetItemId(page, dest);
		tmp = *src_id;
		*src_id = *dest_id;
		*dest_id = tmp;
	}
	spgist_replace_with_dead(page, data + move_src_pos, header.nMove,
						  3, 3, InvalidBlockNumber,
						  InvalidOffsetNumber, header.stateSrc);
	for (i = 0; i < header.nChain; i++)
	{
		OffsetNumber src;
		OffsetNumber dest;
		ItemId id;
		OfflineSpgLeafTupleData *tuple;

		memcpy(&src, data + chain_src_pos + i * sizeof(src), sizeof(src));
		memcpy(&dest, data + chain_dest_pos + i * sizeof(dest), sizeof(dest));
		if (src < FirstOffsetNumber || src > PageGetMaxOffsetNumber(page))
			die("invalid SP-GiST VACUUM_LEAF chain offset");
		id = PageGetItemId(page, src);
		if (!ItemIdIsNormal(id) || ItemIdGetLength(id) < sizeof(*tuple))
			die("invalid SP-GiST VACUUM_LEAF chain tuple");
		tuple = (OfflineSpgLeafTupleData *) PageGetItem(page, id);
		tuple->t_info = (tuple->t_info & 0xC000) | (dest & 0x3FFF);
	}
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_spgist_split_tuple(XLogReaderState *record, Page page, int block_id)
{
	spgxlogSplitTuple header;
	OfflineSpgInnerTupleData prefix;
	OfflineSpgInnerTupleData postfix;
	char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	Size pos = sizeof(header);
	Size postfix_pos;

	if (len < pos + sizeof(prefix))
		die("invalid SP-GiST SPLIT_TUPLE WAL header");
	memcpy(&header, data, sizeof(header));
	memcpy(&prefix, data + pos, sizeof(prefix));
	if (prefix.size < sizeof(prefix) || prefix.size > len - pos)
		die("invalid SP-GiST SPLIT_TUPLE prefix");
	postfix_pos = pos + prefix.size;
	if (len - postfix_pos < sizeof(postfix))
		die("missing SP-GiST SPLIT_TUPLE postfix");
	memcpy(&postfix, data + postfix_pos, sizeof(postfix));
	if (postfix.size < sizeof(postfix) || postfix.size != len - postfix_pos)
		die("invalid SP-GiST SPLIT_TUPLE postfix");
	if (block_id == 1 && !header.postfixBlkSame)
	{
		if (header.newPage)
		{
			OfflineSpgPageOpaqueData *opaque;

			init_index_page_offline(page, sizeof(*opaque));
			opaque = (OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);
			opaque->page_id = 0xFF82;
		}
		spgist_add_or_replace(page, data + postfix_pos, postfix.size,
						  header.offnumPostfix);
	}
	else if (block_id == 0)
	{
		if (header.offnumPrefix < FirstOffsetNumber ||
			header.offnumPrefix > PageGetMaxOffsetNumber(page))
			die("invalid SP-GiST SPLIT_TUPLE prefix offset");
		delete_index_items(page, (const char *) &header.offnumPrefix, 1);
		add_btree_item(page, (Item) (data + pos), prefix.size,
					   header.offnumPrefix);
		if (header.postfixBlkSame)
			spgist_add_or_replace(page, data + postfix_pos, postfix.size,
							  header.offnumPostfix);
	}
	else
		die("invalid SP-GiST SPLIT_TUPLE block reference");
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_spgist_vacuum_redirect(XLogReaderState *record, Page page, int block_id)
{
	spgxlogVacuumRedirect header;
	OfflineSpgPageOpaqueData *opaque =
		(OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);
	char *data = XLogRecGetData(record);
	Size len = XLogRecGetDataLen(record);
	Size prefix = SizeOfSpgxlogVacuumRedirect;
	OffsetNumber maxoff;
	int i;

	if (block_id != 0 || len < prefix)
		die("invalid SP-GiST VACUUM_REDIRECT WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, data, prefix);
	if (header.nToPlaceholder > (len - prefix) / sizeof(OffsetNumber) ||
		opaque->nRedirection < header.nToPlaceholder)
		die("invalid SP-GiST VACUUM_REDIRECT offsets");
	for (i = 0; i < header.nToPlaceholder; i++)
	{
		OffsetNumber offnum;
		ItemId id;
		char *tuple;
		uint32 state;
		ItemPointerData invalid;

		memcpy(&offnum, data + prefix + i * sizeof(offnum), sizeof(offnum));
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
			die("invalid SP-GiST redirect offset");
		id = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(id) || ItemIdGetLength(id) < sizeof(OfflineSpgLeafTupleData))
			die("invalid SP-GiST redirect tuple");
		tuple = PageGetItem(page, id);
		memcpy(&state, tuple, sizeof(state));
		if ((state & 3) != 1)
			die("SP-GiST tuple is not a redirect");
		state = (state & ~3U) | 3;
		memcpy(tuple, &state, sizeof(state));
		ItemPointerSetInvalid(&invalid);
		memcpy(tuple + offsetof(OfflineSpgLeafTupleData, heapPtr),
			   &invalid, sizeof(invalid));
	}
	opaque->nRedirection -= header.nToPlaceholder;
	opaque->nPlaceholder += header.nToPlaceholder;
	maxoff = PageGetMaxOffsetNumber(page);
	if (header.firstPlaceholder != InvalidOffsetNumber)
	{
		OffsetNumber offsets[BLCKSZ / sizeof(ItemIdData)];
		int ndelete = 0;

		if (header.firstPlaceholder < FirstOffsetNumber ||
			header.firstPlaceholder > maxoff)
			die("invalid SP-GiST first placeholder");
		for (i = header.firstPlaceholder; i <= maxoff; i++)
			offsets[ndelete++] = i;
		if (opaque->nPlaceholder < ndelete)
			die("invalid SP-GiST placeholder count");
		opaque->nPlaceholder -= ndelete;
		delete_index_items(page, (const char *) offsets, ndelete);
	}
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_brin_samepage_update(XLogReaderState *record, Page page, int block_id)
{
	OfflineBrinSamepageUpdate header;
	Size len;
	char *data;

	if (block_id != 0 || XLogRecGetDataLen(record) < sizeof(header))
		die("invalid BRIN SAMEPAGE_UPDATE target block or header");
	memcpy(&header, XLogRecGetData(record), sizeof(header));
	data = XLogRecGetBlockData(record, 0, &len);
	if (data == NULL || len == 0)
		die("BRIN SAMEPAGE_UPDATE tuple data missing");
	overwrite_index_item(page, header.offnum, (Item) data, len);
	PageSetLSN(page, record->EndRecPtr);
}

static void
init_brin_page(Page page, uint16 type)
{
	init_index_page_offline(page, MAXALIGN(1));
	memcpy(page + BLCKSZ - sizeof(type), &type, sizeof(type));
}

static ItemPointerData *
brin_revmap_item(Page page, BlockNumber pagesPerRange,
				 BlockNumber heapBlk)
{
	Size maxitems = (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) -
					 MAXALIGN(1)) / sizeof(ItemPointerData);
	Size index;

	if (pagesPerRange == 0)
		die("invalid BRIN pagesPerRange in WAL");
	index = (heapBlk / pagesPerRange) % maxitems;
	return ((ItemPointerData *) PageGetContents(page)) + index;
}

static void
apply_brin_insert_data(XLogReaderState *record, Page page, int block_id,
				   const OfflineBrinInsert *header)
{
	Size len;
	char *data;

	if (block_id == 0)
	{
		if (XLogRecGetInfo(record) & OFFLINE_XLOG_BRIN_INIT_PAGE)
			init_brin_page(page, OFFLINE_BRIN_PAGETYPE_REGULAR);
		data = XLogRecGetBlockData(record, 0, &len);
		if (data == NULL || len == 0)
			die("BRIN INSERT tuple data missing");
		add_btree_item(page, (Item) data, len, header->offnum);
	}
	else if (block_id == 1)
	{
		ItemPointerData *item;
		BlockNumber regblk;

		regblk = btree_block_number(record, 0);
		item = brin_revmap_item(page, header->pagesPerRange,
						 header->heapBlk);
		ItemPointerSet(item, regblk, header->offnum);
	}
	else
		die("invalid BRIN INSERT target block reference %d", block_id);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_brin_insert(XLogReaderState *record, Page page, int block_id)
{
	OfflineBrinInsert header;

	if (XLogRecGetDataLen(record) <
		offsetof(OfflineBrinInsert, offnum) + sizeof(OffsetNumber))
		die("invalid BRIN INSERT WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, XLogRecGetData(record),
		   offsetof(OfflineBrinInsert, offnum) + sizeof(OffsetNumber));
	apply_brin_insert_data(record, page, block_id, &header);
}

static void
apply_brin_update(XLogReaderState *record, Page page, int block_id)
{
	OfflineBrinUpdate header;
	Size size = offsetof(OfflineBrinUpdate, insert) +
		offsetof(OfflineBrinInsert, offnum) + sizeof(OffsetNumber);

	if (XLogRecGetDataLen(record) < size)
		die("invalid BRIN UPDATE WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, XLogRecGetData(record), size);
	if (block_id == 2)
	{
		delete_index_item_no_compact(page, header.oldOffnum);
		PageSetLSN(page, record->EndRecPtr);
	}
	else
		apply_brin_insert_data(record, page, block_id, &header.insert);
}

static void
apply_brin_create_index(XLogReaderState *record, Page page, int block_id)
{
	OfflineBrinCreateIndex header;
	OfflineBrinMetaPageData *meta;
	Size size = offsetof(OfflineBrinCreateIndex, version) + sizeof(uint16);

	if (block_id != 0 || XLogRecGetDataLen(record) < size)
		die("invalid BRIN CREATE_INDEX WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, XLogRecGetData(record), size);
	init_brin_page(page, OFFLINE_BRIN_PAGETYPE_META);
	meta = (OfflineBrinMetaPageData *) PageGetContents(page);
	meta->brinMagic = OFFLINE_BRIN_META_MAGIC;
	meta->brinVersion = header.version;
	meta->pagesPerRange = header.pagesPerRange;
	meta->lastRevmapPage = 0;
	((PageHeader) page)->pd_lower =
		(char *) (meta + 1) - (char *) page;
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_brin_revmap_extend(XLogReaderState *record, Page page, int block_id)
{
	BlockNumber target;

	if (XLogRecGetDataLen(record) < sizeof(target))
		die("invalid BRIN REVMAP_EXTEND WAL header");
	memcpy(&target, XLogRecGetData(record), sizeof(target));
	if (block_id == 0)
	{
		OfflineBrinMetaPageData *meta =
			(OfflineBrinMetaPageData *) PageGetContents(page);

		meta->lastRevmapPage = target;
		((PageHeader) page)->pd_lower =
			(char *) (meta + 1) - (char *) page;
	}
	else if (block_id == 1)
		init_brin_page(page, OFFLINE_BRIN_PAGETYPE_REVMAP);
	else
		die("invalid BRIN REVMAP_EXTEND block reference %d", block_id);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_brin_desummarize(XLogReaderState *record, Page page, int block_id)
{
	OfflineBrinDesummarize header;
	Size size = offsetof(OfflineBrinDesummarize, regOffset) +
		sizeof(OffsetNumber);

	if (XLogRecGetDataLen(record) < size)
		die("invalid BRIN DESUMMARIZE WAL header");
	memset(&header, 0, sizeof(header));
	memcpy(&header, XLogRecGetData(record), size);
	if (block_id == 0)
		ItemPointerSetInvalid(brin_revmap_item(page,
									 header.pagesPerRange, header.heapBlk));
	else if (block_id == 1)
		delete_index_item_no_compact(page, header.regOffset);
	else
		die("invalid BRIN DESUMMARIZE block reference %d", block_id);
	PageSetLSN(page, record->EndRecPtr);
}

/* Generic WAL stores independent byte-range replacements for each block. */
static void
apply_generic_delta(XLogReaderState *record, Page page, int block_id)
{
	Size len;
	char *data = XLogRecGetBlockData(record, block_id, &len);
	Size pos = 0;
	PageHeader phdr;

	if (data == NULL && len != 0)
		die("Generic WAL block data is missing");
	while (pos < len)
	{
		OffsetNumber offset;
		OffsetNumber length;

		if (len - pos < 2 * sizeof(OffsetNumber))
			die("truncated Generic WAL fragment header");
		memcpy(&offset, data + pos, sizeof(offset));
		pos += sizeof(offset);
		memcpy(&length, data + pos, sizeof(length));
		pos += sizeof(length);
		if (length > len - pos || offset > BLCKSZ ||
			length > BLCKSZ - offset)
			die("invalid Generic WAL fragment bounds");
		memcpy(page + offset, data + pos, length);
		pos += length;
	}
	phdr = (PageHeader) page;
	check_page_pointers(page);
	memset(page + phdr->pd_lower, 0, phdr->pd_upper - phdr->pd_lower);
	PageSetLSN(page, record->EndRecPtr);
}

/* Sequence WAL contains a complete replacement tuple for block zero. */
static void
apply_sequence_log(XLogReaderState *record, Page page, int block_id)
{
	Size mainlen = XLogRecGetDataLen(record);
	char *data = XLogRecGetData(record);
	uint32 *magic;

	if (block_id != 0 || mainlen <= sizeof(RelFileLocator))
		die("invalid Sequence WAL target block or tuple");
	init_index_page_offline(page, sizeof(uint32));
	magic = (uint32 *) PageGetSpecialPointer(page);
	*magic = OFFLINE_SEQ_MAGIC;
	add_btree_item(page, (Item) (data + sizeof(RelFileLocator)),
				   mainlen - sizeof(RelFileLocator), FirstOffsetNumber);
	PageSetLSN(page, record->EndRecPtr);
}

static void
truncate_line_pointer_array(Page page)
{
	PageHeader	phdr = (PageHeader) page;
	OffsetNumber maxoff;
	OffsetNumber offnum;
	LocationIndex new_lower;

	maxoff = PageGetMaxOffsetNumber(page);

	while (maxoff >= FirstOffsetNumber)
	{
		ItemId itemid = PageGetItemId(page, maxoff);

		if (ItemIdIsUsed(itemid))
			break;

		if (maxoff == FirstOffsetNumber)
		{
			maxoff = InvalidOffsetNumber;
			break;
		}

		maxoff = OffsetNumberPrev(maxoff);
	}

	if (maxoff == InvalidOffsetNumber)
		new_lower = SizeOfPageHeaderData;
	else
		new_lower = SizeOfPageHeaderData + maxoff * sizeof(ItemIdData);

	if (new_lower < phdr->pd_lower)
	{
		OffsetNumber oldmax = PageGetMaxOffsetNumber(page);

		for (offnum = OffsetNumberNext(maxoff);
			 offnum <= oldmax;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId itemid = PageGetItemId(page, offnum);
			ItemIdSetUnused(itemid);
		}

		phdr->pd_lower = new_lower;
	}
}

static void
repair_page_fragmentation(Page page)
{
	PageHeader	phdr = (PageHeader) page;
	PGAlignedBlock tmpbuf;
	Page		tmp = (Page) tmpbuf.data;
	OffsetNumber maxoff;
	OffsetNumber offnum;
	LocationIndex upper;

	check_page_pointers(page);

	memcpy(tmp, page, BLCKSZ);

	maxoff = PageGetMaxOffsetNumber(page);
	upper = phdr->pd_special;

	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
	{
		ItemId itemid = PageGetItemId(page, offnum);

		if (ItemIdHasStorage(itemid))
		{
			LocationIndex oldoff = ItemIdGetOffset(itemid);
			Size len = ItemIdGetLength(itemid);
			Size aligned_len = MAXALIGN(len);

			if (oldoff + len > BLCKSZ)
				die("item %u points outside page: off=%u len=%zu", offnum, oldoff, len);

			if (upper < aligned_len || upper - aligned_len < phdr->pd_lower)
				die("not enough space while repairing fragmentation");

			upper -= aligned_len;

			memmove((char *) page + upper, (char *) tmp + oldoff, len);
			ItemIdSetNormal(itemid, upper, len);
		}
	}

	phdr->pd_upper = upper;

	truncate_line_pointer_array(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= PageGetMaxOffsetNumber(page);
		 offnum = OffsetNumberNext(offnum))
	{
		if (!ItemIdIsUsed(PageGetItemId(page, offnum)))
		{
			PageSetHasFreeLinePointers(page);
			return;
		}
	}
	PageClearHasFreeLinePointers(page);
}

static int
find_matching_block(XLogReaderState *record,
					RelFileLocator target_rnode,
					BlockNumber target_blkno)
{
	int			block_id;

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		RelFileLocator rnode;
		ForkNumber	forknum;
		BlockNumber blkno;

		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		if (!get_block_tag(record, block_id, &rnode, &forknum, &blkno))
			continue;

		if (forknum == MAIN_FORKNUM &&
			blkno == target_blkno &&
			same_rnode(rnode, target_rnode))
			return block_id;
	}

	return -1;
}

static void
check_relation_wal(XLogReaderState *record, RelFileLocator target,
				   BlockNumber blkno)
{
	uint8 info;

	if (XLogRecGetRmid(record) != RM_SMGR_ID)
		return;
	info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec;

		if (XLogRecGetDataLen(record) < sizeof(xl_smgr_truncate))
			die("SMGR TRUNCATE main data too short");
		xlrec = (xl_smgr_truncate *) XLogRecGetData(record);
#if PG_VERSION_NUM >= 160000
		if (same_rnode(xlrec->rlocator, target) &&
#else
		if (same_rnode(xlrec->rnode, target) &&
#endif
			(xlrec->flags & SMGR_TRUNCATE_HEAP) && xlrec->blkno <= blkno)
			die("target heap block was truncated at LSN %X/%08X",
				(uint32) (record->ReadRecPtr >> 32),
				(uint32) record->ReadRecPtr);
	}
	else if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec;

		if (XLogRecGetDataLen(record) < sizeof(xl_smgr_create))
			die("SMGR CREATE main data too short");
		xlrec = (xl_smgr_create *) XLogRecGetData(record);
#if PG_VERSION_NUM >= 160000
		if (same_rnode(xlrec->rlocator, target) && xlrec->forkNum == MAIN_FORKNUM)
#else
		if (same_rnode(xlrec->rnode, target) && xlrec->forkNum == MAIN_FORKNUM)
#endif
			die("target relation was recreated at LSN %X/%08X",
				(uint32) (record->ReadRecPtr >> 32),
				(uint32) record->ReadRecPtr);
	}
}

static void
check_offnum(Page page, OffsetNumber offnum, const char *kind)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	if (offnum < FirstOffsetNumber || offnum > maxoff)
		die("%s offset out of range: off=%u maxoff=%u", kind, offnum, maxoff);
}

static void
fix_infomask_from_infobits(uint8 infobits, uint16 *infomask, uint16 *infomask2)
{
	if (infobits & XLHL_XMAX_IS_MULTI)
		*infomask |= HEAP_XMAX_IS_MULTI;
	if (infobits & XLHL_XMAX_LOCK_ONLY)
		*infomask |= HEAP_XMAX_LOCK_ONLY;
	if (infobits & XLHL_XMAX_EXCL_LOCK)
		*infomask |= HEAP_XMAX_EXCL_LOCK;
	if (infobits & XLHL_XMAX_KEYSHR_LOCK)
		*infomask |= HEAP_XMAX_KEYSHR_LOCK;
	if (infobits & XLHL_KEYS_UPDATED)
		*infomask2 |= HEAP_KEYS_UPDATED;
}

static void
set_tuple_xmin(HeapTupleHeader htup, TransactionId xid)
{
	htup->t_choice.t_heap.t_xmin = xid;
}

static void
set_tuple_xmax(HeapTupleHeader htup, TransactionId xid)
{
	htup->t_choice.t_heap.t_xmax = xid;
}

static void
set_tuple_cid(HeapTupleHeader htup, CommandId cid)
{
	htup->t_choice.t_heap.t_field3.t_cid = cid;
}

static HeapTupleHeader
get_normal_tuple(Page page, OffsetNumber offnum, ItemId *itemid)
{
	check_offnum(page, offnum, "tuple");
	*itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(*itemid))
		die("offset %u is not a normal heap tuple", offnum);
	if (ItemIdGetOffset(*itemid) + ItemIdGetLength(*itemid) > BLCKSZ ||
		ItemIdGetLength(*itemid) < SizeofHeapTupleHeader)
		die("offset %u points outside the page", offnum);
	return (HeapTupleHeader) PageGetItem(page, *itemid);
}

static void
freeze_tuple(Page page, OffsetNumber offnum, TransactionId xmax,
			 uint16 infomask, uint16 infomask2, uint8 flags)
{
	ItemId itemid;
	HeapTupleHeader htup = get_normal_tuple(page, offnum, &itemid);

	set_tuple_xmax(htup, xmax);
	if (flags & XLH_FREEZE_XVAC)
		HeapTupleHeaderSetXvac(htup, FrozenTransactionId);
	if (flags & XLH_INVALID_XVAC)
		HeapTupleHeaderSetXvac(htup, InvalidTransactionId);
	htup->t_infomask = infomask;
	htup->t_infomask2 = infomask2;
}

static void
apply_heap_delete(XLogReaderState *record, Page page, BlockNumber blkno)
{
	xl_heap_delete *xlrec;
	HeapTupleHeader htup;
	ItemId itemid;

	if (XLogRecGetDataLen(record) < SizeOfHeapDelete)
		die("DELETE main data too short");
	xlrec = (xl_heap_delete *) XLogRecGetData(record);
	htup = get_normal_tuple(page, xlrec->offnum, &itemid);
	htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	htup->t_infomask2 &= ~(HEAP_KEYS_UPDATED | HEAP_HOT_UPDATED);
	fix_infomask_from_infobits(xlrec->infobits_set,
						   &htup->t_infomask, &htup->t_infomask2);
	if (xlrec->flags & XLH_DELETE_IS_SUPER)
		set_tuple_xmin(htup, InvalidTransactionId);
	else
		set_tuple_xmax(htup, xlrec->xmax);
	set_tuple_cid(htup, FirstCommandId);
	set_prunable(page, XLogRecGetXid(record));
	if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
		PageClearAllVisible(page);
	if (xlrec->flags & XLH_DELETE_IS_PARTITION_MOVE)
		HeapTupleHeaderSetMovedPartitions(htup);
	else
		ItemPointerSet(&htup->t_ctid, blkno, xlrec->offnum);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_heap_confirm(XLogReaderState *record, Page page, BlockNumber blkno)
{
	xl_heap_confirm *xlrec;
	ItemId itemid;
	HeapTupleHeader htup;

	if (XLogRecGetDataLen(record) < SizeOfHeapConfirm)
		die("CONFIRM main data too short");
	xlrec = (xl_heap_confirm *) XLogRecGetData(record);
	htup = get_normal_tuple(page, xlrec->offnum, &itemid);
	ItemPointerSet(&htup->t_ctid, blkno, xlrec->offnum);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_heap_lock(XLogReaderState *record, Page page, BlockNumber blkno,
				bool updated)
{
	xl_heap_lock *xlrec;
	ItemId itemid;
	HeapTupleHeader htup;

	if (XLogRecGetDataLen(record) < SizeOfHeapLock)
		die("LOCK main data too short");
	xlrec = (xl_heap_lock *) XLogRecGetData(record);
	htup = get_normal_tuple(page, xlrec->offnum, &itemid);
	htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	fix_infomask_from_infobits(xlrec->infobits_set,
						   &htup->t_infomask, &htup->t_infomask2);
	if (!updated && HEAP_XMAX_IS_LOCKED_ONLY(htup->t_infomask))
	{
		htup->t_infomask2 &= ~HEAP_HOT_UPDATED;
		ItemPointerSet(&htup->t_ctid, blkno, xlrec->offnum);
	}
#if PG_VERSION_NUM < 160000
	set_tuple_xmax(htup, updated ? ((xl_heap_lock_updated *) xlrec)->xmax :
				   xlrec->locking_xid);
#else
	set_tuple_xmax(htup, xlrec->xmax);
#endif
	if (!updated)
		set_tuple_cid(htup, FirstCommandId);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_heap_inplace(XLogReaderState *record, Page page)
{
	OffsetNumber offnum;
	ItemId itemid;
	HeapTupleHeader htup;
	Size newlen;
	char *data;

	if (XLogRecGetDataLen(record) < sizeof(OffsetNumber))
		die("INPLACE main data too short");
	memcpy(&offnum, XLogRecGetData(record), sizeof(OffsetNumber));
	htup = get_normal_tuple(page, offnum, &itemid);
	data = XLogRecGetBlockData(record, 0, &newlen);
	if (data == NULL || htup->t_hoff > ItemIdGetLength(itemid) ||
		newlen != (Size) (ItemIdGetLength(itemid) - htup->t_hoff))
		die("INPLACE tuple data length mismatch");
	memcpy((char *) htup + htup->t_hoff, data, newlen);
	PageSetLSN(page, record->EndRecPtr);
}

static void
apply_heap_insert(XLogReaderState *record,
				  Page page,
				  int block_id,
				  BlockNumber blkno)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	xl_heap_insert *xlrec;
	Size		datalen;
	char	   *data;
	xl_heap_header xlhdr;
	uint32		newlen;
	TransactionId xid;
	ItemPointerData target_tid;

	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	} tbuf;

	HeapTupleHeader htup;
	char		lsnbuf[64];

	if (XLogRecGetRmid(record) != RM_HEAP_ID)
		die("record is not Heap rmgr");

	if ((info & XLOG_HEAP_OPMASK) != XLOG_HEAP_INSERT)
		die("unsupported Heap record info=0x%02X; this tool expected XLOG_HEAP_INSERT", info);

	if (XLogRecGetDataLen(record) < SizeOfHeapInsert)
		die("main data too short for xl_heap_insert");

	xlrec = (xl_heap_insert *) XLogRecGetData(record);

	data = XLogRecGetBlockData(record, block_id, &datalen);

	if (data == NULL)
		die("matching INSERT block has no block data");

	if (datalen <= SizeOfHeapHeader)
		die("INSERT block data too short: %zu", datalen);

	if (info & XLOG_HEAP_INIT_PAGE)
		init_heap_page_offline(page);

	newlen = datalen - SizeOfHeapHeader;

	if (newlen + SizeofHeapTupleHeader > MaxHeapTupleSize)
		die("INSERT tuple too large: payload=%u", newlen);

	memcpy(&xlhdr, data, SizeOfHeapHeader);
	data += SizeOfHeapHeader;

	htup = &tbuf.hdr;
	memset(htup, 0, SizeofHeapTupleHeader);

	memcpy((char *) htup + SizeofHeapTupleHeader, data, newlen);
	newlen += SizeofHeapTupleHeader;

	htup->t_infomask2 = xlhdr.t_infomask2;
	htup->t_infomask = xlhdr.t_infomask;
	htup->t_hoff = xlhdr.t_hoff;

	xid = XLogRecGetXid(record);

	set_tuple_xmin(htup, xid);
	set_tuple_cid(htup, FirstCommandId);

	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);
	htup->t_ctid = target_tid;

	add_heap_item(page, (Item) htup, newlen, xlrec->offnum);
	set_prunable(page, xid);

	PageSetLSN(page, record->EndRecPtr);

	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
		PageClearAllVisible(page);

	format_lsn(lsnbuf, sizeof(lsnbuf), record->ReadRecPtr);

	printf("applied Heap INSERT lsn=%s xid=%lu off=%u tuple_len=%u\n",
		   lsnbuf, (unsigned long) xid, xlrec->offnum, newlen);
}

static void
apply_heap_multi_insert(XLogReaderState *record, Page page, BlockNumber blkno)
{
	xl_heap_multi_insert *xlrec;
	bool isinit = (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE) != 0;
	Size mainlen = XLogRecGetDataLen(record);
	Size datalen;
	char *data;
	char *end;
	char lsnbuf[64];
	int i;

	if (mainlen < SizeOfHeapMultiInsert)
		die("MULTI_INSERT main data too short");
	xlrec = (xl_heap_multi_insert *) XLogRecGetData(record);
	if (xlrec->ntuples > MaxHeapTuplesPerPage ||
		(!isinit && mainlen < SizeOfHeapMultiInsert +
		 (Size) xlrec->ntuples * sizeof(OffsetNumber)))
		die("MULTI_INSERT offsets truncated");
	data = XLogRecGetBlockData(record, 0, &datalen);
	if (data == NULL)
		die("MULTI_INSERT has no tuple data");
	end = data + datalen;
	if (isinit)
		init_heap_page_offline(page);

	for (i = 0; i < xlrec->ntuples; i++)
	{
		OffsetNumber offnum = isinit ? FirstOffsetNumber + i : xlrec->offsets[i];
		xl_multi_insert_tuple xlhdr;
		union
		{
			HeapTupleHeaderData hdr;
			char data[MaxHeapTupleSize];
		} tbuf;
		HeapTupleHeader htup = &tbuf.hdr;
		Size newlen;

		data = (char *) SHORTALIGN(data);
		if (data > end || (Size) (end - data) < SizeOfMultiInsertTuple)
			die("MULTI_INSERT tuple header truncated");
		memcpy(&xlhdr, data, SizeOfMultiInsertTuple);
		data += SizeOfMultiInsertTuple;
		if ((Size) (end - data) < xlhdr.datalen ||
			xlhdr.datalen + SizeofHeapTupleHeader > MaxHeapTupleSize)
			die("MULTI_INSERT tuple data truncated or oversized");
		newlen = xlhdr.datalen + SizeofHeapTupleHeader;
		memset(htup, 0, SizeofHeapTupleHeader);
		memcpy((char *) htup + SizeofHeapTupleHeader, data, xlhdr.datalen);
		data += xlhdr.datalen;
		htup->t_infomask2 = xlhdr.t_infomask2;
		htup->t_infomask = xlhdr.t_infomask;
		htup->t_hoff = xlhdr.t_hoff;
		set_tuple_xmin(htup, XLogRecGetXid(record));
		set_tuple_cid(htup, FirstCommandId);
		ItemPointerSet(&htup->t_ctid, blkno, offnum);
		add_heap_item(page, (Item) htup, newlen, offnum);
	}
	if (data != end)
		die("MULTI_INSERT has trailing tuple data");
	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
		PageClearAllVisible(page);
	if (xlrec->flags & XLH_INSERT_ALL_FROZEN_SET)
	{
		PageSetAllVisible(page);
		((PageHeader) page)->pd_prune_xid = InvalidTransactionId;
	}
	else
		set_prunable(page, XLogRecGetXid(record));
	PageSetLSN(page, record->EndRecPtr);

	format_lsn(lsnbuf, sizeof(lsnbuf), record->ReadRecPtr);
	printf("applied Heap2 MULTI_INSERT lsn=%s xid=%lu tuples=%u flags=0x%02X\n",
		   lsnbuf, (unsigned long) XLogRecGetXid(record),
		   (unsigned int) xlrec->ntuples, (unsigned int) xlrec->flags);
}

static void
apply_update_old_tuple(XLogReaderState *record,
					   Page page,
					   xl_heap_update *xlrec,
					   bool hot_update)
{
	RelFileLocator new_rnode;
	ForkNumber	new_forknum;
	BlockNumber new_blkno;
	ItemPointerData new_tid;
	ItemId		lp;
	HeapTupleHeader htup;
	TransactionId xid;

	if (!get_block_tag(record, HEAP_UPDATE_BLKREF_HEAP_NEW,
						   &new_rnode, &new_forknum, &new_blkno))
		die("UPDATE has no new heap block ref");

	check_offnum(page, xlrec->old_offnum, "UPDATE old_off");

	lp = PageGetItemId(page, xlrec->old_offnum);
	if (!ItemIdIsNormal(lp))
		die("UPDATE old_off=%u is not a normal line pointer", xlrec->old_offnum);

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;

	if (hot_update)
	{
#ifdef HEAP_HOT_UPDATED
		htup->t_infomask2 |= HEAP_HOT_UPDATED;
#endif
	}
	else
	{
#ifdef HEAP_HOT_UPDATED
		htup->t_infomask2 &= ~HEAP_HOT_UPDATED;
#endif
	}

	fix_infomask_from_infobits(xlrec->old_infobits_set,
							   &htup->t_infomask,
							   &htup->t_infomask2);

	set_tuple_xmax(htup, xlrec->old_xmax);
	set_tuple_cid(htup, FirstCommandId);

	ItemPointerSetBlockNumber(&new_tid, new_blkno);
	ItemPointerSetOffsetNumber(&new_tid, xlrec->new_offnum);
	htup->t_ctid = new_tid;

	xid = XLogRecGetXid(record);
	set_prunable(page, xid);

	if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
		PageClearAllVisible(page);
}

static void
apply_update_new_tuple(XLogReaderState *record,
					   Page page,
					   xl_heap_update *xlrec,
					   bool hot_update)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	Size		datalen;
	char	   *data;
	char	   *recdata_end;
	xl_heap_header xlhdr;
	uint32		newlen;
	uint16		prefixlen = 0;
	uint16		suffixlen = 0;
	HeapTupleHeader oldhtup = NULL;
	Size oldlen = 0;
	ItemId olditemid;
	TransactionId xid;
	ItemPointerData new_tid;
	RelFileLocator new_rnode;
	ForkNumber	new_forknum;
	BlockNumber new_blkno;

	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	} tbuf;

	HeapTupleHeader htup;

	if (!get_block_tag(record, HEAP_UPDATE_BLKREF_HEAP_NEW,
						   &new_rnode, &new_forknum, &new_blkno))
		die("UPDATE has no new heap block ref");

	data = XLogRecGetBlockData(record, HEAP_UPDATE_BLKREF_HEAP_NEW, &datalen);

	if (data == NULL)
		die("UPDATE new heap block has no tuple data");

	recdata_end = data + datalen;

	if (xlrec->flags & XLH_UPDATE_PREFIX_FROM_OLD)
	{
		if (data + sizeof(uint16) > recdata_end)
			die("UPDATE prefix flag set but block data too short");
		memcpy(&prefixlen, data, sizeof(uint16));
		data += sizeof(uint16);
	}

	if (xlrec->flags & XLH_UPDATE_SUFFIX_FROM_OLD)
	{
		if (data + sizeof(uint16) > recdata_end)
			die("UPDATE suffix flag set but block data too short");
		memcpy(&suffixlen, data, sizeof(uint16));
		data += sizeof(uint16);
	}

	if (prefixlen != 0 || suffixlen != 0)
	{
		if (XLogRecHasBlockRef(record, HEAP_UPDATE_BLKREF_HEAP_OLD))
			die("UPDATE prefix/suffix refers to an unavailable old page");
		oldhtup = get_normal_tuple(page, xlrec->old_offnum, &olditemid);
		oldlen = ItemIdGetLength(olditemid);
		if (oldhtup->t_hoff > oldlen ||
			prefixlen > oldlen - oldhtup->t_hoff ||
			suffixlen > oldlen - oldhtup->t_hoff)
			die("UPDATE prefix/suffix exceeds old tuple");
	}

	if (data + SizeOfHeapHeader > recdata_end)
		die("UPDATE new tuple block data too short");

	if (info & XLOG_HEAP_INIT_PAGE)
		init_heap_page_offline(page);

	memcpy(&xlhdr, data, SizeOfHeapHeader);
	data += SizeOfHeapHeader;

	newlen = recdata_end - data;

	if (newlen + SizeofHeapTupleHeader + prefixlen + suffixlen > MaxHeapTupleSize)
		die("UPDATE tuple too large: payload=%u", newlen);

	htup = &tbuf.hdr;
	memset(htup, 0, SizeofHeapTupleHeader);

	if (prefixlen != 0)
	{
		Size header_extra = xlhdr.t_hoff - SizeofHeapTupleHeader;

		if (xlhdr.t_hoff < SizeofHeapTupleHeader || header_extra > newlen)
			die("UPDATE tuple header exceeds WAL payload");
		memcpy((char *) htup + SizeofHeapTupleHeader, data, header_extra);
		memcpy((char *) htup + xlhdr.t_hoff,
			   (char *) oldhtup + oldhtup->t_hoff, prefixlen);
		memcpy((char *) htup + xlhdr.t_hoff + prefixlen,
			   data + header_extra, newlen - header_extra);
	}
	else
		memcpy((char *) htup + SizeofHeapTupleHeader, data, newlen);
	if (suffixlen != 0)
		memcpy((char *) htup + SizeofHeapTupleHeader + newlen + prefixlen,
			   (char *) oldhtup + oldlen - suffixlen, suffixlen);
	newlen += SizeofHeapTupleHeader + prefixlen + suffixlen;

	htup->t_infomask2 = xlhdr.t_infomask2;
	htup->t_infomask = xlhdr.t_infomask;
	htup->t_hoff = xlhdr.t_hoff;

	xid = XLogRecGetXid(record);

	set_tuple_xmin(htup, xid);
	set_tuple_cid(htup, FirstCommandId);
	set_tuple_xmax(htup, xlrec->new_xmax);

	ItemPointerSetBlockNumber(&new_tid, new_blkno);
	ItemPointerSetOffsetNumber(&new_tid, xlrec->new_offnum);
	htup->t_ctid = new_tid;

	if (hot_update)
	{
#ifdef HEAP_ONLY_TUPLE
		htup->t_infomask2 |= HEAP_ONLY_TUPLE;
#endif
	}

	add_heap_item(page, (Item) htup, newlen, xlrec->new_offnum);

	if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
		PageClearAllVisible(page);
}

static void
apply_heap_update(XLogReaderState *record,
				  Page page,
				  int block_id)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	uint8		op = info & XLOG_HEAP_OPMASK;
	xl_heap_update *xlrec;
	bool		hot_update;
	bool		has_old_block;
	bool		target_is_new;
	bool		target_is_old;
	char		lsnbuf[64];

	if (XLogRecGetRmid(record) != RM_HEAP_ID)
		die("record is not Heap rmgr");

	if (op != XLOG_HEAP_UPDATE && op != XLOG_HEAP_HOT_UPDATE)
		die("unsupported Heap record info=0x%02X; this tool expected UPDATE/HOT_UPDATE", info);

	if (XLogRecGetDataLen(record) < SizeOfHeapUpdate)
		die("main data too short for xl_heap_update");

	xlrec = (xl_heap_update *) XLogRecGetData(record);
	hot_update = (op == XLOG_HEAP_HOT_UPDATE);

	has_old_block = XLogRecHasBlockRef(record, HEAP_UPDATE_BLKREF_HEAP_OLD);

	target_is_new = (block_id == HEAP_UPDATE_BLKREF_HEAP_NEW);
	target_is_old = (block_id == HEAP_UPDATE_BLKREF_HEAP_OLD) || !has_old_block;

	if (target_is_old)
		apply_update_old_tuple(record, page, xlrec, hot_update);

	if (target_is_new)
		apply_update_new_tuple(record, page, xlrec, hot_update);

	if (!target_is_old && !target_is_new)
		die("UPDATE matched unexpected block_id=%d", block_id);

	PageSetLSN(page, record->EndRecPtr);

	format_lsn(lsnbuf, sizeof(lsnbuf), record->ReadRecPtr);

	printf("applied Heap %s lsn=%s xid=%lu old_off=%u new_off=%u target_part=%s%s\n",
		   hot_update ? "HOT_UPDATE" : "UPDATE",
		   lsnbuf,
		   (unsigned long) XLogRecGetXid(record),
		   xlrec->old_offnum,
		   xlrec->new_offnum,
		   target_is_old ? "old" : "",
		   target_is_new ? "+new" : "");
}

#if PG_VERSION_NUM < 170000
static void
apply_heap2_prune_legacy(XLogReaderState *record, Page page, int block_id,
						 bool vacuum)
{
	Size datalen;
	char *data = XLogRecGetBlockData(record, block_id, &datalen);
	OffsetNumber *offsets;
	int nredirected = 0;
	int ndead = 0;
	int nunused;
	int i;

	if (data == NULL)
		die("Heap2 PRUNE/VACUUM has no block data");
	if (vacuum)
	{
		xl_heap_vacuum *xlrec;

		if (XLogRecGetDataLen(record) < SizeOfHeapVacuum)
			die("Heap2 VACUUM main data too short");
		xlrec = (xl_heap_vacuum *) XLogRecGetData(record);
		nunused = xlrec->nunused;
		if (datalen != (Size) nunused * sizeof(OffsetNumber))
			die("Heap2 VACUUM block data has unexpected length");
	}
	else
	{
		xl_heap_prune *xlrec;
		int nitems;

		if (XLogRecGetDataLen(record) < SizeOfHeapPrune)
			die("Heap2 PRUNE main data too short");
		xlrec = (xl_heap_prune *) XLogRecGetData(record);
		nredirected = xlrec->nredirected;
		ndead = xlrec->ndead;
		if (datalen % sizeof(OffsetNumber) != 0)
			die("Heap2 PRUNE block data is misaligned");
		nitems = datalen / sizeof(OffsetNumber);
		nunused = nitems - 2 * nredirected - ndead;
		if (nunused < 0)
			die("Heap2 PRUNE block data too short");
	}

	offsets = (OffsetNumber *) data;
	for (i = 0; i < nredirected; i++)
	{
		OffsetNumber from = offsets[2 * i];
		OffsetNumber to = offsets[2 * i + 1];

		check_offnum(page, from, "redirect-from");
		check_offnum(page, to, "redirect-to");
		ItemIdSetRedirect(PageGetItemId(page, from), to);
	}
	for (i = 0; i < ndead; i++)
	{
		OffsetNumber off = offsets[2 * nredirected + i];

		check_offnum(page, off, "dead");
		ItemIdSetDead(PageGetItemId(page, off));
	}
	for (i = 0; i < nunused; i++)
	{
		OffsetNumber off = offsets[2 * nredirected + ndead + i];

		check_offnum(page, off, "unused");
		ItemIdSetUnused(PageGetItemId(page, off));
	}
	if (nredirected || ndead || nunused)
		repair_page_fragmentation(page);
	PageSetLSN(page, record->EndRecPtr);
	printf("applied Heap2 %s: redirected=%d dead=%d unused=%d\n",
		   vacuum ? "VACUUM" : "PRUNE", nredirected, ndead, nunused);
}

static void
apply_heap2_freeze_legacy(XLogReaderState *record, Page page, int block_id)
{
	xl_heap_freeze_page *xlrec;
	Size datalen;
	char *data;
	int i;

	if (XLogRecGetDataLen(record) < SizeOfHeapFreezePage)
		die("Heap2 FREEZE_PAGE main data too short");
	xlrec = (xl_heap_freeze_page *) XLogRecGetData(record);
	data = XLogRecGetBlockData(record, block_id, &datalen);
	if (data == NULL)
		die("Heap2 FREEZE_PAGE has no block data");
#if PG_VERSION_NUM < 160000
	if (datalen != (Size) xlrec->ntuples * sizeof(xl_heap_freeze_tuple))
		die("Heap2 FREEZE_PAGE tuple array has wrong length");
	for (i = 0; i < xlrec->ntuples; i++)
	{
		xl_heap_freeze_tuple frz;

		memcpy(&frz, data + i * sizeof(frz), sizeof(frz));
		freeze_tuple(page, frz.offset, frz.xmax, frz.t_infomask,
					 frz.t_infomask2, frz.frzflags);
	}
#else
	{
		Size plans_size = (Size) xlrec->nplans * sizeof(xl_heap_freeze_plan);
		char *offsets;
		char *end = data + datalen;

		if (plans_size > datalen)
			die("Heap2 FREEZE_PAGE plans truncated");
		offsets = data + plans_size;
		for (i = 0; i < xlrec->nplans; i++)
		{
			xl_heap_freeze_plan frz;
			int j;

			memcpy(&frz, data + i * sizeof(frz), sizeof(frz));
			for (j = 0; j < frz.ntuples; j++)
			{
				OffsetNumber offnum;

				if ((Size) (end - offsets) < sizeof(offnum))
					die("Heap2 FREEZE_PAGE offsets truncated");
				memcpy(&offnum, offsets, sizeof(offnum));
				offsets += sizeof(offnum);
				freeze_tuple(page, offnum, frz.xmax, frz.t_infomask,
							 frz.t_infomask2, frz.frzflags);
			}
		}
		if (offsets != end)
			die("Heap2 FREEZE_PAGE has trailing data");
	}
#endif
	PageSetLSN(page, record->EndRecPtr);
}
#endif

#if PG_VERSION_NUM >= 170000
static char *
read_prune_items(char *cursor, char *end,
				 const char *name,
				 int multiplier,
				 int *nitems,
				 OffsetNumber **items)
{
	xlhp_prune_items *subrecord;
	Size needed;

	if (cursor + offsetof(xlhp_prune_items, data) > end)
		die("PRUNE block data too short before %s subrecord", name);

	subrecord = (xlhp_prune_items *) cursor;
	*nitems = subrecord->ntargets;
	*items = subrecord->data;

	needed = offsetof(xlhp_prune_items, data) +
			 sizeof(OffsetNumber) * multiplier * (*nitems);

	if (cursor + needed > end)
		die("PRUNE block data too short in %s subrecord: need=%zu available=%zu",
			name, needed, (Size) (end - cursor));

	return cursor + needed;
}

static const char *
prune_op_name(uint8 op)
{
	switch (op)
	{
		case XLOG_HEAP2_PRUNE_ON_ACCESS:
			return "PRUNE_ON_ACCESS";
		case XLOG_HEAP2_PRUNE_VACUUM_SCAN:
			return "PRUNE_VACUUM_SCAN";
		case XLOG_HEAP2_PRUNE_VACUUM_CLEANUP:
			return "PRUNE_VACUUM_CLEANUP";
		default:
			return "PRUNE_UNKNOWN";
	}
}

static void
apply_heap2_prune_pg17(XLogReaderState *record,
					   Page page,
					   int block_id)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	uint8		op = info & XLOG_HEAP_OPMASK;
	xl_heap_prune *xlrec;
	Size		datalen;
	char	   *data;
	char	   *cursor;
	char	   *end;
	OffsetNumber *redirected = NULL;
	OffsetNumber *nowdead = NULL;
	OffsetNumber *nowunused = NULL;
	xlhp_freeze_plan *plans = NULL;
	int nplans = 0;
	int			nredirected = 0;
	int			ndead = 0;
	int			nunused = 0;
	int			i;
	char		lsnbuf[64];

	if (XLogRecGetRmid(record) != RM_HEAP2_ID)
		die("record is not Heap2 rmgr");

	if (op != XLOG_HEAP2_PRUNE_ON_ACCESS &&
		op != XLOG_HEAP2_PRUNE_VACUUM_SCAN &&
		op != XLOG_HEAP2_PRUNE_VACUUM_CLEANUP)
	{
		die("unsupported Heap2 record info=0x%02X; this tool supports only PRUNE_*", info);
	}

	if (XLogRecGetDataLen(record) < SizeOfHeapPrune)
		die("main data too short for xl_heap_prune");

	xlrec = (xl_heap_prune *) XLogRecGetData(record);

	data = XLogRecGetBlockData(record, block_id, &datalen);

	if (data == NULL)
		die("PRUNE matching block has no block data");

	cursor = data;
	end = data + datalen;
	if (xlrec->flags & XLHP_HAS_FREEZE_PLANS)
	{
		xlhp_freeze_plans *header;
		Size bytes;

		if ((Size) (end - cursor) < offsetof(xlhp_freeze_plans, plans))
			die("PRUNE freeze plan header truncated");
		header = (xlhp_freeze_plans *) cursor;
		nplans = header->nplans;
		bytes = offsetof(xlhp_freeze_plans, plans) +
			(Size) nplans * sizeof(xlhp_freeze_plan);
		if (bytes > (Size) (end - cursor))
			die("PRUNE freeze plans truncated");
		plans = header->plans;
		cursor += bytes;
	}

	if (xlrec->flags & XLHP_HAS_REDIRECTIONS)
		cursor = read_prune_items(cursor, end, "redirected", 2,
								  &nredirected, &redirected);

	if (xlrec->flags & XLHP_HAS_DEAD_ITEMS)
		cursor = read_prune_items(cursor, end, "dead", 1,
								  &ndead, &nowdead);

	if (xlrec->flags & XLHP_HAS_NOW_UNUSED_ITEMS)
		cursor = read_prune_items(cursor, end, "unused", 1,
								  &nunused, &nowunused);

	if (cursor > end)
		die("PRUNE parser overran block data");

	for (i = 0; i < nredirected; i++)
	{
		OffsetNumber from = redirected[i * 2];
		OffsetNumber to = redirected[i * 2 + 1];
		ItemId		lp;

		check_offnum(page, from, "redirect-from");
		check_offnum(page, to, "redirect-to");

		lp = PageGetItemId(page, from);
		ItemIdSetRedirect(lp, to);
	}

	for (i = 0; i < ndead; i++)
	{
		OffsetNumber off = nowdead[i];
		ItemId		lp;

		check_offnum(page, off, "dead");

		lp = PageGetItemId(page, off);
		ItemIdSetDead(lp);
	}

	for (i = 0; i < nunused; i++)
	{
		OffsetNumber off = nowunused[i];
		ItemId		lp;

		check_offnum(page, off, "unused");

		lp = PageGetItemId(page, off);
		ItemIdSetUnused(lp);
	}

	if (nredirected > 0 || ndead > 0 || nunused > 0)
		repair_page_fragmentation(page);
	for (i = 0; i < nplans; i++)
	{
		int j;

		for (j = 0; j < plans[i].ntuples; j++)
		{
			OffsetNumber offnum;

			if ((Size) (end - cursor) < sizeof(offnum))
				die("PRUNE freeze offsets truncated");
			memcpy(&offnum, cursor, sizeof(offnum));
			cursor += sizeof(offnum);
			freeze_tuple(page, offnum, plans[i].xmax,
						 plans[i].t_infomask, plans[i].t_infomask2,
						 plans[i].frzflags);
		}
	}
	if (cursor != end)
		die("PRUNE has trailing block data");

	PageSetLSN(page, record->EndRecPtr);

	format_lsn(lsnbuf, sizeof(lsnbuf), record->ReadRecPtr);

	printf("applied Heap2 %s lsn=%s flags=0x%04X nredirected=%d ndead=%d nunused=%d\n",
		   prune_op_name(op),
		   lsnbuf,
		   xlrec->flags,
		   nredirected,
		   ndead,
		   nunused);
}
#endif

static void
usage(const char *progname)
{
	fprintf(stderr,
			"Usage:\n"
			"  %s --waldir=DIR --fpi=FILE --out=FILE \\\n"
			"     --rel=T/D/R --block=N --start=LSN --stop=LSN \\\n"
			"     [--tli=N] [--segsize=BYTES] [--data-checksums]\n\n"
			"Example:\n"
			"  %s \\\n"
			"    --waldir=/path/to/wals \\\n"
			"    --fpi=/path/to/fpi/00000001-00002A9C-51FB7280.1663.24605.2072200.847224_main \\\n"
			"    --out=/path/to/page_after.bin \\\n"
			"    --rel=1663/24605/2072200 \\\n"
			"    --block=847224 \\\n"
			"    --start=2A9C/51FB7280 \\\n"
			"    --stop=2A9C/51FB7D98 \\\n"
			"    --tli=1\n",
			progname, progname);
}

int
main(int argc, char **argv)
{
	ReplayOptions opts;
	ReplayPrivate private;
	XLogReaderRoutine routine;
	XLogReaderState *reader;
	XLogRecord *record;
	XLogRecPtr first_record;
	char	   *errormsg = NULL;
	PGAlignedBlock pagebuf;
	Page		page = (Page) pagebuf.data;
	int			applied = 0;

	static struct option long_options[] =
	{
		{"waldir",	required_argument, NULL, 1},
		{"fpi",	required_argument, NULL, 2},
		{"out",	required_argument, NULL, 3},
		{"rel",	required_argument, NULL, 4},
		{"block",	required_argument, NULL, 5},
		{"start",	required_argument, NULL, 6},
		{"stop",	required_argument, NULL, 7},
		{"tli",	required_argument, NULL, 8},
		{"segsize", required_argument, NULL, 9},
		{"data-checksums", no_argument, NULL, 10},
		{NULL, 0, NULL, 0}
	};

	memset(&opts, 0, sizeof(opts));

	opts.timeline = 1;
	opts.segsize = DEFAULT_XLOG_SEG_SIZE;
	opts.start_lsn = InvalidXLogRecPtr;
	opts.stop_lsn = InvalidXLogRecPtr;
	opts.blkno = InvalidBlockNumber;

	while (1)
	{
		int option_index = 0;
		int c = getopt_long(argc, argv, "", long_options, &option_index);

		if (c == -1)
			break;

		switch (c)
		{
			case 1:
				opts.waldir = optarg;
				break;
			case 2:
				opts.fpi_path = optarg;
				break;
			case 3:
				opts.out_path = optarg;
				break;
			case 4:
				if (!parse_relation(optarg, &opts.rnode))
					die("invalid --rel value: %s", optarg);
				opts.has_relation = true;
				break;
			case 5:
				if (sscanf(optarg, "%u", &opts.blkno) != 1)
					die("invalid --block value: %s", optarg);
				break;
			case 6:
				if (!parse_lsn(optarg, &opts.start_lsn))
					die("invalid --start LSN: %s", optarg);
				break;
			case 7:
				if (!parse_lsn(optarg, &opts.stop_lsn))
					die("invalid --stop LSN: %s", optarg);
				break;
			case 8:
				if (sscanf(optarg, "%u", &opts.timeline) != 1)
					die("invalid --tli value: %s", optarg);
				break;
			case 9:
				if (sscanf(optarg, "%d", &opts.segsize) != 1)
					die("invalid --segsize value: %s", optarg);
				break;
			case 10:
				opts.force_checksums = true;
				break;
			default:
				usage(argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	if (opts.waldir == NULL ||
		opts.fpi_path == NULL ||
		opts.out_path == NULL ||
		!opts.has_relation ||
		XLogRecPtrIsInvalid(opts.start_lsn) ||
		XLogRecPtrIsInvalid(opts.stop_lsn) ||
		opts.blkno == InvalidBlockNumber)
	{
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if (opts.stop_lsn < opts.start_lsn)
		die("--stop must be >= --start");
	if (!IsValidWalSegSize(opts.segsize))
		die("invalid --segsize: %d", opts.segsize);
	if (opts.timeline == 0)
		die("--tli must be greater than zero");

	WalSegSz = opts.segsize;

	read_input_page(opts.fpi_path, page);
	check_page_pointers(page);
	input_has_checksum = opts.force_checksums ||
		(((PageHeader) page)->pd_checksum != 0 &&
		 ((PageHeader) page)->pd_checksum == pg_checksum_page(page, opts.blkno));

	private.timeline = opts.timeline;

	memset(&routine, 0, sizeof(routine));
	routine.page_read = ReplayReadPage;
	routine.segment_open = ReplayOpenSegment;
	routine.segment_close = ReplayCloseSegment;

	reader = XLogReaderAllocate(WalSegSz, opts.waldir, &routine, &private);
	if (reader == NULL)
		die("XLogReaderAllocate failed");

	first_record = XLogFindNextRecord(reader, opts.start_lsn);
	if (XLogRecPtrIsInvalid(first_record))
		die("could not find first WAL record at or after start LSN");

	XLogBeginRead(reader, first_record);

	while ((record = XLogReadRecord(reader, &errormsg)) != NULL)
	{
		XLogRecPtr	recptr = reader->ReadRecPtr;
		RmgrId		rmid;
		int			block_id;

		if (recptr < opts.start_lsn)
			continue;

		if (recptr > opts.stop_lsn)
			break;
		check_relation_wal(reader, opts.rnode, opts.blkno);

		block_id = find_matching_block(reader, opts.rnode, opts.blkno);
		if (block_id < 0)
			goto record_done;
		if (PageGetLSN(page) >= reader->EndRecPtr)
			goto record_done;

		/* A usable FPI supersedes the record's incremental change. */
		if (XLogRecHasBlockImage(reader, block_id) &&
			XLogRecBlockImageApply(reader, block_id))
		{
			char lsnbuf[64];

			if (!RestoreBlockImage(reader, block_id, page))
				die("could not restore full-page image at target block");
			apply_after_block_image(reader, page, block_id);
			PageSetLSN(page, reader->EndRecPtr);
			format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
			printf("applied FPI lsn=%s rmgr=%u block=%u\n",
				   lsnbuf, (unsigned int) XLogRecGetRmid(reader),
				   (unsigned int) opts.blkno);
			applied++;
			goto record_done;
		}

		rmid = XLogRecGetRmid(reader);

		if (rmid == RM_HEAP_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
			uint8 op = info & XLOG_HEAP_OPMASK;

			if (op == XLOG_HEAP_INSERT)
			{
				apply_heap_insert(reader, page, block_id, opts.blkno);
				applied++;
				goto record_done;
			}

			if (op == XLOG_HEAP_UPDATE || op == XLOG_HEAP_HOT_UPDATE)
			{
				apply_heap_update(reader, page, block_id);
				applied++;
				goto record_done;
			}
			if (op == XLOG_HEAP_DELETE)
			{
				apply_heap_delete(reader, page, opts.blkno);
				applied++;
				goto record_done;
			}
			if (op == XLOG_HEAP_CONFIRM)
			{
				apply_heap_confirm(reader, page, opts.blkno);
				applied++;
				goto record_done;
			}
			if (op == XLOG_HEAP_LOCK)
			{
				apply_heap_lock(reader, page, opts.blkno, false);
				applied++;
				goto record_done;
			}
			if (op == XLOG_HEAP_INPLACE)
			{
				apply_heap_inplace(reader, page);
				applied++;
				goto record_done;
			}

			die("unsupported Heap record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_HEAP2_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
			uint8 op = info & XLOG_HEAP_OPMASK;

			if (op == XLOG_HEAP2_MULTI_INSERT)
			{
				apply_heap_multi_insert(reader, page, opts.blkno);
				applied++;
				goto record_done;
			}
			if (op == XLOG_HEAP2_LOCK_UPDATED)
			{
				apply_heap_lock(reader, page, opts.blkno, true);
				applied++;
				goto record_done;
			}
			if (op == XLOG_HEAP2_VISIBLE)
			{
				if (block_id != 1)
					die("VISIBLE matched an unexpected heap block reference");
				PageSetAllVisible(page);
				((PageHeader) page)->pd_prune_xid = InvalidTransactionId;
				if (input_has_checksum)
					PageSetLSN(page, reader->EndRecPtr);
				applied++;
				goto record_done;
			}

#if PG_VERSION_NUM >= 170000
			if (op == XLOG_HEAP2_PRUNE_ON_ACCESS ||
				op == XLOG_HEAP2_PRUNE_VACUUM_SCAN ||
				op == XLOG_HEAP2_PRUNE_VACUUM_CLEANUP)
			{
				apply_heap2_prune_pg17(reader, page, block_id);
				applied++;
				goto record_done;
			}
#else
			if (op == XLOG_HEAP2_PRUNE || op == XLOG_HEAP2_VACUUM)
			{
				apply_heap2_prune_legacy(reader, page, block_id,
									 op == XLOG_HEAP2_VACUUM);
				applied++;
				goto record_done;
			}
			if (op == XLOG_HEAP2_FREEZE_PAGE)
			{
				apply_heap2_freeze_legacy(reader, page, block_id);
				applied++;
				goto record_done;
			}
#endif

			die("unsupported Heap2 record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_BTREE_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
			char lsnbuf[64];

			if (info == XLOG_BTREE_INSERT_LEAF ||
				info == XLOG_BTREE_INSERT_UPPER ||
				info == XLOG_BTREE_INSERT_META)
			{
				if (block_id == 0)
					apply_btree_insert(reader, page, block_id,
						info == XLOG_BTREE_INSERT_LEAF ? "INSERT_LEAF" :
						info == XLOG_BTREE_INSERT_UPPER ? "INSERT_UPPER" :
						"INSERT_META");
				else if (block_id == 1 && info != XLOG_BTREE_INSERT_LEAF)
				{
					apply_btree_clear_incomplete_split(page);
					PageSetLSN(page, reader->EndRecPtr);
				}
				else if (block_id == 2 && info == XLOG_BTREE_INSERT_META)
					apply_btree_meta(reader, page, block_id);
				else
					die("invalid B-tree INSERT target block reference %d", block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_BTREE_SPLIT_L || info == XLOG_BTREE_SPLIT_R)
			{
				apply_btree_split(reader, page, block_id,
								  info == XLOG_BTREE_SPLIT_L);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Btree %s lsn=%s block_ref=%d\n",
					   info == XLOG_BTREE_SPLIT_L ? "SPLIT_L" : "SPLIT_R",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_BTREE_INSERT_POST || info == XLOG_BTREE_DEDUP)
			{
				if (info == XLOG_BTREE_INSERT_POST)
					apply_btree_insert_post(reader, page, block_id);
				else
					apply_btree_dedup(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Btree %s lsn=%s\n",
					   info == XLOG_BTREE_INSERT_POST ? "INSERT_POST" : "DEDUP",
					   lsnbuf);
				applied++;
				goto record_done;
			}
			if (info == XLOG_BTREE_NEWROOT)
			{
				apply_btree_newroot(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Btree NEWROOT lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_BTREE_META_CLEANUP && block_id == 0)
			{
				apply_btree_meta(reader, page, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_BTREE_DELETE || info == XLOG_BTREE_VACUUM)
			{
				apply_btree_delete_vacuum(reader, page, block_id,
								  info == XLOG_BTREE_VACUUM);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Btree %s lsn=%s\n",
					   info == XLOG_BTREE_VACUUM ? "VACUUM" : "DELETE",
					   lsnbuf);
				applied++;
				goto record_done;
			}
			if (info == XLOG_BTREE_MARK_PAGE_HALFDEAD)
			{
				apply_btree_mark_halfdead(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Btree MARK_PAGE_HALFDEAD lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_BTREE_UNLINK_PAGE ||
				info == XLOG_BTREE_UNLINK_PAGE_META)
			{
				apply_btree_unlink_page(reader, page, block_id,
								info == XLOG_BTREE_UNLINK_PAGE_META);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Btree %s lsn=%s block_ref=%d\n",
					   info == XLOG_BTREE_UNLINK_PAGE_META ?
					   "UNLINK_PAGE_META" : "UNLINK_PAGE", lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			die("unsupported B-tree record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_HASH_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

			if (apply_after_block_image(reader, page, block_id))
			{
				applied++;
				goto record_done;
			}
			if (info == XLOG_HASH_INIT_META_PAGE ||
				info == XLOG_HASH_INIT_BITMAP_PAGE)
			{
				char lsnbuf[64];
				const char *name;

				if (info == XLOG_HASH_INIT_META_PAGE)
				{
					apply_hash_init_meta(reader, page, block_id);
					name = "INIT_META_PAGE";
				}
				else
				{
					apply_hash_init_bitmap(reader, page, block_id);
					name = "INIT_BITMAP_PAGE";
				}
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Hash %s lsn=%s block_ref=%d\n",
					   name, lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_HASH_ADD_OVFL_PAGE)
			{
				char lsnbuf[64];

				apply_hash_add_overflow(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Hash ADD_OVFL_PAGE lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_HASH_MOVE_PAGE_CONTENTS ||
				info == XLOG_HASH_SQUEEZE_PAGE)
			{
				bool changed;
				char lsnbuf[64];
				const char *name;

				if (info == XLOG_HASH_MOVE_PAGE_CONTENTS)
				{
					changed = apply_hash_move_contents(reader, page, block_id);
					name = "MOVE_PAGE_CONTENTS";
				}
				else
				{
					changed = apply_hash_squeeze_page(reader, page, block_id);
					name = "SQUEEZE_PAGE";
				}
				if (changed)
				{
					format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
					printf("applied Hash %s lsn=%s block_ref=%d\n",
						   name, lsnbuf, block_id);
					applied++;
				}
				goto record_done;
			}
			if (info == XLOG_HASH_INSERT)
			{
				char lsnbuf[64];

				apply_hash_insert(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Hash INSERT lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_HASH_SPLIT_ALLOCATE_PAGE && block_id == 2)
			{
				char lsnbuf[64];

				apply_hash_split_meta(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Hash SPLIT_ALLOCATE_PAGE lsn=%s block_ref=2\n",
					   lsnbuf);
				applied++;
				goto record_done;
			}
			if (info == XLOG_HASH_SPLIT_ALLOCATE_PAGE && block_id == 1)
			{
				xl_hash_split_allocate_page *xlrec;
				OfflineHashPageOpaqueData *opaque;
				char lsnbuf[64];

				if (XLogRecGetDataLen(reader) < SizeOfHashSplitAllocPage)
					die("invalid Hash SPLIT_ALLOCATE_PAGE WAL header");
				xlrec = (xl_hash_split_allocate_page *) XLogRecGetData(reader);
				init_index_page_offline(page, sizeof(OfflineHashPageOpaqueData));
				opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
				opaque->hasho_prevblkno = xlrec->new_bucket;
				opaque->hasho_nextblkno = InvalidBlockNumber;
				opaque->hasho_bucket = xlrec->new_bucket;
				opaque->hasho_flag = xlrec->new_bucket_flag;
				opaque->hasho_page_id = 0xFF80;
				PageSetLSN(page, reader->EndRecPtr);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Hash SPLIT_ALLOCATE_PAGE lsn=%s block_ref=1\n",
					   lsnbuf);
				applied++;
				goto record_done;
			}
			if (info == XLOG_HASH_DELETE)
			{
				if (apply_hash_delete(reader, page, block_id))
				{
					char lsnbuf[64];

					format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
					printf("applied Hash DELETE lsn=%s block_ref=%d\n",
						   lsnbuf, block_id);
					applied++;
				}
				goto record_done;
			}
			if (info == XLOG_HASH_SPLIT_CLEANUP)
			{
				char lsnbuf[64];

				apply_hash_split_cleanup(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Hash SPLIT_CLEANUP lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_HASH_UPDATE_META_PAGE ||
				info == XLOG_HASH_VACUUM_ONE_PAGE)
			{
				char lsnbuf[64];
				const char *name;

				if (info == XLOG_HASH_UPDATE_META_PAGE)
				{
					apply_hash_update_meta(reader, page, block_id);
					name = "UPDATE_META_PAGE";
				}
				else
				{
					apply_hash_vacuum_one_page(reader, page, block_id);
					name = "VACUUM_ONE_PAGE";
				}
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied Hash %s lsn=%s block_ref=%d\n",
					   name, lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			die("unsupported Hash record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_GIST_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

			if (apply_after_block_image(reader, page, block_id))
			{
				applied++;
				goto record_done;
			}
			if (info == OFFLINE_XLOG_GIST_PAGE_UPDATE)
			{
				char lsnbuf[64];

				apply_gist_page_update(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied GiST PAGE_UPDATE lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == OFFLINE_XLOG_GIST_DELETE ||
				info == OFFLINE_XLOG_GIST_PAGE_SPLIT ||
				info == OFFLINE_XLOG_GIST_PAGE_DELETE)
			{
				char lsnbuf[64];
				const char *name;

				if (info == OFFLINE_XLOG_GIST_DELETE)
				{
					apply_gist_delete(reader, page, block_id);
					name = "DELETE";
				}
				else if (info == OFFLINE_XLOG_GIST_PAGE_SPLIT)
				{
					apply_gist_page_split(reader, page, block_id, opts.blkno);
					name = "PAGE_SPLIT";
				}
				else
				{
					apply_gist_page_delete(reader, page, block_id);
					name = "PAGE_DELETE";
				}
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied GiST %s lsn=%s block_ref=%d\n",
					   name, lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			die("unsupported GiST record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_GIN_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
			char lsnbuf[64];
			const char *name = NULL;

			if (info == OFFLINE_XLOG_GIN_INSERT)
			{
				if (block_id == 1)
				{
					GinPageOpaqueData *opaque =
						(GinPageOpaqueData *) PageGetSpecialPointer(page);

					opaque->flags &= ~GIN_INCOMPLETE_SPLIT;
					PageSetLSN(page, reader->EndRecPtr);
					name = "CLEAR_INCOMPLETE_SPLIT";
				}
				else
				{
					apply_gin_insert(reader, page, block_id);
					name = "INSERT";
				}
			}
			else if (info == XLOG_GIN_CREATE_PTREE)
			{
				apply_gin_create_ptree(reader, page, block_id);
				name = "CREATE_PTREE";
			}
			else if (info == XLOG_GIN_INSERT_LISTPAGE)
			{
				apply_gin_insert_listpage(reader, page, block_id);
				name = "INSERT_LISTPAGE";
			}
			else if (info == XLOG_GIN_UPDATE_META_PAGE)
			{
				apply_gin_update_meta(reader, page, block_id);
				name = "UPDATE_META_PAGE";
			}
			else if (info == XLOG_GIN_DELETE_LISTPAGE)
			{
				apply_gin_delete_listpage(reader, page, block_id);
				name = "DELETE_LISTPAGE";
			}
			else if (info == XLOG_GIN_DELETE_PAGE)
			{
				apply_gin_delete_page(reader, page, block_id);
				name = "DELETE_PAGE";
			}
			else if (info == XLOG_GIN_VACUUM_DATA_LEAF_PAGE)
			{
				Size len;
				char *data;

				if (block_id != 0)
					die("invalid GIN VACUUM_DATA_LEAF_PAGE block reference");
				data = XLogRecGetBlockData(reader, 0, &len);
				gin_recompress_page(page, data, len);
				PageSetLSN(page, reader->EndRecPtr);
				name = "VACUUM_DATA_LEAF_PAGE";
			}
			else if (info == XLOG_GIN_SPLIT && block_id == 3)
			{
				GinPageOpaqueData *opaque =
					(GinPageOpaqueData *) PageGetSpecialPointer(page);

				opaque->flags &= ~GIN_INCOMPLETE_SPLIT;
				PageSetLSN(page, reader->EndRecPtr);
				name = "CLEAR_INCOMPLETE_SPLIT";
			}
			if (name != NULL)
			{
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied GIN %s lsn=%s block_ref=%d\n",
					   name, lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			die("unsupported GIN record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_SPGIST_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

			if (info == OFFLINE_XLOG_SPGIST_ADD_LEAF)
			{
				char lsnbuf[64];

				apply_spgist_add_leaf(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST ADD_LEAF lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_SPGIST_VACUUM_ROOT)
			{
				char lsnbuf[64];

				apply_spgist_vacuum_root(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST VACUUM_ROOT lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_SPGIST_SPLIT_TUPLE)
			{
				char lsnbuf[64];

				apply_spgist_split_tuple(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST SPLIT_TUPLE lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_SPGIST_MOVE_LEAFS)
			{
				char lsnbuf[64];

				apply_spgist_move_leafs(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST MOVE_LEAFS lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_SPGIST_PICKSPLIT)
			{
				char lsnbuf[64];

				apply_spgist_picksplit(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST PICKSPLIT lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_SPGIST_VACUUM_REDIRECT)
			{
				char lsnbuf[64];

				apply_spgist_vacuum_redirect(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST VACUUM_REDIRECT lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_SPGIST_ADD_NODE)
			{
				char lsnbuf[64];

				apply_spgist_add_node(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST ADD_NODE lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			if (info == XLOG_SPGIST_VACUUM_LEAF)
			{
				char lsnbuf[64];

				apply_spgist_vacuum_leaf(reader, page, block_id);
				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied SP-GiST VACUUM_LEAF lsn=%s block_ref=%d\n",
					   lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			die("unsupported SP-GiST record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_BRIN_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
			uint8 op = info & OFFLINE_XLOG_BRIN_OPMASK;
			const char *name = NULL;

			if (op == 0x00)
			{
				apply_brin_create_index(reader, page, block_id);
				name = "CREATE_INDEX";
			}
			else if (op == OFFLINE_XLOG_BRIN_INSERT)
			{
				apply_brin_insert(reader, page, block_id);
				name = "INSERT";
			}
			else if (op == OFFLINE_XLOG_BRIN_UPDATE)
			{
				apply_brin_update(reader, page, block_id);
				name = "UPDATE";
			}
			else if (op == OFFLINE_XLOG_BRIN_SAMEPAGE_UPDATE)
			{
				apply_brin_samepage_update(reader, page, block_id);
				name = "SAMEPAGE_UPDATE";
			}
			else if (op == OFFLINE_XLOG_BRIN_REVMAP_EXTEND)
			{
				apply_brin_revmap_extend(reader, page, block_id);
				name = "REVMAP_EXTEND";
			}
			else if (op == OFFLINE_XLOG_BRIN_DESUMMARIZE)
			{
				apply_brin_desummarize(reader, page, block_id);
				name = "DESUMMARIZE";
			}
			if (name != NULL)
			{
				char lsnbuf[64];

				format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
				printf("applied BRIN %s lsn=%s block_ref=%d\n",
					   name, lsnbuf, block_id);
				applied++;
				goto record_done;
			}
			die("unsupported BRIN record at target block: info=0x%02X", info);
		}
		else if (rmid == RM_GENERIC_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
			char lsnbuf[64];

			if (info != 0)
				die("unsupported Generic record at target block: info=0x%02X", info);
			apply_generic_delta(reader, page, block_id);
			format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
			printf("applied Generic CHANGE lsn=%s block_ref=%d\n",
				   lsnbuf, block_id);
			applied++;
			goto record_done;
		}
		else if (rmid == RM_SEQ_ID)
		{
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
			char lsnbuf[64];

			if (info != OFFLINE_XLOG_SEQ_LOG)
				die("unsupported Sequence record at target block: info=0x%02X", info);
			apply_sequence_log(reader, page, block_id);
			format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
			printf("applied Sequence LOG lsn=%s block_ref=%d\n",
				   lsnbuf, block_id);
			applied++;
			goto record_done;
		}
		else
		{
			char lsnbuf[64];
			uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

			if (rmid == RM_XLOG_ID && info == XLOG_FPI_FOR_HINT)
				goto record_done;

			format_lsn(lsnbuf, sizeof(lsnbuf), recptr);
			die("cannot replay rmgr=%u info=0x%02X without a full-page image at LSN %s",
				(unsigned int) rmid, XLogRecGetInfo(reader), lsnbuf);
		}

record_done:
		if (recptr == opts.stop_lsn)
			break;
	}

	if (errormsg != NULL)
		die("error while reading WAL: %s", errormsg);

	if (input_has_checksum)
		((PageHeader) page)->pd_checksum = pg_checksum_page(page, opts.blkno);
	write_page(opts.out_path, page);

	printf("done: applied %d records, wrote %s\n", applied, opts.out_path);

	XLogReaderFree(reader);

	return 0;
}
