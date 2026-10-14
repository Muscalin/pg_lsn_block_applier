#define FRONTEND 1

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/rmgr.h"
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
			if (!RestoreBlockImage(reader, block_id, page))
				die("could not restore full-page image at target block");
			PageSetLSN(page, reader->EndRecPtr);
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
