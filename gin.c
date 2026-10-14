#include "pg_lsn_block_applier_internal.h"

static int
compare_uint64_values(const void *left, const void *right)
{
	uint64 a = *(const uint64 *) left;
	uint64 b = *(const uint64 *) right;

	return (a > b) - (a < b);
}

void
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

		if (count < 0 || count > (int) lengthof(values))
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
			if (count >= (int) lengthof(values))
					die("too many GIN posting items");
				previous += delta;
				values[count++] = previous;
			}
			for (j = 0; j < nitems; j++)
			{
				ItemPointerData item;

				if (count >= (int) lengthof(values))
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

void
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
		char *record_data = XLogRecGetData(record);
		BlockIdData id;

		if (XLogRecGetDataLen(record) < sizeof(header) + 2 * sizeof(id))
			die("invalid GIN internal INSERT child payload");
		memcpy(&id, record_data + sizeof(header) + sizeof(id), sizeof(id));
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

void
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

void
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

void
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

void
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

void
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


bool
replay_gin_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	(void) blkno;
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

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
		log_applied(reader, "GIN", name, "block_ref=%d", block_id);
		return true;
	}
	die("unsupported GIN record at target block: info=0x%02X", info);
	return false;
}
