#include "pg_lsn_block_applier_internal.h"
#include "storage/checksum_impl.h"
#include <dirent.h>

static bool output_json;
static bool record_logged;
static bool suppress_applied_log;

bool
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
	char	   *base_page_path;
	char	   *out_path;

	RelFileLocator rnode;
	BlockNumber blkno;

	XLogRecPtr	start_lsn;
	XLogRecPtr	stop_lsn;

	TimeLineID	timeline;
	bool		has_relation;
	bool		force_checksums;
} ReplayOptions;

bool input_has_checksum;

void
set_prunable(Page page, TransactionId xid)
{
	PageHeader phdr = (PageHeader) page;

	if (TransactionIdIsNormal(xid) &&
		(!TransactionIdIsValid(phdr->pd_prune_xid) ||
		 (int32) (xid - phdr->pd_prune_xid) < 0))
		phdr->pd_prune_xid = xid;
}

void
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
parse_uint32_component(const char **str, int base, uint32 *value)
{
	const char *start = *str;
	size_t		digits;
	char	   *end;
	unsigned long long parsed;

	digits = strspn(start, base == 16 ?
				 "0123456789abcdefABCDEF" : "0123456789");
	if (digits == 0)
		return false;
	errno = 0;
	parsed = strtoull(start, &end, base);
	if (errno == ERANGE || parsed > 0xFFFFFFFFULL || end != start + digits)
		return false;
	*str = end;
	*value = (uint32) parsed;
	return true;
}

static bool
parse_lsn(const char *str, XLogRecPtr *lsn)
{
	uint32		hi;
	uint32		lo;

	if (!parse_uint32_component(&str, 16, &hi) || *str++ != '/' ||
		!parse_uint32_component(&str, 16, &lo) || *str != '\0')
		return false;

	*lsn = ((uint64) hi << 32) | lo;
	return true;
}

static bool
parse_relation(const char *str, RelFileLocator *rnode)
{
	uint32		spc;
	uint32		db;
	uint32		rel;

	if (!parse_uint32_component(&str, 10, &spc) || *str++ != '/' ||
		!parse_uint32_component(&str, 10, &db) || *str++ != '/' ||
		!parse_uint32_component(&str, 10, &rel) || *str != '\0')
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

void
format_lsn(char *buf, size_t buflen, XLogRecPtr lsn)
{
	snprintf(buf, buflen, "%X/%08X",
			 (uint32) (lsn >> 32),
			 (uint32) lsn);
}

static void
print_json_string(const char *value)
{
	const unsigned char *p = (const unsigned char *) value;

	putchar('"');
	for (; *p != '\0'; p++)
	{
		if (*p == '"' || *p == '\\')
			printf("\\%c", *p);
		else if (*p < 0x20)
			printf("\\u%04x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

void
log_applied(XLogReaderState *record, const char *rmgr,
			const char *operation, const char *detail_fmt,...)
{
	char lsnbuf[64];
	char details[1024] = "";
	va_list args;

	if (suppress_applied_log || record_logged)
		return;
	record_logged = true;
	format_lsn(lsnbuf, sizeof(lsnbuf), record->ReadRecPtr);
	if (detail_fmt != NULL && detail_fmt[0] != '\0')
	{
		va_start(args, detail_fmt);
		vsnprintf(details, sizeof(details), detail_fmt, args);
		va_end(args);
	}
	if (output_json)
	{
		char end_lsn[64];

		format_lsn(end_lsn, sizeof(end_lsn), record->EndRecPtr);
		printf("{\"event\":\"applied\",\"rmgr\":");
		print_json_string(rmgr);
		printf(",\"operation\":");
		print_json_string(operation[0] != '\0' ? operation : rmgr);
		printf(",\"lsn\":");
		print_json_string(lsnbuf);
		printf(",\"end_lsn\":");
		print_json_string(end_lsn);
		printf(",\"rmid\":%u,\"info\":%u",
				(unsigned int) XLogRecGetRmid(record),
				(unsigned int) XLogRecGetInfo(record));
		printf(",\"details\":");
		print_json_string(details);
		printf("}\n");
	}
	else
	{
		printf("applied %s", rmgr);
		if (operation[0] != '\0')
			printf(" %s", operation);
		printf(" lsn=%s", lsnbuf);
		if (details[0] != '\0')
			printf(" %s", details);
		putchar('\n');
	}
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

static int
detect_wal_segment_size(const char *directory, TimeLineID timeline,
						XLogRecPtr start_lsn, bool start_from_page)
{
	DIR *dir = opendir(directory);
	struct dirent *entry;
	XLogRecPtr earliest_segment = InvalidXLogRecPtr;
	bool have_segment = false;
	int result = 0;

	if (dir == NULL)
		die("could not open WAL directory \"%s\": %s", directory, strerror(errno));
	while ((entry = readdir(dir)) != NULL)
	{
		TimeLineID file_tli;
		XLogSegNo segno;
		XLogLongPageHeaderData header;
		char path[MAXPGPATH];
		int fd;

		if (!IsXLogFileName(entry->d_name) ||
			sscanf(entry->d_name, "%08X", &file_tli) != 1 ||
			file_tli != timeline)
			continue;
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		fd = open(path, O_RDONLY | PG_BINARY, 0);
		if (fd < 0)
			continue;
		if (read(fd, &header, sizeof(header)) != sizeof(header))
		{
			close(fd);
			continue;
		}
		close(fd);
		if (header.std.xlp_magic != XLOG_PAGE_MAGIC ||
			!(header.std.xlp_info & XLP_LONG_HEADER) ||
			header.std.xlp_tli == 0 ||
			header.xlp_xlog_blcksz != XLOG_BLCKSZ ||
			!IsValidWalSegSize(header.xlp_seg_size))
			continue;
		XLogFromFileName(entry->d_name, &file_tli, &segno,
						 header.xlp_seg_size);
		if (header.std.xlp_pageaddr != segno * (uint64) header.xlp_seg_size)
			continue;
		if (!have_segment || header.std.xlp_pageaddr < earliest_segment)
		{
			earliest_segment = header.std.xlp_pageaddr;
			have_segment = true;
		}
		if (start_lsn >= header.std.xlp_pageaddr &&
			start_lsn < header.std.xlp_pageaddr + header.xlp_seg_size)
		{
			result = header.xlp_seg_size;
			break;
		}
	}
	closedir(dir);
	if (result == 0)
	{
		if (start_from_page && have_segment && start_lsn < earliest_segment)
		{
			char lsnbuf[64];

			format_lsn(lsnbuf, sizeof(lsnbuf), start_lsn);
			die("base page LSN %s is older than available WAL; pass --start (for --save-fullpage files, use the LSN from the file name)",
				lsnbuf);
		}
		die("could not find a valid WAL segment on timeline %u containing start LSN",
			(unsigned int) timeline);
	}
	return result;
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

	(void) reqLen;
	(void) targetRecPtr;
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
		die("could not open base page file \"%s\": %s", path, strerror(errno));

	if (fread(page, BLCKSZ, 1, f) != 1 || fgetc(f) != EOF)
		die("input file \"%s\" must contain exactly one %d-byte page",
			path, BLCKSZ);

	if (fclose(f) != 0)
		die("could not close base page file \"%s\": %s", path, strerror(errno));
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

void
check_page_pointers(Page page)
{
	PageHeader	phdr = (PageHeader) page;
	static const char zero_page[BLCKSZ] = {0};

	if (PageIsNew(page))
	{
		if (memcmp(page, zero_page, BLCKSZ) != 0)
			die("corrupted uninitialized page: page is not zero-filled");
		return;
	}

	if (PageGetPageSize(page) != BLCKSZ ||
		PageGetPageLayoutVersion(page) != PG_PAGE_LAYOUT_VERSION)
		die("invalid page size or layout version: size=%zu version=%u",
			PageGetPageSize(page), PageGetPageLayoutVersion(page));
	if (phdr->pd_special != MAXALIGN(phdr->pd_special))
		die("misaligned page special pointer: special=%u", phdr->pd_special);

	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
	{
		die("corrupted page pointers: lower=%u upper=%u special=%u",
			phdr->pd_lower, phdr->pd_upper, phdr->pd_special);
	}
}

void
init_heap_page_offline(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	memset(page, 0, BLCKSZ);

	phdr->pd_lower = SizeOfPageHeaderData;
	phdr->pd_upper = BLCKSZ;
	phdr->pd_special = BLCKSZ;
	PageSetPageSizeAndVersion(page, BLCKSZ, PG_PAGE_LAYOUT_VERSION);
}

OffsetNumber
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
void
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

void
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

void
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

void
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

void
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

void
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

BlockNumber
btree_block_number(XLogReaderState *record, uint8 id)
{
	BlockNumber blkno;

	if (!get_block_tag(record, id, NULL, NULL, &blkno))
		die("B-tree WAL record is missing block reference %u", id);
	return blkno;
}

#if PG_VERSION_NUM >= 150000
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
#endif

/* VACUUM's second pass does not compact tuple storage.  PostgreSQL also
 * retains one LP_UNUSED entry when every line pointer became unused. */
void
vacuum_truncate_line_pointer_array(Page page)
{
	PageHeader phdr = (PageHeader) page;
	int maxoff = PageGetMaxOffsetNumber(page);
	int last_used = 0;
	int keep;
	int i;
	bool has_free = false;

	if (maxoff < FirstOffsetNumber)
		die("VACUUM cannot truncate a page without line pointers");
	for (i = maxoff; i >= FirstOffsetNumber; i--)
	{
		if (ItemIdIsUsed(PageGetItemId(page, i)))
		{
			last_used = i;
			break;
		}
	}
	keep = Max(last_used, (int) FirstOffsetNumber);
	for (i = FirstOffsetNumber; i <= keep; i++)
	{
		if (!ItemIdIsUsed(PageGetItemId(page, i)))
		{
			has_free = true;
			break;
		}
	}
	phdr->pd_lower = SizeOfPageHeaderData + keep * sizeof(ItemIdData);
	if (has_free)
		PageSetHasFreeLinePointers(page);
	else
		PageClearHasFreeLinePointers(page);
}

void
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

#if PG_VERSION_NUM >= 150000
	truncate_line_pointer_array(page);
#endif
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

/* Some redo routines change special space even after restoring an FPI. */
bool
apply_after_block_image(XLogReaderState *record, Page page, int block_id)
{
	RmgrId rmid = XLogRecGetRmid(record);
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

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
		log_applied(record, "Hash", "SPLIT_ALLOCATE_PAGE",
					"block_ref=%d", block_id);
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
		log_applied(record, "Hash", "SPLIT_COMPLETE",
					"block_ref=%d", block_id);
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
		log_applied(record, "GiST", "CLEAR_FOLLOW_RIGHT",
					"block_ref=%d", block_id);
		return true;
	}
	return false;
}

typedef bool (*ReplayHandler)(XLogReaderState *, Page, int, BlockNumber);

typedef struct ReplayDispatch
{
	RmgrId rmid;
	ReplayHandler handler;
} ReplayDispatch;

static const ReplayDispatch replay_handlers[] =
{
	{RM_HEAP_ID, replay_heap_record},
	{RM_HEAP2_ID, replay_heap2_record},
	{RM_BTREE_ID, replay_btree_record},
	{RM_HASH_ID, replay_hash_record},
	{RM_GIST_ID, replay_gist_record},
	{RM_GIN_ID, replay_gin_record},
	{RM_SPGIST_ID, replay_spgist_record},
	{RM_BRIN_ID, replay_brin_record},
	{RM_GENERIC_ID, replay_generic_record},
	{RM_SEQ_ID, replay_seq_record},
};

static bool
dispatch_record(XLogReaderState *reader, Page page, int block_id,
				BlockNumber blkno)
{
	RmgrId rmid = XLogRecGetRmid(reader);
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
	Size i;
	char lsnbuf[64];

	for (i = 0; i < lengthof(replay_handlers); i++)
		if (replay_handlers[i].rmid == rmid)
			return replay_handlers[i].handler(reader, page, block_id, blkno);

	if (rmid == RM_XLOG_ID && info == XLOG_FPI_FOR_HINT)
		return false;
	format_lsn(lsnbuf, sizeof(lsnbuf), reader->ReadRecPtr);
	die("cannot replay rmgr=%u info=0x%02X without a full-page image at LSN %s",
		(unsigned int) rmid, XLogRecGetInfo(reader), lsnbuf);
	return false;
}

static void
usage(const char *progname, FILE *stream)
{
	fprintf(stream,
			"Usage:\n"
			"  %s --waldir=DIR --base-page=FILE --out=FILE \\\n"
			"     --rel=T/D/R --block=N --stop=LSN \\\n"
			"     [--start=LSN] [--tli=N] [--data-checksums] [--json]\n\n"
			"  --input=FILE is an alias for --base-page. Without --start, replay\n"
			"  begins at the LSN stored in the base page. WAL segment size is\n"
			"  read from the segment header. For pg_waldump --save-fullpage files,\n"
			"  use the LSN in the file name as --start.\n\n"
			"Example:\n"
			"  %s \\\n"
			"    --waldir=/path/to/wals \\\n"
			"    --base-page=/path/to/fpi/00000001-00002A9C-51FB7280.1663.24605.2072200.847224_main \\\n"
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
	int			segsize;
	bool		start_from_page;

	static struct option long_options[] =
	{
		{"waldir",	required_argument, NULL, 1},
		{"base-page", required_argument, NULL, 2},
		{"input", required_argument, NULL, 2},
		{"out",	required_argument, NULL, 3},
		{"rel",	required_argument, NULL, 4},
		{"block",	required_argument, NULL, 5},
		{"start",	required_argument, NULL, 6},
		{"stop",	required_argument, NULL, 7},
		{"tli",	required_argument, NULL, 8},
		{"data-checksums", no_argument, NULL, 10},
		{"json", no_argument, NULL, 11},
		{"help", no_argument, NULL, 12},
		{"version", no_argument, NULL, 13},
		{NULL, 0, NULL, 0}
	};

	memset(&opts, 0, sizeof(opts));

	opts.timeline = 1;
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
				opts.base_page_path = optarg;
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
			{
				const char *value = optarg;

				if (!parse_uint32_component(&value, 10, &opts.blkno) ||
					*value != '\0' || opts.blkno == InvalidBlockNumber)
					die("invalid --block value: %s", optarg);
				break;
			}
			case 6:
				if (!parse_lsn(optarg, &opts.start_lsn))
					die("invalid --start LSN: %s", optarg);
				break;
			case 7:
				if (!parse_lsn(optarg, &opts.stop_lsn))
					die("invalid --stop LSN: %s", optarg);
				break;
			case 8:
			{
				const char *value = optarg;

				if (!parse_uint32_component(&value, 10, &opts.timeline) ||
					*value != '\0')
					die("invalid --tli value: %s", optarg);
				break;
			}
			case 10:
				opts.force_checksums = true;
				break;
			case 11:
				output_json = true;
				break;
			case 12:
				usage(argv[0], stdout);
				return EXIT_SUCCESS;
			case 13:
				printf("pg_lsn_block_applier 0.1.1 (PostgreSQL %s)\n", PG_VERSION);
				return EXIT_SUCCESS;
			default:
				usage(argv[0], stderr);
				exit(EXIT_FAILURE);
		}
	}

	if (opts.waldir == NULL ||
		opts.base_page_path == NULL ||
		opts.out_path == NULL ||
		!opts.has_relation ||
		XLogRecPtrIsInvalid(opts.stop_lsn) ||
		opts.blkno == InvalidBlockNumber)
	{
		usage(argv[0], stderr);
		exit(EXIT_FAILURE);
	}

	if (opts.timeline == 0)
		die("--tli must be greater than zero");

	read_input_page(opts.base_page_path, page);
	check_page_pointers(page);
	start_from_page = XLogRecPtrIsInvalid(opts.start_lsn);
	if (start_from_page)
	{
		if (PageIsNew(page) || XLogRecPtrIsInvalid(PageGetLSN(page)))
			die("uninitialized base page has no LSN; specify --start");
		opts.start_lsn = PageGetLSN(page);
	}
	else if (!PageIsNew(page) && PageGetLSN(page) < opts.start_lsn)
	{
		char page_lsn[64];
		char start_lsn[64];

		format_lsn(page_lsn, sizeof(page_lsn), PageGetLSN(page));
		format_lsn(start_lsn, sizeof(start_lsn), opts.start_lsn);
		fprintf(stderr, "warning: base page LSN %s precedes --start %s; intervening WAL records may be skipped\n",
				page_lsn, start_lsn);
	}
	if (opts.stop_lsn < opts.start_lsn)
		die("--stop must be >= --start");
	segsize = detect_wal_segment_size(opts.waldir, opts.timeline,
								  opts.start_lsn, start_from_page);
	input_has_checksum = opts.force_checksums ||
		(((PageHeader) page)->pd_checksum != 0 &&
		 ((PageHeader) page)->pd_checksum == pg_checksum_page(page, opts.blkno));

	private.timeline = opts.timeline;

	memset(&routine, 0, sizeof(routine));
	routine.page_read = ReplayReadPage;
	routine.segment_open = ReplayOpenSegment;
	routine.segment_close = ReplayCloseSegment;

	reader = XLogReaderAllocate(segsize, opts.waldir, &routine, &private);
	if (reader == NULL)
		die("XLogReaderAllocate failed");

	first_record = XLogFindNextRecord(reader, opts.start_lsn);
	if (XLogRecPtrIsInvalid(first_record))
		die("could not find first WAL record at or after start LSN");

	XLogBeginRead(reader, first_record);

	while ((record = XLogReadRecord(reader, &errormsg)) != NULL)
	{
		XLogRecPtr	recptr = reader->ReadRecPtr;
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
		record_logged = false;

		/* A usable FPI supersedes the record's incremental change. */
		if (XLogRecHasBlockImage(reader, block_id) &&
			XLogRecBlockImageApply(reader, block_id))
		{
			if (!RestoreBlockImage(reader, block_id, page))
				die("could not restore full-page image at target block");
			suppress_applied_log = true;
			apply_after_block_image(reader, page, block_id);
			suppress_applied_log = false;
			PageSetLSN(page, reader->EndRecPtr);
			log_applied(reader, "FPI", "", "rmgr=%u block=%u",
					(unsigned int) XLogRecGetRmid(reader),
					(unsigned int) opts.blkno);
			applied++;
			goto record_done;
		}


		if (dispatch_record(reader, page, block_id, opts.blkno))
		{
			if (!record_logged)
				log_applied(reader, "WAL", "APPLY",
						"rmgr=%u info=0x%02X block_ref=%d",
						(unsigned int) XLogRecGetRmid(reader),
						XLogRecGetInfo(reader), block_id);
			applied++;
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

	if (output_json)
	{
		printf("{\"event\":\"done\",\"applied\":%d,\"output\":", applied);
		print_json_string(opts.out_path);
		printf("}\n");
	}
	else
		printf("done: applied %d records, wrote %s\n", applied, opts.out_path);

	XLogReaderFree(reader);

	return 0;
}
