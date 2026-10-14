#include "pg_lsn_block_applier_internal.h"

void
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

void
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

bool
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

void
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
	opaque->hasho_flag = OFFLINE_LH_BITMAP_PAGE;
	opaque->hasho_page_id = OFFLINE_HASHO_PAGE_ID;
	memset(PageGetContents(page), 0xFF, bmsize);
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + bmsize;
}

void
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
	opaque->hasho_flag = OFFLINE_LH_META_PAGE;
	opaque->hasho_page_id = OFFLINE_HASHO_PAGE_ID;
	meta = (OfflineHashMetaPageData *) PageGetContents(page);
	meta->hashm_magic = OFFLINE_HASH_MAGIC;
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

void
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

void
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
		opaque->hasho_flag = OFFLINE_LH_OVERFLOW_PAGE;
		opaque->hasho_page_id = OFFLINE_HASHO_PAGE_ID;
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

bool
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

bool
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
		opaque->hasho_page_id = OFFLINE_HASHO_PAGE_ID;
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

void
apply_hash_update_meta(XLogReaderState *record, Page page, int block_id)
{
	OfflineHashMetaPageData *meta = (OfflineHashMetaPageData *) PageGetContents(page);

	if (block_id != 0 || XLogRecGetDataLen(record) < sizeof(double))
		die("invalid Hash UPDATE_META_PAGE WAL header");
	memcpy(&meta->hashm_ntuples, XLogRecGetData(record), sizeof(double));
	PageSetLSN(page, record->EndRecPtr);
}

void
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

bool
replay_hash_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	(void) blkno;
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

	if (apply_after_block_image(reader, page, block_id))
	{
		return true;
	}
	if (info == XLOG_HASH_INIT_META_PAGE ||
		info == XLOG_HASH_INIT_BITMAP_PAGE)
	{

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
		log_applied(reader, "Hash", name, "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_HASH_ADD_OVFL_PAGE)
	{

		apply_hash_add_overflow(reader, page, block_id);
		log_applied(reader, "Hash", "ADD_OVFL_PAGE", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_HASH_MOVE_PAGE_CONTENTS ||
		info == XLOG_HASH_SQUEEZE_PAGE)
	{
		bool changed;

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
			log_applied(reader, "Hash", name, "block_ref=%d", block_id);
		}
		return changed;
	}
	if (info == XLOG_HASH_INSERT)
	{

		apply_hash_insert(reader, page, block_id);
		log_applied(reader, "Hash", "INSERT", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_HASH_SPLIT_ALLOCATE_PAGE && block_id == 2)
	{

		apply_hash_split_meta(reader, page, block_id);
		log_applied(reader, "Hash", "SPLIT_ALLOCATE_PAGE", "block_ref=2");
		return true;
	}
	if (info == XLOG_HASH_SPLIT_ALLOCATE_PAGE && block_id == 1)
	{
		xl_hash_split_allocate_page *xlrec;
		OfflineHashPageOpaqueData *opaque;

		if (XLogRecGetDataLen(reader) < SizeOfHashSplitAllocPage)
			die("invalid Hash SPLIT_ALLOCATE_PAGE WAL header");
		xlrec = (xl_hash_split_allocate_page *) XLogRecGetData(reader);
		init_index_page_offline(page, sizeof(OfflineHashPageOpaqueData));
		opaque = (OfflineHashPageOpaqueData *) PageGetSpecialPointer(page);
		opaque->hasho_prevblkno = xlrec->new_bucket;
		opaque->hasho_nextblkno = InvalidBlockNumber;
		opaque->hasho_bucket = xlrec->new_bucket;
		opaque->hasho_flag = xlrec->new_bucket_flag;
		opaque->hasho_page_id = OFFLINE_HASHO_PAGE_ID;
		PageSetLSN(page, reader->EndRecPtr);
		log_applied(reader, "Hash", "SPLIT_ALLOCATE_PAGE", "block_ref=1");
		return true;
	}
	if (info == XLOG_HASH_DELETE)
	{
		bool changed = apply_hash_delete(reader, page, block_id);

		if (changed)
		{

			log_applied(reader, "Hash", "DELETE", "block_ref=%d", block_id);
		}
		return changed;
	}
	if (info == XLOG_HASH_SPLIT_CLEANUP)
	{

		apply_hash_split_cleanup(reader, page, block_id);
		log_applied(reader, "Hash", "SPLIT_CLEANUP", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_HASH_UPDATE_META_PAGE ||
		info == XLOG_HASH_VACUUM_ONE_PAGE)
	{

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
		log_applied(reader, "Hash", name, "block_ref=%d", block_id);
		return true;
	}
	die("unsupported Hash record at target block: info=0x%02X", info);
	return false;
}
