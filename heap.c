#include "pg_lsn_block_applier_internal.h"

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

void
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

void
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

void
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

void
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

void
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

	PageSetLSN(page, record->EndRecPtr);

	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
		PageClearAllVisible(page);

	log_applied(record, "Heap", "INSERT", "xid=%lu off=%u tuple_len=%u", (unsigned long) xid, xlrec->offnum, newlen);
}

void
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
		PageSetAllVisible(page);
	PageSetLSN(page, record->EndRecPtr);

	log_applied(record, "Heap2", "MULTI_INSERT",
				"xid=%lu tuples=%u flags=0x%02X",
				(unsigned long) XLogRecGetXid(record),
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

void
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

	log_applied(record, "Heap", hot_update ? "HOT_UPDATE" : "UPDATE",
				"xid=%lu old_off=%u new_off=%u target_part=%s%s",
				(unsigned long) XLogRecGetXid(record),
				xlrec->old_offnum, xlrec->new_offnum,
				target_is_old ? "old" : "",
				target_is_new ? "+new" : "");
}

#if PG_VERSION_NUM < 170000
void
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
	if (vacuum)
		vacuum_truncate_line_pointer_array(page);
	else if (nredirected || ndead || nunused)
		repair_page_fragmentation(page);
	PageSetLSN(page, record->EndRecPtr);
	log_applied(record, "Heap2", vacuum ? "VACUUM:" : "PRUNE:",
				"redirected=%d dead=%d unused=%d",
				nredirected, ndead, nunused);
}

void
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

void
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
	{
		if (xlrec->flags & XLHP_CLEANUP_LOCK)
			repair_page_fragmentation(page);
		else
		{
			if (nredirected > 0 || ndead > 0)
				die("PRUNE without cleanup lock changes redirected or dead items");
			vacuum_truncate_line_pointer_array(page);
		}
	}
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

	log_applied(record, "Heap2", prune_op_name(op),
				"flags=0x%04X nredirected=%d ndead=%d nunused=%d",
				xlrec->flags, nredirected, ndead, nunused);
}
#endif


bool
replay_heap_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
	uint8 op = info & XLOG_HEAP_OPMASK;

	if (op == XLOG_HEAP_INSERT)
	{
		apply_heap_insert(reader, page, block_id, blkno);
		return true;
	}

	if (op == XLOG_HEAP_UPDATE || op == XLOG_HEAP_HOT_UPDATE)
	{
		apply_heap_update(reader, page, block_id);
		return true;
	}
	if (op == XLOG_HEAP_DELETE)
	{
		apply_heap_delete(reader, page, blkno);
		log_applied(reader, "Heap", "DELETE", NULL);
		return true;
	}
	if (op == XLOG_HEAP_CONFIRM)
	{
		apply_heap_confirm(reader, page, blkno);
		log_applied(reader, "Heap", "CONFIRM", NULL);
		return true;
	}
	if (op == XLOG_HEAP_LOCK)
	{
		apply_heap_lock(reader, page, blkno, false);
		log_applied(reader, "Heap", "LOCK", NULL);
		return true;
	}
	if (op == XLOG_HEAP_INPLACE)
	{
		apply_heap_inplace(reader, page);
		log_applied(reader, "Heap", "INPLACE", NULL);
		return true;
	}

	die("unsupported Heap record at target block: info=0x%02X", info);
	return false;
}

bool
replay_heap2_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;
	uint8 op = info & XLOG_HEAP_OPMASK;

	if (op == XLOG_HEAP2_MULTI_INSERT)
	{
		apply_heap_multi_insert(reader, page, blkno);
		return true;
	}
	if (op == XLOG_HEAP2_LOCK_UPDATED)
	{
		apply_heap_lock(reader, page, blkno, true);
		log_applied(reader, "Heap2", "LOCK_UPDATED", NULL);
		return true;
	}
	if (op == XLOG_HEAP2_VISIBLE)
	{
		if (block_id != 1)
			die("VISIBLE matched an unexpected heap block reference");
		PageSetAllVisible(page);
		if (input_has_checksum)
			PageSetLSN(page, reader->EndRecPtr);
		log_applied(reader, "Heap2", "VISIBLE", NULL);
		return true;
	}

#if PG_VERSION_NUM >= 170000
	if (op == XLOG_HEAP2_PRUNE_ON_ACCESS ||
		op == XLOG_HEAP2_PRUNE_VACUUM_SCAN ||
		op == XLOG_HEAP2_PRUNE_VACUUM_CLEANUP)
	{
		apply_heap2_prune_pg17(reader, page, block_id);
		return true;
	}
#else
	if (op == XLOG_HEAP2_PRUNE || op == XLOG_HEAP2_VACUUM)
	{
		apply_heap2_prune_legacy(reader, page, block_id,
							 op == XLOG_HEAP2_VACUUM);
		return true;
	}
	if (op == XLOG_HEAP2_FREEZE_PAGE)
	{
		apply_heap2_freeze_legacy(reader, page, block_id);
		log_applied(reader, "Heap2", "FREEZE_PAGE", NULL);
		return true;
	}
#endif

	die("unsupported Heap2 record at target block: info=0x%02X", info);
	return false;
}
