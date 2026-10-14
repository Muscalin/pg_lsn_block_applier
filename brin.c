#include "pg_lsn_block_applier_internal.h"

void
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

void
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

void
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

void
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

void
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

void
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

bool
replay_brin_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	(void) blkno;
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
	uint8 op = info & OFFLINE_XLOG_BRIN_OPMASK;
	const char *name = NULL;

	if (op == OFFLINE_XLOG_BRIN_CREATE_INDEX)
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

		log_applied(reader, "BRIN", name, "block_ref=%d", block_id);
		return true;
	}
	die("unsupported BRIN record at target block: info=0x%02X", info);
	return false;
}
