#include "pg_lsn_block_applier_internal.h"

void
apply_btree_insert(XLogReaderState *record, Page page, int block_id,
			   const char *name)
{
	xl_btree_insert *xlrec;
	IndexTupleData itup;
	Size mainlen = XLogRecGetDataLen(record);
	Size datalen;
	char *data;


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
	log_applied(record, "Btree", name, "off=%u tuple_len=%zu", (unsigned int) xlrec->offnum, datalen);
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

void
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

void
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

void
apply_btree_clear_incomplete_split(Page page)
{
	OfflineBTPageOpaqueData *opaque = OfflineBTPageGetOpaque(page);

	if (!(opaque->btpo_flags & OFFLINE_BTP_INCOMPLETE_SPLIT))
		die("B-tree child page lacks INCOMPLETE_SPLIT flag");
	opaque->btpo_flags &= ~OFFLINE_BTP_INCOMPLETE_SPLIT;
}

void
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

void
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

void
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

void
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

void
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

void
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


bool
replay_btree_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	(void) blkno;
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

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
			log_applied(reader, "Btree", "CLEAR_INCOMPLETE_SPLIT",
						"block_ref=%d", block_id);
		}
		else if (block_id == 2 && info == XLOG_BTREE_INSERT_META)
		{
			apply_btree_meta(reader, page, block_id);
			log_applied(reader, "Btree", "INSERT_META",
						"block_ref=%d", block_id);
		}
		else
			die("invalid B-tree INSERT target block reference %d", block_id);
		return true;
	}
	if (info == XLOG_BTREE_SPLIT_L || info == XLOG_BTREE_SPLIT_R)
	{
		apply_btree_split(reader, page, block_id,
						  info == XLOG_BTREE_SPLIT_L);
		log_applied(reader, "Btree", info == XLOG_BTREE_SPLIT_L ? "SPLIT_L" : "SPLIT_R", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_BTREE_INSERT_POST || info == XLOG_BTREE_DEDUP)
	{
		if (info == XLOG_BTREE_INSERT_POST)
			apply_btree_insert_post(reader, page, block_id);
		else
			apply_btree_dedup(reader, page, block_id);
		log_applied(reader, "Btree", info == XLOG_BTREE_INSERT_POST ?
					"INSERT_POST" : "DEDUP", NULL);
		return true;
	}
	if (info == XLOG_BTREE_NEWROOT)
	{
		apply_btree_newroot(reader, page, block_id);
		log_applied(reader, "Btree", "NEWROOT", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_BTREE_META_CLEANUP && block_id == 0)
	{
		apply_btree_meta(reader, page, block_id);
		log_applied(reader, "Btree", "META_CLEANUP",
					"block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_BTREE_DELETE || info == XLOG_BTREE_VACUUM)
	{
		apply_btree_delete_vacuum(reader, page, block_id,
						  info == XLOG_BTREE_VACUUM);
		log_applied(reader, "Btree", info == XLOG_BTREE_VACUUM ?
					"VACUUM" : "DELETE", NULL);
		return true;
	}
	if (info == XLOG_BTREE_MARK_PAGE_HALFDEAD)
	{
		apply_btree_mark_halfdead(reader, page, block_id);
		log_applied(reader, "Btree", "MARK_PAGE_HALFDEAD", "block_ref=%d", block_id);
		return true;
	}
	if (info == XLOG_BTREE_UNLINK_PAGE ||
		info == XLOG_BTREE_UNLINK_PAGE_META)
	{
		apply_btree_unlink_page(reader, page, block_id,
						info == XLOG_BTREE_UNLINK_PAGE_META);
		log_applied(reader, "Btree", info == XLOG_BTREE_UNLINK_PAGE_META ?
			   "UNLINK_PAGE_META" : "UNLINK_PAGE", "block_ref=%d", block_id);
		return true;
	}
	die("unsupported B-tree record at target block: info=0x%02X", info);
	return false;
}
