#include "pg_lsn_block_applier_internal.h"

void
apply_gist_page_update(XLogReaderState *record, Page page, int block_id)
{
	OfflineGistPageUpdate header;
	Size len;
	char *data;
	Size pos = 0;
	uint16 i;
	Size offsets_len;
	OfflineGistPageOpaqueData *opaque;

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
	{
		delete_index_items(page, data, header.ntodelete);
		opaque = (OfflineGistPageOpaqueData *) PageGetSpecialPointer(page);
		if (opaque->flags & OFFLINE_GIST_LEAF)
			opaque->flags |= OFFLINE_GIST_TUPLES_DELETED;
	}
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

void
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
	opaque->flags |= OFFLINE_GIST_TUPLES_DELETED;
	PageSetLSN(page, record->EndRecPtr);
}

void
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

void
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

bool
replay_gist_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

	if (apply_after_block_image(reader, page, block_id))
	{
		return true;
	}
	if (info == OFFLINE_XLOG_GIST_PAGE_UPDATE)
	{

		apply_gist_page_update(reader, page, block_id);
		log_applied(reader, "GiST", "PAGE_UPDATE", "block_ref=%d", block_id);
		return true;
	}
	if (info == OFFLINE_XLOG_GIST_DELETE ||
		info == OFFLINE_XLOG_GIST_PAGE_SPLIT ||
		info == OFFLINE_XLOG_GIST_PAGE_DELETE)
	{

		const char *name;

		if (info == OFFLINE_XLOG_GIST_DELETE)
		{
			apply_gist_delete(reader, page, block_id);
			name = "DELETE";
		}
		else if (info == OFFLINE_XLOG_GIST_PAGE_SPLIT)
		{
			apply_gist_page_split(reader, page, block_id, blkno);
			name = "PAGE_SPLIT";
		}
		else
		{
			apply_gist_page_delete(reader, page, block_id);
			name = "PAGE_DELETE";
		}
		log_applied(reader, "GiST", name, "block_ref=%d", block_id);
		return true;
	}
	die("unsupported GiST record at target block: info=0x%02X", info);
	return false;
}
