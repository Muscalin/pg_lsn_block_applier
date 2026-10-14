#include "pg_lsn_block_applier_internal.h"

void
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
void
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


bool
replay_generic_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	(void) blkno;
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

	if (info != 0)
		die("unsupported Generic record at target block: info=0x%02X", info);
	apply_generic_delta(reader, page, block_id);
	log_applied(reader, "Generic", "CHANGE", "block_ref=%d", block_id);
	return true;
	return false;
}

bool
replay_seq_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno)
{
	(void) blkno;
	uint8 info = XLogRecGetInfo(reader) & ~XLR_INFO_MASK;

	if (info != OFFLINE_XLOG_SEQ_LOG)
		die("unsupported Sequence record at target block: info=0x%02X", info);
	apply_sequence_log(reader, page, block_id);
	log_applied(reader, "Sequence", "LOG", "block_ref=%d", block_id);
	return true;
	return false;
}
