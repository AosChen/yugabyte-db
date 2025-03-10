/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  This file contains routines to support indexes defined on system
 *	  catalogs.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/indexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "executor/executor.h"
#include "utils/syscache.h"
#include "utils/rel.h"

#include "pg_yb_utils.h"
#include "access/yb_scan.h"
#include "executor/ybcModifyTable.h"
#include "miscadmin.h"

/*
 * CatalogOpenIndexes - open the indexes on a system catalog.
 *
 * When inserting or updating tuples in a system catalog, call this
 * to prepare to update the indexes for the catalog.
 *
 * In the current implementation, we share code for opening/closing the
 * indexes with execUtils.c.  But we do not use ExecInsertIndexTuples,
 * because we don't want to create an EState.  This implies that we
 * do not support partial or expressional indexes on system catalogs,
 * nor can we support generalized exclusion constraints.
 * This could be fixed with localized changes here if we wanted to pay
 * the extra overhead of building an EState.
 */
CatalogIndexState
CatalogOpenIndexes(Relation heapRel)
{
	ResultRelInfo *resultRelInfo;

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;	/* dummy */
	resultRelInfo->ri_RelationDesc = heapRel;
	resultRelInfo->ri_TrigDesc = NULL;	/* we don't fire triggers */

	ExecOpenIndices(resultRelInfo, false);

	return resultRelInfo;
}

/*
 * CatalogCloseIndexes - clean up resources allocated by CatalogOpenIndexes
 */
void
CatalogCloseIndexes(CatalogIndexState indstate)
{
	ExecCloseIndices(indstate);
	pfree(indstate);
}

/*
 * CatalogIndexInsert - insert index entries for one catalog tuple
 *
 * This should be called for each inserted or updated catalog tuple.
 *
 * This is effectively a cut-down version of ExecInsertIndexTuples.
 *
 * if yb_shared_insert is specified, this insert will be done in every
 * database (including template0 and template1). This is needed when
 * creating shared relations.
 * This flag should not be used during initdb bootstrap.
 */
static void
CatalogIndexInsert(CatalogIndexState indstate, HeapTuple heapTuple, bool yb_shared_insert)
{
	int			i;
	int			numIndexes;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleTableSlot *slot;
	IndexInfo **indexInfoArray;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/* HOT update does not require index inserts */
	if (HeapTupleIsHeapOnly(heapTuple))
		return;

	/*
	 * Get information from the state structure.  Fall out if nothing to do.
	 */
	numIndexes = indstate->ri_NumIndices;
	if (numIndexes == 0)
		return;
	relationDescs = indstate->ri_IndexRelationDescs;
	indexInfoArray = indstate->ri_IndexRelationInfo;
	heapRelation = indstate->ri_RelationDesc;

	/* Need a slot to hold the tuple being examined */
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation));
	ExecStoreHeapTuple(heapTuple, slot, false);

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndexes; i++)
	{
		/*
		 * No need to update YugaByte primary key which is intrinic part of
		 * the base table.
		 */
		if (IsYugaByteEnabled() && relationDescs[i]->rd_index->indisprimary)
			continue;

		IndexInfo  *indexInfo;

		indexInfo = indexInfoArray[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * Expressional and partial indexes on system catalogs are not
		 * supported, nor exclusion constraints, nor deferred uniqueness
		 */
		Assert(indexInfo->ii_Expressions == NIL);
		Assert(indexInfo->ii_Predicate == NIL);
		Assert(indexInfo->ii_ExclusionOps == NULL);
		Assert(relationDescs[i]->rd_index->indimmediate);
		Assert(indexInfo->ii_NumIndexKeyAttrs != 0);

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   NULL,	/* no expression eval to do */
					   values,
					   isnull);

		/*
		 * The index AM does the rest.
		 */
		index_insert(relationDescs[i],	/* index relation */
					 values,	/* array of index Datums */
					 isnull,	/* is-null flags */
					 &(heapTuple->t_self),	/* tid of heap tuple */
					 heapTuple,	/* heap tuple */
					 heapRelation,
					 relationDescs[i]->rd_index->indisunique ?
					 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
					 indexInfo,
					 yb_shared_insert);
	}

	ExecDropSingleTupleTableSlot(slot);
}

/*
 * CatalogIndexDelete - delete index entries for one catalog tuple
 *
 * This should be called for each updated or deleted catalog tuple.
 *
 * This is effectively a cut-down version of ExecDeleteIndexTuples.
 */
static void
CatalogIndexDelete(CatalogIndexState indstate, HeapTuple heapTuple)
{
	int			i;
	int			numIndexes;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleTableSlot *slot;
	IndexInfo **indexInfoArray;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/*
	 * Get information from the state structure.  Fall out if nothing to do.
	 */
	numIndexes = indstate->ri_NumIndices;
	if (numIndexes == 0)
		return;
	relationDescs = indstate->ri_IndexRelationDescs;
	indexInfoArray = indstate->ri_IndexRelationInfo;
	heapRelation = indstate->ri_RelationDesc;

	/* Need a slot to hold the tuple being examined */
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation));
	ExecStoreHeapTuple(heapTuple, slot, false);

	/*
	 * for each index, form and delete the index tuple
	 */
	for (i = 0; i < numIndexes; i++)
	{
		/*
		 * No need to update YugaByte primary key which is intrinic part of
		 * the base table.
		 */
		if (IsYugaByteEnabled() && relationDescs[i]->rd_index->indisprimary)
			continue;

		IndexInfo  *indexInfo;

		indexInfo = indexInfoArray[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * Expressional and partial indexes on system catalogs are not
		 * supported, nor exclusion constraints, nor deferred uniqueness
		 */
		Assert(indexInfo->ii_Expressions == NIL);
		Assert(indexInfo->ii_Predicate == NIL);
		Assert(indexInfo->ii_ExclusionOps == NULL);
		Assert(relationDescs[i]->rd_index->indimmediate);

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   NULL,	/* no expression eval to do */
					   values,
					   isnull);

		/*
		 * The index AM does the rest.
		 */
		index_delete(relationDescs[i],	/* index relation */
					 values,	/* array of index Datums */
					 isnull,	/* is-null flags */
					 heapTuple->t_ybctid,	/* heap tuple */
					 heapRelation,
					 indexInfo);
	}

	ExecDropSingleTupleTableSlot(slot);
}

/*
 * CatalogTupleInsert - do heap and indexing work for a new catalog tuple
 *
 * Insert the tuple data in "tup" into the specified catalog relation.
 * The Oid of the inserted tuple is returned.
 *
 * This is a convenience routine for the common case of inserting a single
 * tuple in a system catalog; it inserts a new heap tuple, keeping indexes
 * current.  Avoid using it for multiple tuples, since opening the indexes
 * and building the index info structures is moderately expensive.
 * (Use CatalogTupleInsertWithInfo in such cases.)
 */
Oid
CatalogTupleInsert(Relation heapRel, HeapTuple tup)
{
	return YBCatalogTupleInsert(heapRel, tup, false);
}

/*
 * Enhanced version of CatalogTupleInsert.
 *
 * if yb_shared_insert is specified, this insert will be done in every
 * database (including template0 and template1). This is needed when
 * creating shared relations.
 * This flag should not be used during initdb bootstrap.
 */
Oid
YBCatalogTupleInsert(Relation heapRel, HeapTuple tup, bool yb_shared_insert)
{
	CatalogIndexState indstate;
	Oid			oid;

	if (IsYugaByteEnabled())
	{
		/* Keep ybctid consistent across all databases. */
		Datum ybctid = 0;

		if (yb_shared_insert)
		{
			if (!IsYsqlUpgrade)
				elog(ERROR, "shared insert cannot be done outside of YSQL upgrade");

			YB_FOR_EACH_DB(pg_db_tuple)
			{
				Oid dboid = HeapTupleGetOid(pg_db_tuple);
				/*
				 * Since this is a catalog table, we assume it exists in all databases.
				 * YB doesn't use PG locks so it's okay not to take them.
				 */
				if (dboid == YBCGetDatabaseOid(heapRel))
					continue; /* Will be done after the loop. */
				YBCExecuteInsertForDb(dboid,
									  heapRel,
									  RelationGetDescr(heapRel),
									  tup,
									  ONCONFLICT_NONE,
									  &ybctid);
			}
			YB_FOR_EACH_DB_END;
		}
		oid = YBCExecuteInsertForDb(YBCGetDatabaseOid(heapRel),
									heapRel,
									RelationGetDescr(heapRel),
									tup,
									ONCONFLICT_NONE,
									&ybctid);
		/* Update the local cache automatically */
		YbSetSysCacheTuple(heapRel, tup);
	}
	else
	{
		oid = simple_heap_insert(heapRel, tup);
	}

	indstate = CatalogOpenIndexes(heapRel);

	CatalogIndexInsert(indstate, tup, yb_shared_insert);
	CatalogCloseIndexes(indstate);

	return oid;
}

/*
 * CatalogTupleInsertWithInfo - as above, but with caller-supplied index info
 *
 * This should be used when it's important to amortize CatalogOpenIndexes/
 * CatalogCloseIndexes work across multiple insertions.  At some point we
 * might cache the CatalogIndexState data somewhere (perhaps in the relcache)
 * so that callers needn't trouble over this ... but we don't do so today.
 *
 * if yb_shared_insert is specified, this insert will be done in every
 * database (including template0 and template1). This is needed when
 * creating shared relations.
 * This flag should not be used during initdb bootstrap.
 */
Oid
CatalogTupleInsertWithInfo(Relation heapRel, HeapTuple tup,
						   CatalogIndexState indstate, bool yb_shared_insert)
{
	Oid			oid;

	if (IsYugaByteEnabled())
	{
		/* Keep ybctid consistent across all databases. */
		Datum ybctid = 0;

		if (yb_shared_insert)
		{
			if (!IsYsqlUpgrade)
				elog(ERROR, "shared insert cannot be done outside of YSQL upgrade");

			YB_FOR_EACH_DB(pg_db_tuple)
			{
				Oid dboid = HeapTupleGetOid(pg_db_tuple);
				/*
				 * Since this is a catalog table, we assume it exists in all databases.
				 * YB doesn't use PG locks so it's okay not to take them.
				 */
				if (dboid == YBCGetDatabaseOid(heapRel))
					continue; /* Will be done after the loop. */
				YBCExecuteInsertForDb(dboid,
									  heapRel,
									  RelationGetDescr(heapRel),
									  tup,
									  ONCONFLICT_NONE,
									  &ybctid);
			}
			YB_FOR_EACH_DB_END;
		}
		oid = YBCExecuteInsertForDb(YBCGetDatabaseOid(heapRel),
									heapRel,
									RelationGetDescr(heapRel),
									tup,
									ONCONFLICT_NONE,
									&ybctid);
		/* Update the local cache automatically */
		YbSetSysCacheTuple(heapRel, tup);
	}
	else
	{
		oid = simple_heap_insert(heapRel, tup);
	}

	CatalogIndexInsert(indstate, tup, yb_shared_insert);

	return oid;
}

/*
 * CatalogTupleUpdate - do heap and indexing work for updating a catalog tuple
 *
 * Update the tuple identified by "otid", replacing it with the data in "tup".
 *
 * This is a convenience routine for the common case of updating a single
 * tuple in a system catalog; it updates one heap tuple, keeping indexes
 * current.  Avoid using it for multiple tuples, since opening the indexes
 * and building the index info structures is moderately expensive.
 * (Use CatalogTupleUpdateWithInfo in such cases.)
 */
void
CatalogTupleUpdate(Relation heapRel, ItemPointer otid, HeapTuple tup)
{
	CatalogIndexState indstate;

	indstate = CatalogOpenIndexes(heapRel);

	if (IsYugaByteEnabled())
	{
		HeapTuple	oldtup = NULL;
		bool		has_indices = YBRelHasSecondaryIndices(heapRel);

		if (has_indices)
		{
			if (tup->t_ybctid)
			{
				oldtup = YBCFetchTuple(heapRel, tup->t_ybctid);
				CatalogIndexDelete(indstate, oldtup);
			}
			else
				YBC_LOG_WARNING("ybctid missing in %s's tuple",
								RelationGetRelationName(heapRel));
		}

		YBCUpdateSysCatalogTuple(heapRel, oldtup, tup);
		/* Update the local cache automatically */
		YbSetSysCacheTuple(heapRel, tup);

		if (has_indices)
			CatalogIndexInsert(indstate, tup, false /* yb_shared_insert */);
	}
	else
	{
		simple_heap_update(heapRel, otid, tup);

		CatalogIndexInsert(indstate, tup, false /* yb_shared_insert */);
	}

	CatalogCloseIndexes(indstate);
}

/*
 * CatalogTupleUpdateWithInfo - as above, but with caller-supplied index info
 *
 * This should be used when it's important to amortize CatalogOpenIndexes/
 * CatalogCloseIndexes work across multiple updates.  At some point we
 * might cache the CatalogIndexState data somewhere (perhaps in the relcache)
 * so that callers needn't trouble over this ... but we don't do so today.
 */
void
CatalogTupleUpdateWithInfo(Relation heapRel, ItemPointer otid, HeapTuple tup,
						   CatalogIndexState indstate)
{
	if (IsYugaByteEnabled())
	{
		HeapTuple	oldtup = NULL;
		bool		has_indices = YBRelHasSecondaryIndices(heapRel);

		if (has_indices)
		{
			if (tup->t_ybctid)
			{
				oldtup = YBCFetchTuple(heapRel, tup->t_ybctid);
				CatalogIndexDelete(indstate, oldtup);
			}
			else
				YBC_LOG_WARNING("ybctid missing in %s's tuple",
								RelationGetRelationName(heapRel));
		}

		YBCUpdateSysCatalogTuple(heapRel, oldtup, tup);
		/* Update the local cache automatically */
		YbSetSysCacheTuple(heapRel, tup);

		if (has_indices)
			CatalogIndexInsert(indstate, tup, false /* yb_shared_insert */);
	}
	else
	{
		simple_heap_update(heapRel, otid, tup);

		CatalogIndexInsert(indstate, tup, false /* yb_shared_insert */);
	}
}

/*
 * CatalogTupleDelete - do heap and indexing work for deleting a catalog tuple
 *
 * Delete the tuple identified by "tid" in the specified catalog.
 *
 * With Postgres heaps, there is no index work to do at deletion time;
 * cleanup will be done later by VACUUM.  However, callers of this function
 * shouldn't have to know that; we'd like a uniform abstraction for all
 * catalog tuple changes.  Hence, provide this currently-trivial wrapper.
 *
 * The abstraction is a bit leaky in that we don't provide an optimized
 * CatalogTupleDeleteWithInfo version, because there is currently nothing to
 * optimize.  If we ever need that, rather than touching a lot of call sites,
 * it might be better to do something about caching CatalogIndexState.
 */
void
CatalogTupleDelete(Relation heapRel, HeapTuple tup)
{
	if (IsYugaByteEnabled())
	{
		YBCDeleteSysCatalogTuple(heapRel, tup);

		CatalogIndexState indstate = CatalogOpenIndexes(heapRel);

		CatalogIndexDelete(indstate, tup);
		CatalogCloseIndexes(indstate);
	}
	else
	{
		simple_heap_delete(heapRel, &tup->t_self);
	}
}
