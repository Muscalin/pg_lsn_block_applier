#include "pg_lsn_block_applier_internal.h"

static void
spgist_init_page(Page page, uint16 flags)
{
	OfflineSpgPageOpaqueData *opaque;

	init_index_page_offline(page, sizeof(*opaque));
	opaque = (OfflineSpgPageOpaqueData *) PageGetSpecialPointer(page);
	opaque->flags = flags;
	opaque->page_id = OFFLINE_SPGIST_PAGE_ID;
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
	if (nitems < 0 || nitems >= (int) lengthof(sorted))
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

void
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

void
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
			opaque->flags = OFFLINE_SPGIST_LEAF | (header.storesNulls ? OFFLINE_SPGIST_NULLS : 0);
			opaque->page_id = OFFLINE_SPGIST_PAGE_ID;
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

void
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
	if (header.nMoves >= (int) (BLCKSZ / sizeof(OffsetNumber)) ||
		ninsert >= (int) (BLCKSZ / sizeof(OffsetNumber)) ||
		len - pos < (Size) (header.nMoves + ninsert) * sizeof(OffsetNumber))
		die("invalid SP-GiST MOVE_LEAFS offsets");
	tuple_pos = pos + (Size) (header.nMoves + ninsert) * sizeof(OffsetNumber);
	if (block_id == 1)
	{
		if (header.newPage)
			spgist_init_page(page, OFFLINE_SPGIST_LEAF | (header.storesNulls ? OFFLINE_SPGIST_NULLS : 0));
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

void
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
			spgist_init_page(page, OFFLINE_SPGIST_LEAF | (header.storesNulls ? OFFLINE_SPGIST_NULLS : 0));
		else
			spgist_replace_with_dead(page, data + pos, header.nDelete,
						 header.stateSrc.isBuild ? 3 : 1, 3,
						 inner_block, header.offnumInner,
						 header.stateSrc);
	}
	else if (block_id == 1)
	{
		if (header.initDest)
			spgist_init_page(page, OFFLINE_SPGIST_LEAF | (header.storesNulls ? OFFLINE_SPGIST_NULLS : 0));
	}
	else if (block_id == 2)
	{
		if (header.initInner)
			spgist_init_page(page, header.storesNulls ? OFFLINE_SPGIST_NULLS : 0);
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

void
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

void
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

void
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
			opaque->page_id = OFFLINE_SPGIST_PAGE_ID;
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

void
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


bool
replay_spgist_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	(void) blkno;
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

	if (info == OFFLINE_XLOG_SPGIST_ADD_LEAF)
	{

		apply_spgist_add_leaf(reader, page, block_id);
		log_applied(reader, "SP-GiST", "ADD_LEAF", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_SPGIST_VACUUM_ROOT)
	{

		apply_spgist_vacuum_root(reader, page, block_id);
		log_applied(reader, "SP-GiST", "VACUUM_ROOT", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_SPGIST_SPLIT_TUPLE)
	{

		apply_spgist_split_tuple(reader, page, block_id);
		log_applied(reader, "SP-GiST", "SPLIT_TUPLE", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_SPGIST_MOVE_LEAFS)
	{

		apply_spgist_move_leafs(reader, page, block_id);
		log_applied(reader, "SP-GiST", "MOVE_LEAFS", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_SPGIST_PICKSPLIT)
	{

		apply_spgist_picksplit(reader, page, block_id);
		log_applied(reader, "SP-GiST", "PICKSPLIT", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_SPGIST_VACUUM_REDIRECT)
	{

		apply_spgist_vacuum_redirect(reader, page, block_id);
		log_applied(reader, "SP-GiST", "VACUUM_REDIRECT", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_SPGIST_ADD_NODE)
	{

		apply_spgist_add_node(reader, page, block_id);
		log_applied(reader, "SP-GiST", "ADD_NODE", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_SPGIST_VACUUM_LEAF)
	{

		apply_spgist_vacuum_leaf(reader, page, block_id);
		log_applied(reader, "SP-GiST", "VACUUM_LEAF", "block_ref=%d", block_id);
		return true;
	}
	die("unsupported SP-GiST record at target block: info=0x%02X", info);
	return false;
}
