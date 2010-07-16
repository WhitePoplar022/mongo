/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_db_col_del --
 *	Db.col_del method.
 */
int
__wt_db_col_del(WT_TOC *toc, u_int64_t recno)
{
	ENV *env;
	IDB *idb;
	WT_COL_EXPAND *exp, **new_expcol;
	WT_PAGE *page;
	WT_REPL **new_repl, *repl;
	int ret;

	env = toc->env;
	idb = toc->db->idb;

	page = NULL;
	exp = NULL;
	new_expcol = NULL;
	new_repl = NULL;
	repl = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_bt_search_col(toc, recno));
	page = toc->srch_page;

	/*
	 * Repeat-count compressed (RCC) column store operations are difficult
	 * because each original on-disk index for an RCC can represent large
	 * numbers of records, and we're only deleting a single one of those
	 * records, which means working in the WT_COL_EXPAND array.  All other
	 * column store deletes are simple changes where a new WT_REPL entry is
	 * entered into the page's modification array.  There are three code
	 * paths:
	 *
	 * 1: column store deletes other than RCC column stores: delete an entry
	 * from the on-disk page by creating a new WT_REPL entry, and linking it
	 * into the WT_REPL array.
	 *
	 * 2: an RCC column store delete of a record not yet modified: create
	 * a new WT_COL_EXPAND/WT_REPL pair, and link it into the WT_COL_EXPAND
	 * array.
	 *
	 * 3: an RCC columstore n delete of an already modified record: create
	 * a new WT_REPL entry, and link it to the WT_COL_EXPAND entry's WT_REPL
	 * list.
	 */
	if (!F_ISSET(idb, WT_REPEAT_COMP)) {		/* #1 */
		/* Allocate a page replacement array if necessary. */
		if (page->repl == NULL)
			WT_ERR(__wt_calloc(env,
			    page->indx_count, sizeof(WT_REPL *), &new_repl));

		/* Allocate a WT_REPL structure and fill it in. */
		WT_ERR(__wt_calloc(env, 1, sizeof(WT_REPL), &repl));
		repl->data = WT_REPL_DELETED_VALUE;

		/* Schedule the workQ to insert the WT_REPL structure. */
		__wt_bt_update_serial(toc, page,
		    WT_COL_SLOT(page, toc->srch_ip), new_repl, repl, ret);
	} else if (toc->srch_repl == NULL) {		/* #2 */
		/* Allocate a page expansion array as necessary. */
		if (page->expcol == NULL)
			WT_ERR(__wt_calloc(env, page->indx_count,
			    sizeof(WT_COL_EXPAND *), &new_expcol));

		/* Allocate a WT_COL_EXPAND structure and fill it in. */
		WT_ERR(__wt_calloc(env, 1, sizeof(WT_COL_EXPAND), &exp));
		WT_ERR(__wt_calloc(env, 1, sizeof(WT_REPL), &repl));
		exp->rcc_offset = toc->srch_rcc_offset;
		exp->repl = repl;
		repl->data = WT_REPL_DELETED_VALUE;

		/* Schedule the workQ to link in the WT_COL_EXPAND structure. */
		__wt_bt_rcc_expand_serial(toc, page,
		    WT_COL_SLOT(page, toc->srch_ip), new_expcol, exp, ret);
		goto done;
	} else {					/* #3 */
		/* Allocate a WT_REPL structure and fill it in. */
		WT_ERR(__wt_calloc(env, 1, sizeof(WT_REPL), &repl));
		repl->data = WT_REPL_DELETED_VALUE;

		/* Schedule the workQ to insert the WT_REPL structure. */
		__wt_bt_rcc_expand_repl_serial(
		    toc, page, toc->srch_exp, repl, ret);
	}

	if (0) {
err:		if (exp != NULL)
			__wt_free(env, exp, sizeof(WT_COL_EXPAND));
		if (repl != NULL)
			__wt_free(env, repl, sizeof(WT_REPL));
	}

done:	/* Free any allocated page expansion array unless the workQ used it. */
	if (new_expcol != NULL && new_expcol != page->expcol)
		__wt_free(env,
		    new_expcol, page->indx_count * sizeof(WT_COL_EXPAND *));

	/* Free any replacement array unless the workQ used it. */
	if (new_repl != NULL && new_repl != page->repl)
		__wt_free(env, new_repl, page->indx_count * sizeof(WT_REPL *));

	if (page != NULL && page != idb->root_page)
		__wt_bt_page_out(toc, &page, ret == 0 ? WT_MODIFIED : 0);

	return (0);
}

/*
 * __wt_bt_rcc_expand_serial_func --
 *	Server function to expand a repeat-count compressed column store
 *	during a delete.
 */
int
__wt_bt_rcc_expand_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_COL_EXPAND **new_exp, *exp;
	int slot;

	__wt_bt_rcc_expand_unpack(toc, page, slot, new_exp, exp);

	/*
	 * If the page does not yet have an expansion array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect & free the passed-in expansion array if we don't use it.)
	 */
	if (page->expcol == NULL)
		page->expcol = new_exp;

	/*
	 * Insert the new WT_COL_EXPAND as the first item in the forward-linked
	 * list of expansion structures.  Flush memory to ensure the list is
	 * never broken.
	 */
	exp->next = page->expcol[slot];
	WT_MEMORY_FLUSH;
	page->expcol[slot] = exp;
	WT_PAGE_MODIFY_SET_AND_FLUSH(page);
	return (0);
}

/*
 * __wt_bt_rcc_expand_repl_serial_func --
 *	Server function to update a WT_REPL entry in an already expanded
 *	repeat-count compressed column store during a delete.
 */
int
__wt_bt_rcc_expand_repl_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_COL_EXPAND *exp;
	WT_REPL *repl;

	__wt_bt_rcc_expand_repl_unpack(toc, page, exp, repl);

	/*
	 * Insert the new WT_REPL as the first item in the forward-linked list
	 * of replacement structures from the WT_COL_EXPAND structure.  Flush
	 * memory to ensure the list is never broken.
	 */
	repl->next = exp->repl;
	WT_MEMORY_FLUSH;
	exp->repl = repl;
	WT_PAGE_MODIFY_SET_AND_FLUSH(page);
	return (0);
}
