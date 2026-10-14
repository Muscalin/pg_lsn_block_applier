#ifndef PG_LSN_BLOCK_APPLIER_INTERNAL_H
#define PG_LSN_BLOCK_APPLIER_INTERNAL_H

#define FRONTEND 1

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/ginxlog.h"
#include "access/itup.h"
#include "access/hash_xlog.h"
#include "access/nbtxlog.h"
#include "access/rmgr.h"
#include "access/spgxlog.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "common/relpath.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "getopt_long.h"
#include "storage/bufpage.h"
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

/* The access method headers pull backend-only buffer and lock APIs into a
 * frontend program.  Keep only the on-disk fields used by these redo paths. */
typedef struct OfflineBTPageOpaqueData
{
	BlockNumber btpo_prev;
	BlockNumber btpo_next;
	uint32 btpo_level;
	uint16 btpo_flags;
	uint16 btpo_cycleid;
} OfflineBTPageOpaqueData;

#define OfflineBTPageGetOpaque(page) \
	((OfflineBTPageOpaqueData *) PageGetSpecialPointer(page))
#define OFFLINE_BTP_LEAF 0x0001
#define OFFLINE_BTP_ROOT 0x0002
#define OFFLINE_BTP_DELETED 0x0004
#define OFFLINE_BTP_META 0x0008
#define OFFLINE_BTP_HALF_DEAD 0x0010
#define OFFLINE_BTP_HAS_GARBAGE 0x0040
#define OFFLINE_BTP_INCOMPLETE_SPLIT 0x0080
#define OFFLINE_BTP_HAS_FULLXID 0x0100
#define OFFLINE_BTREE_MAGIC 0x053162
#define OFFLINE_P_NONE 0

typedef struct OfflineBTMetaPageData
{
	uint32 btm_magic;
	uint32 btm_version;
	BlockNumber btm_root;
	uint32 btm_level;
	BlockNumber btm_fastroot;
	uint32 btm_fastlevel;
	uint32 btm_last_cleanup_num_delpages;
	float8 btm_last_cleanup_num_heap_tuples;
	bool btm_allequalimage;
} OfflineBTMetaPageData;

typedef struct OfflineHashMetaPrefix
{
	uint32 hashm_magic;
	uint32 hashm_version;
	double hashm_ntuples;
} OfflineHashMetaPrefix;

#define OFFLINE_HASH_MAX_SPLITPOINTS 98
typedef struct OfflineHashMetaPageData
{
	uint32 hashm_magic;
	uint32 hashm_version;
	double hashm_ntuples;
	uint16 hashm_ffactor;
	uint16 hashm_bsize;
	uint16 hashm_bmsize;
	uint16 hashm_bmshift;
	uint32 hashm_maxbucket;
	uint32 hashm_highmask;
	uint32 hashm_lowmask;
	uint32 hashm_ovflpoint;
	uint32 hashm_firstfree;
	uint32 hashm_nmaps;
	RegProcedure hashm_procid;
	uint32 hashm_spares[OFFLINE_HASH_MAX_SPLITPOINTS];
	BlockNumber hashm_mapp[1024];
} OfflineHashMetaPageData;

typedef struct OfflineHashPageOpaqueData
{
	BlockNumber hasho_prevblkno;
	BlockNumber hasho_nextblkno;
	uint32 hasho_bucket;
	uint16 hasho_flag;
	uint16 hasho_page_id;
} OfflineHashPageOpaqueData;

typedef struct OfflineGistPageOpaqueData
{
	PageXLogRecPtr nsn;
	BlockNumber rightlink;
	uint16 flags;
	uint16 gist_page_id;
} OfflineGistPageOpaqueData;

typedef struct OfflineGistPageUpdate
{
	uint16 ntodelete;
	uint16 ntoinsert;
} OfflineGistPageUpdate;

typedef struct OfflineGinInsert
{
	uint16 flags;
} OfflineGinInsert;

typedef struct OfflineGinInsertEntry
{
	OffsetNumber offset;
	bool isDelete;
	IndexTupleData tuple;
} OfflineGinInsertEntry;

typedef struct OfflineSpgAddLeaf
{
	bool newPage;
	bool storesNulls;
	OffsetNumber offnumLeaf;
	OffsetNumber offnumHeadLeaf;
	OffsetNumber offnumParent;
	uint16 nodeI;
} OfflineSpgAddLeaf;

typedef struct OfflineSpgLeafTupleData
{
	unsigned int tupstate:2, size:30;
	uint16 t_info;
	ItemPointerData heapPtr;
} OfflineSpgLeafTupleData;

typedef struct OfflineSpgPageOpaqueData
{
	uint16 flags;
	uint16 nRedirection;
	uint16 nPlaceholder;
	uint16 page_id;
} OfflineSpgPageOpaqueData;

typedef struct OfflineSpgInnerTupleData
{
	unsigned int tupstate:2, allTheSame:1, nNodes:13, prefixSize:16;
	uint16 size;
} OfflineSpgInnerTupleData;

typedef struct OfflineSpgDeadTupleData
{
	unsigned int tupstate:2, size:30;
	uint16 t_info;
	ItemPointerData pointer;
	TransactionId xid;
} OfflineSpgDeadTupleData;

#if PG_VERSION_NUM >= 170000
#define OFFLINE_SPG_REDIRECT_XID(state) ((state).redirectXid)
#else
#define OFFLINE_SPG_REDIRECT_XID(state) ((state).myXid)
#endif

#define OFFLINE_XLOG_GIST_PAGE_UPDATE 0x00
#define OFFLINE_XLOG_GIST_DELETE 0x10
#define OFFLINE_XLOG_GIST_PAGE_SPLIT 0x30
#define OFFLINE_XLOG_GIST_PAGE_DELETE 0x60
#define OFFLINE_GIST_LEAF 0x01
#define OFFLINE_GIST_DELETED 0x02
#define OFFLINE_GIST_FOLLOW_RIGHT 0x08
#define OFFLINE_GIST_HAS_GARBAGE 0x10
#define OFFLINE_GIST_PAGE_ID 0xFF81
#define OFFLINE_HASH_NEEDS_SPLIT_CLEANUP 0x40
#define OFFLINE_HASH_HAS_DEAD_TUPLES 0x80
#define OFFLINE_LH_OVERFLOW_PAGE 0x01
#define OFFLINE_LH_BITMAP_PAGE 0x04
#define OFFLINE_LH_META_PAGE 0x08
#define OFFLINE_HASHO_PAGE_ID 0xFF80
#define OFFLINE_HASH_MAGIC 0x6440640
#define OFFLINE_XLOG_GIN_INSERT 0x20
#define OFFLINE_GIN_INSERT_ISDATA 0x01
#define OFFLINE_GIN_INSERT_ISLEAF 0x02
#define OFFLINE_XLOG_SPGIST_ADD_LEAF 0x10
#define OFFLINE_SPGIST_LEAF 0x04
#define OFFLINE_SPGIST_NULLS 0x08
#define OFFLINE_SPGIST_PAGE_ID 0xFF82
#define OFFLINE_XLOG_BRIN_CREATE_INDEX 0x00
#define OFFLINE_XLOG_BRIN_INSERT 0x10
#define OFFLINE_XLOG_BRIN_UPDATE 0x20
#define OFFLINE_XLOG_BRIN_SAMEPAGE_UPDATE 0x30
#define OFFLINE_XLOG_BRIN_REVMAP_EXTEND 0x40
#define OFFLINE_XLOG_BRIN_DESUMMARIZE 0x50
#define OFFLINE_XLOG_BRIN_OPMASK 0x70
#define OFFLINE_XLOG_BRIN_INIT_PAGE 0x80
#define OFFLINE_BRIN_META_MAGIC 0xA8109CFA
#define OFFLINE_BRIN_PAGETYPE_META 0xF091
#define OFFLINE_BRIN_PAGETYPE_REVMAP 0xF092
#define OFFLINE_BRIN_PAGETYPE_REGULAR 0xF093
#define OFFLINE_XLOG_SEQ_LOG 0x00
#define OFFLINE_SEQ_MAGIC 0x1717

typedef struct OfflineBrinInsert
{
	BlockNumber heapBlk;
	BlockNumber pagesPerRange;
	OffsetNumber offnum;
} OfflineBrinInsert;

typedef struct OfflineBrinSamepageUpdate
{
	OffsetNumber offnum;
} OfflineBrinSamepageUpdate;

typedef struct OfflineBrinUpdate
{
	OffsetNumber oldOffnum;
	OfflineBrinInsert insert;
} OfflineBrinUpdate;

typedef struct OfflineBrinMetaPageData
{
	uint32 brinMagic;
	uint32 brinVersion;
	BlockNumber pagesPerRange;
	BlockNumber lastRevmapPage;
} OfflineBrinMetaPageData;

typedef struct OfflineBrinCreateIndex
{
	BlockNumber pagesPerRange;
	uint16 version;
} OfflineBrinCreateIndex;

typedef struct OfflineBrinDesummarize
{
	BlockNumber pagesPerRange;
	BlockNumber heapBlk;
	OffsetNumber regOffset;
} OfflineBrinDesummarize;

typedef struct OfflineGistPageSplit
{
	BlockNumber origrlink;
	XLogRecPtr orignsn;
	bool origleaf;
	uint16 npage;
	bool markfollowright;
} OfflineGistPageSplit;

typedef struct OfflineGistPageDelete
{
	FullTransactionId deleteXid;
	OffsetNumber downlinkOffset;
} OfflineGistPageDelete;


void die(const char *fmt,...) pg_attribute_printf(1, 2);
void log_applied(XLogReaderState *record, const char *rmgr,
				 const char *operation, const char *detail_fmt,...)
	pg_attribute_printf(4, 5);
extern bool input_has_checksum;
bool
get_block_tag(XLogReaderState *record, uint8 block_id,
			  RelFileLocator *rnode, ForkNumber *forknum, BlockNumber *blkno);
void
set_prunable(Page page, TransactionId xid);
void
format_lsn(char *buf, size_t buflen, XLogRecPtr lsn);
void
check_page_pointers(Page page);
void
init_heap_page_offline(Page page);
OffsetNumber
add_heap_item(Page page, Item item, Size size, OffsetNumber offnum);
void
add_btree_item(Page page, Item item, Size size, OffsetNumber offnum);
void
overwrite_index_item(Page page, OffsetNumber offnum, Item item, Size size);
void
delete_index_items(Page page, const char *offsets, int ndelete);
void
delete_index_item_no_compact(Page page, OffsetNumber offnum);
void
init_index_page_offline(Page page, Size special_size);
void
restore_btree_items(Page page, const char *data, Size datalen);
BlockNumber
btree_block_number(XLogReaderState *record, uint8 id);
void
apply_btree_insert(XLogReaderState *record, Page page, int block_id,
			   const char *name);
void
apply_btree_insert_post(XLogReaderState *record, Page page, int block_id);
void
apply_btree_dedup(XLogReaderState *record, Page page, int block_id);
void
apply_btree_clear_incomplete_split(Page page);
void
apply_btree_meta(XLogReaderState *record, Page page, int block_id);
void
apply_btree_split(XLogReaderState *record, Page page, int block_id,
				  bool newitemonleft);
void
apply_btree_newroot(XLogReaderState *record, Page page, int block_id);
void
apply_btree_mark_halfdead(XLogReaderState *record, Page page, int block_id);
void
apply_btree_unlink_page(XLogReaderState *record, Page page,
						 int block_id, bool with_meta);
void
apply_btree_delete_vacuum(XLogReaderState *record, Page page,
						  int block_id, bool vacuum);
void
apply_hash_insert(XLogReaderState *record, Page page, int block_id);
void
apply_hash_split_meta(XLogReaderState *record, Page page, int block_id);
bool
apply_hash_delete(XLogReaderState *record, Page page, int block_id);
void
apply_hash_split_cleanup(XLogReaderState *record, Page page, int block_id);
void
apply_hash_init_meta(XLogReaderState *record, Page page, int block_id);
void
apply_hash_init_bitmap(XLogReaderState *record, Page page, int block_id);
void
apply_hash_add_overflow(XLogReaderState *record, Page page, int block_id);
bool
apply_hash_move_contents(XLogReaderState *record, Page page, int block_id);
bool
apply_hash_squeeze_page(XLogReaderState *record, Page page, int block_id);
void
apply_hash_update_meta(XLogReaderState *record, Page page, int block_id);
void
apply_hash_vacuum_one_page(XLogReaderState *record, Page page, int block_id);
bool
apply_after_block_image(XLogReaderState *record, Page page, int block_id);
void
apply_gist_page_update(XLogReaderState *record, Page page, int block_id);
void
apply_gist_delete(XLogReaderState *record, Page page, int block_id);
void
apply_gist_page_split(XLogReaderState *record, Page page, int block_id,
					  BlockNumber target_blkno);
void
apply_gist_page_delete(XLogReaderState *record, Page page, int block_id);
void
gin_recompress_page(Page page, const char *wal, Size wal_len);
void
apply_gin_insert(XLogReaderState *record, Page page, int block_id);
void
apply_gin_create_ptree(XLogReaderState *record, Page page, int block_id);
void
apply_gin_insert_listpage(XLogReaderState *record, Page page, int block_id);
void
apply_gin_update_meta(XLogReaderState *record, Page page, int block_id);
void
apply_gin_delete_listpage(XLogReaderState *record, Page page, int block_id);
void
apply_gin_delete_page(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_vacuum_root(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_add_leaf(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_move_leafs(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_picksplit(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_add_node(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_vacuum_leaf(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_split_tuple(XLogReaderState *record, Page page, int block_id);
void
apply_spgist_vacuum_redirect(XLogReaderState *record, Page page, int block_id);
void
apply_brin_samepage_update(XLogReaderState *record, Page page, int block_id);
void
apply_brin_insert(XLogReaderState *record, Page page, int block_id);
void
apply_brin_update(XLogReaderState *record, Page page, int block_id);
void
apply_brin_create_index(XLogReaderState *record, Page page, int block_id);
void
apply_brin_revmap_extend(XLogReaderState *record, Page page, int block_id);
void
apply_brin_desummarize(XLogReaderState *record, Page page, int block_id);
void
apply_generic_delta(XLogReaderState *record, Page page, int block_id);
void
apply_sequence_log(XLogReaderState *record, Page page, int block_id);
void
vacuum_truncate_line_pointer_array(Page page);
void
repair_page_fragmentation(Page page);
void
apply_heap_delete(XLogReaderState *record, Page page, BlockNumber blkno);
void
apply_heap_confirm(XLogReaderState *record, Page page, BlockNumber blkno);
void
apply_heap_lock(XLogReaderState *record, Page page, BlockNumber blkno,
				bool updated);
void
apply_heap_inplace(XLogReaderState *record, Page page);
void
apply_heap_insert(XLogReaderState *record,
				  Page page,
				  int block_id,
				  BlockNumber blkno);
void
apply_heap_multi_insert(XLogReaderState *record, Page page, BlockNumber blkno);
void
apply_heap_update(XLogReaderState *record,
				  Page page,
				  int block_id);
void
apply_heap2_prune_legacy(XLogReaderState *record, Page page, int block_id,
						 bool vacuum);
void
apply_heap2_freeze_legacy(XLogReaderState *record, Page page, int block_id);
void
apply_heap2_prune_pg17(XLogReaderState *record,
					   Page page,
					   int block_id);

bool replay_heap_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_heap2_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_btree_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_hash_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_gist_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_gin_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_spgist_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_brin_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_generic_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);
bool replay_seq_record(XLogReaderState *reader, Page page, int block_id, BlockNumber blkno);

#endif /* PG_LSN_BLOCK_APPLIER_INTERNAL_H */
