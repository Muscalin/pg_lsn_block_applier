/* Compile only: compare the frontend's on-disk copies with backend headers. */
#include "pg_lsn_block_applier_internal.h"

#undef FRONTEND
#include "access/brin_page.h"
#include "access/brin_xlog.h"
#include "access/gist.h"
#include "access/gistxlog.h"
#include "access/hash.h"
#include "access/nbtree.h"
#include "access/spgist_private.h"

StaticAssertDecl(sizeof(OfflineBTPageOpaqueData) == sizeof(BTPageOpaqueData),
				 "B-tree page opaque layout changed");
StaticAssertDecl(sizeof(OfflineBTMetaPageData) == sizeof(BTMetaPageData),
				 "B-tree metapage layout changed");
StaticAssertDecl(sizeof(OfflineHashPageOpaqueData) == sizeof(HashPageOpaqueData),
				 "Hash page opaque layout changed");
StaticAssertDecl(sizeof(OfflineHashMetaPageData) == sizeof(HashMetaPageData),
				 "Hash metapage layout changed");
StaticAssertDecl(sizeof(OfflineHashMetaPrefix) ==
				 offsetof(HashMetaPageData, hashm_ffactor),
				 "Hash metapage prefix changed");
StaticAssertDecl(sizeof(OfflineGistPageOpaqueData) == sizeof(GISTPageOpaqueData),
				 "GiST page opaque layout changed");
StaticAssertDecl(sizeof(OfflineGistPageUpdate) == sizeof(gistxlogPageUpdate),
				 "GiST page update WAL layout changed");
StaticAssertDecl(sizeof(OfflineGistPageSplit) == sizeof(gistxlogPageSplit),
				 "GiST page split WAL layout changed");
StaticAssertDecl(sizeof(OfflineGistPageDelete) == sizeof(gistxlogPageDelete),
				 "GiST page delete WAL layout changed");
StaticAssertDecl(sizeof(OfflineGinInsert) == sizeof(ginxlogInsert),
				 "GIN insert WAL layout changed");
StaticAssertDecl(sizeof(OfflineGinInsertEntry) == sizeof(ginxlogInsertEntry),
				 "GIN entry WAL layout changed");
StaticAssertDecl(sizeof(OfflineSpgPageOpaqueData) == sizeof(SpGistPageOpaqueData),
				 "SP-GiST page opaque layout changed");
StaticAssertDecl(sizeof(OfflineSpgAddLeaf) == sizeof(spgxlogAddLeaf),
				 "SP-GiST add leaf WAL layout changed");
StaticAssertDecl(sizeof(OfflineSpgLeafTupleData) == sizeof(SpGistLeafTupleData),
				 "SP-GiST leaf tuple layout changed");
StaticAssertDecl(sizeof(OfflineSpgInnerTupleData) == sizeof(SpGistInnerTupleData),
				 "SP-GiST inner tuple layout changed");
StaticAssertDecl(sizeof(OfflineSpgDeadTupleData) == sizeof(SpGistDeadTupleData),
				 "SP-GiST dead tuple layout changed");
StaticAssertDecl(sizeof(OfflineBrinMetaPageData) == sizeof(BrinMetaPageData),
				 "BRIN metapage layout changed");
StaticAssertDecl(sizeof(OfflineBrinCreateIndex) == sizeof(xl_brin_createidx),
				 "BRIN create index WAL layout changed");
StaticAssertDecl(sizeof(OfflineBrinInsert) == sizeof(xl_brin_insert),
				 "BRIN insert WAL layout changed");
StaticAssertDecl(sizeof(OfflineBrinUpdate) == sizeof(xl_brin_update),
				 "BRIN update WAL layout changed");
StaticAssertDecl(sizeof(OfflineBrinSamepageUpdate) ==
				 sizeof(xl_brin_samepage_update),
				 "BRIN same-page update WAL layout changed");
StaticAssertDecl(sizeof(OfflineBrinDesummarize) == sizeof(xl_brin_desummarize),
				 "BRIN desummarize WAL layout changed");

StaticAssertDecl(offsetof(OfflineBTPageOpaqueData, btpo_flags) ==
				 offsetof(BTPageOpaqueData, btpo_flags),
				 "B-tree page flags moved");
StaticAssertDecl(offsetof(OfflineHashPageOpaqueData, hasho_flag) ==
				 offsetof(HashPageOpaqueData, hasho_flag),
				 "Hash page flags moved");
StaticAssertDecl(offsetof(OfflineHashMetaPageData, hashm_mapp) ==
				 offsetof(HashMetaPageData, hashm_mapp),
				 "Hash bitmap map moved");
StaticAssertDecl(OFFLINE_LH_OVERFLOW_PAGE == LH_OVERFLOW_PAGE &&
				 OFFLINE_LH_BITMAP_PAGE == LH_BITMAP_PAGE &&
				 OFFLINE_LH_META_PAGE == LH_META_PAGE &&
				 OFFLINE_HASHO_PAGE_ID == HASHO_PAGE_ID &&
				 OFFLINE_HASH_MAGIC == HASH_MAGIC,
				 "Hash page constants changed");
StaticAssertDecl(OFFLINE_SPGIST_LEAF == SPGIST_LEAF &&
				 OFFLINE_SPGIST_NULLS == SPGIST_NULLS &&
				 OFFLINE_SPGIST_PAGE_ID == SPGIST_PAGE_ID,
				 "SP-GiST page constants changed");
