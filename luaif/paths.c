/*
 * Copyright (C) 2006,2007 Lauri Leukkunen <lle@rahina.org>
 * Portion Copyright (c) 2008 Nokia Corporation.
 * (symlink- and path resolution code refactored by Lauri T. Aarnio at Nokia)
 *
 * Licensed under LGPL version 2.1, see top level LICENSE file for details.
 *
 * ----------------
 *
 * This file implements path resolution for SB2. Please read background
 * information from the "path_resolution" manual page (Linux documentation).
 *
 * Path resolution usually belongs to the operating system, but SB2 must
 * implement a replacement because of one specific feature: Symbolic
 * links must be mapped. For example, if path "/a/b/c" contains
 * intermediate symbolic links (i.e. "a" or "b" is a symlink), the path
 * mapping engine must be called for those all of those symbolic links.
 *
 * TERMINOLOGY:
 *
 * - an _absolute path_ is a path which begins with a slash '/'
 *
 * - a _relative path_ is a path which does not begin with a slash '/',
 *   instead it is relative to the current working directory
 *
 * - a _clean path_ is a path which does not contain a dot ('.',
 *   referring to the current directory), a doubled dot (parent
 *   directory) or doubled slashes (redundant) as a component.
 *
 * - a _virtual path_ is a path that has not been mapped.
 *
 * - a _host path_ is a path that refers to the real filesystem
 *   (i.e. a mapped path)
 *
 * - a _real path_ is an absolute path where none of components is a 
 *   symbolic link and it does not have any . or .. inside
 *   (see realpath(3) for details).
 *   NOTE that within Scratchbox 2 we may have both "host realpaths"
 *   and "virtual realpaths!"
 *
 * - a _resolved path_ is otherwise like a _real path_, but the
 *   last component of a _resolved path_ may be a symbolic link.
 *
 * The division between Lua and C code is that
 * - C has been used for parts that must follow "standardized" or "fixed"
 *   approach, and the implementation is not expected to be changed, in other
 *   words, the implementation/design freedom is restricted by external
 *   requirements.
 * - Lua has been used for the mapping algorithm itself.
*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _GNU_SOURCE
#undef _GNU_SOURCE
#include <string.h>
#include <libgen.h>
#define _GNU_SOURCE
#else
#include <string.h>
#include <libgen.h>
#endif

#include <limits.h>
#include <sys/param.h>
#include <sys/file.h>
#include <assert.h>
#include <pthread.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <mapping.h>
#include <sb2.h>
#include "libsb2.h"
#include "exported.h"

#ifdef EXTREME_DEBUGGING
#include <execinfo.h>
#endif

typedef struct path_mapping_context_s {
	const char		*pmc_binary_name;
	const char		*pmc_func_name;
	const char		*pmc_virtual_orig_path;
	int			pmc_dont_resolve_final_symlink;
	struct lua_instance	*pmc_luaif;
} path_mapping_context_t;

#define clear_path_mapping_context(p) {memset((p),0,sizeof(*(p)));}

/* ========== Path & Path component handling primitives: ========== */

struct path_entry {
	struct path_entry *pe_prev;
	struct path_entry *pe_next;

	int	pe_flags;
	char	*pe_link_dest;	/* used only for symlinks */

	int	pe_path_component_len;

	/* pe_path_component MUST BE the last member of this
	 * struct, this is really a larger string buffer:
	 * pe_path_component[pe_path_component_len+1] */
	char	pe_path_component[1];
};

struct path_entry_list {
	struct path_entry	*pl_first;
	int			pl_flags;
};

#define PATH_FLAGS_ABSOLUTE	01
#define PATH_FLAGS_HAS_TRAILING_SLASH	02
#define PATH_FLAGS_HOST_PATH	04

#define PATH_FLAGS_NOT_SYMLINK	010
#define PATH_FLAGS_IS_SYMLINK	020

#define clear_path_entry_list(p) {memset((p),0,sizeof(*(p)));}

static void set_flags_in_path_entries(struct path_entry *pep, int flags)
{
	while (pep) {
		pep->pe_flags |= flags;
		pep = pep->pe_next;
	}
}

static struct path_entry *append_path_entries(
	struct path_entry *head,
	struct path_entry *new_entries)
{
	struct path_entry *work = head;
	
	if (!head) return(new_entries);

	/* move work to point to last component of "head" */
	while (work->pe_next) work = work->pe_next;

	work->pe_next = new_entries;
	if (new_entries) new_entries->pe_prev = work;

	return (head);
}

/* returns an allocated buffer */
static char *path_entries_to_string_until(
	const struct path_entry *p_entry,
	const struct path_entry *last_path_entry_to_include,
	int flags)
{
	char *buf;
	const struct path_entry *work;
	int len;

	if (!p_entry) {
		/* "p_entry" will be empty if orig.path was "/." */
		return(strdup("/"));
	}

	/* first, count length of the buffer */
	work = p_entry;
	len = 0;
	while (work) {
		len++; /* "/" */
		len += work->pe_path_component_len;
		if (work == last_path_entry_to_include) break;
		work = work->pe_next;
	}
	len += 2; /* for trailing (optional) '/' and \0 */

	buf = malloc(len);
	*buf = '\0';

	/* add path components to the buffer */
	work = p_entry;
	if (flags & PATH_FLAGS_ABSOLUTE) {
		strcpy(buf, "/");
	}
	while (work) {
		int component_is_empty;
		if (*work->pe_path_component) {
			strcat(buf, work->pe_path_component);
			component_is_empty = 0;
		} else {
			component_is_empty = 1;
		}
		if (work == last_path_entry_to_include) break;
		work = work->pe_next;
		if (work && (component_is_empty==0)) {
			strcat(buf, "/");
		}
	}
	if (flags & PATH_FLAGS_HAS_TRAILING_SLASH) {
		char *cp = buf;
		/* point to last char in buf */
		while (*cp && cp[1]) cp++; 
		if (*cp != '/') strcat(buf, "/");
	}

	return(buf);
}

static char *path_entries_to_string(
	const struct path_entry *p_entry,
	int flags)
{
	return(path_entries_to_string_until(p_entry, NULL, flags));
}

static char *path_list_to_string(const struct path_entry_list *listp)
{
	return(path_entries_to_string_until(listp->pl_first, NULL,
		listp->pl_flags));
}

static void free_path_entry(struct path_entry *work)
{
	SB_LOG(SB_LOGLEVEL_NOISE3,
		"free_path_entry %lX <p=%lX n=%lX> len=%d '%s' (link_dest=%s)",
		(long)work, (long)work->pe_prev, (long)work->pe_next,
		work->pe_path_component_len, work->pe_path_component,
		(work->pe_link_dest ? work->pe_link_dest : NULL));
	if (work->pe_link_dest) free(work->pe_link_dest);
	free(work);
}

static void free_path_entries(struct path_entry *work)
{
	while (work) {
		struct path_entry *next = work->pe_next;
		free_path_entry(work);
		work = next;
	}
}

static void free_path_list(struct path_entry_list *listp)
{
	free_path_entries(listp->pl_first);
	clear_path_entry_list(listp);
}


static struct path_entry *split_path_to_path_entries(
	const char *cpath, int *flagsp)
{
	struct path_entry *first = NULL;
	struct path_entry *work = NULL;
	const char *start;
	const char *next_slash;
	int flags = 0;

	SB_LOG(SB_LOGLEVEL_NOISE3, "going to split '%s'", cpath);

	start = cpath;
	if (*start == '/') {
		while(*start == '/') start++;	/* ignore leading '/' */
		flags |= PATH_FLAGS_ABSOLUTE;
	}

	do {
		next_slash = strchr(start, '/');

		/* ignore empty strings resulting from // */
		if (next_slash != start) {
			struct path_entry *new;
			int	len;

			if (next_slash) {
				len = next_slash - start;
				if (!next_slash[1]) {
					flags |= PATH_FLAGS_HAS_TRAILING_SLASH;
					next_slash = NULL;
				}
			} else {
				/* no more slashes */
				len = strlen(start);
			}
			new = malloc(sizeof(struct path_entry) + len);
			if(!first) first = new;
			if (!new) abort();
			memset(new, 0, sizeof(struct path_entry));
			strncpy(new->pe_path_component, start, len);
			new->pe_path_component[len] = '\0';
			new->pe_path_component_len = len;

			new->pe_prev = work;
			if(work) work->pe_next = new;
			new->pe_next = NULL;
			work = new;
			SB_LOG(SB_LOGLEVEL_NOISE3,
				"created entry 0x%lX '%s'",
				(unsigned long int)work, new->pe_path_component);
		}
		if (next_slash) start = next_slash + 1;
	} while (next_slash != NULL);

	if (flagsp) *flagsp = flags;
	return (first);
}

static void split_path_to_path_list(
	const char *cpath,
	struct path_entry_list	*listp)
{
	listp->pl_first = split_path_to_path_entries(cpath, &(listp->pl_flags));

	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE2)) {
		char *tmp_path_buf = path_list_to_string(listp);

		SB_LOG(SB_LOGLEVEL_NOISE2, "split->'%s'", tmp_path_buf);
		free(tmp_path_buf);
	}
}

static struct path_entry *duplicate_path_entries_until(
	const struct path_entry *duplicate_until_this_component,
	const struct path_entry *source_path)
{
	struct path_entry *first = NULL;
	struct path_entry *dest_path_ptr = NULL;

	SB_LOG(SB_LOGLEVEL_NOISE3, "Duplicating path:");

	while (source_path) {
		struct path_entry *new;
		int	len = source_path->pe_path_component_len;

		new = malloc(sizeof(struct path_entry) + len);
		if (!new) abort();
		if(!first) first = new;
		memset(new, 0, sizeof(struct path_entry) + len);

		strncpy(new->pe_path_component, source_path->pe_path_component, len);
		new->pe_path_component[len] = '\0';
		new->pe_path_component_len = len;

		if (source_path->pe_link_dest)
			new->pe_link_dest = strdup(source_path->pe_link_dest);

		new->pe_prev = dest_path_ptr;
		if (dest_path_ptr) dest_path_ptr->pe_next = new;
		new->pe_next = NULL;
		dest_path_ptr = new;
		SB_LOG(SB_LOGLEVEL_NOISE3,
			"dup: entry 0x%X '%s'",
			(unsigned long int)dest_path_ptr, new->pe_path_component);
		if (duplicate_until_this_component == source_path) break;
		source_path = source_path->pe_next;
	}

	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE3)) {
		char *tmp_path_buf = path_entries_to_string(first, 0);

		SB_LOG(SB_LOGLEVEL_NOISE3, "dup->'%s'", tmp_path_buf);
		free(tmp_path_buf);
	}
	return(first);
}

static void	duplicate_path_list_until(
	const struct path_entry *duplicate_until_this_component,
	struct path_entry_list *new_path_list,
	const struct path_entry_list *source_path_list)
{
	struct path_entry *duplicate = NULL;

	duplicate = duplicate_path_entries_until(
		duplicate_until_this_component, source_path_list->pl_first);

	new_path_list->pl_first = duplicate;
	new_path_list->pl_flags = source_path_list->pl_flags;
}

/* remove a path_entry from list, return pointer to the next
 * path_entry after the removed one (NULL if the removed entry was last)
*/
static struct path_entry *remove_path_entry(
	struct path_entry_list *listp,
	struct path_entry *p_entry)	/* entry to be removed */
{
	struct path_entry *ret = p_entry->pe_next;

	SB_LOG(SB_LOGLEVEL_NOISE3,
		"remove_path_entry at %lX, next=%lX",
		(long)p_entry, (long)ret);
	if (p_entry->pe_prev) {
		/* not the first element in the list */
		SB_LOG(SB_LOGLEVEL_NOISE3,
			"remove_path_entry, not first");
		p_entry->pe_prev->pe_next = p_entry->pe_next;
		if(p_entry->pe_next)
			p_entry->pe_next->pe_prev = p_entry->pe_prev;
	} else {
		SB_LOG(SB_LOGLEVEL_NOISE3,
			"remove_path_entry, first");
		/* removing first element from the list */
		assert(p_entry == listp->pl_first);
		listp->pl_first = p_entry->pe_next;
		if(p_entry->pe_next)
			p_entry->pe_next->pe_prev = NULL;
	}
	free_path_entry(p_entry);
	return(ret);
}

/* remove_dots_from_path_list(), "easy" path cleaning:
 * - all dots, i.e. "." as components, can be safely removed,
 *   BUT if the last component is a dot, then the path will be
 *   marked as having a trailing slash (it has the same meaning,
 *   see the man page about path_resolution)
 * - doubled slashes ("//") have already been removed, when
 *   the path was split to components.
*/
static void remove_dots_from_path_list(struct path_entry_list *listp)
{
	struct path_entry *work = listp->pl_first;

	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE2)) {
		char *tmp_path_buf = path_list_to_string(listp);

		SB_LOG(SB_LOGLEVEL_NOISE2,
			"remove_dots: '%s'", tmp_path_buf);
		free(tmp_path_buf);
	}
	while (work) {
		SB_LOG(SB_LOGLEVEL_NOISE2,
			"remove_dots: work=0x%X examine '%s'",
			(unsigned long int)work, work?work->pe_path_component:"");

		if ((work->pe_path_component[0] == '.') &&
		    (work->pe_path_component[1] == '\0')) {
			if (!work->pe_next) {
				/* last component */
				listp->pl_flags |= PATH_FLAGS_HAS_TRAILING_SLASH;
			}
			/* remove this node */
			work = remove_path_entry(listp, work);
		} else {
			work = work->pe_next;
		}
	}
	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE2)) {
		char *tmp_path_buf = path_list_to_string(listp);

		SB_LOG(SB_LOGLEVEL_NOISE2,
			"remove_dots: result->'%s'", tmp_path_buf);
		free(tmp_path_buf);
	}
}

static struct path_entry *remove_dotdot_entry_and_prev_entry(
	struct path_entry_list *listp,
	struct path_entry *work)
{
	struct path_entry *dotdot = work;
	struct path_entry *preventry = work->pe_prev;

	SB_LOG(SB_LOGLEVEL_NOISE3,
		"remove_dotdot_entry_and_prev_entry at %lX",
		(long)work);
	if (preventry) {
		/* travel up, and eliminate previous name */
		work = remove_path_entry(listp, preventry);
		assert(work == dotdot);
	} else {
		/* no preventry, first component is .. */
		assert(work == listp->pl_first);
	}
	return(remove_path_entry(listp, dotdot));
}

/* check if the path is clean:
 * returns
 *   0: path is clean = does not contain "." or ".."
 *   1: path is not clean, contains one or more dots ("."), but no ".."
 *   2: path is not clean, ".." (requires more complex cleanup)
*/
static int is_clean_path(struct path_entry_list *listp)
{
	struct path_entry *work = listp->pl_first;
	int	found_dot = 0;

	/* check if the path contains "." or ".." as components */
	while (work) {
		const char	*cp = work->pe_path_component;
		if (*cp == '.') {
			if (cp[1] == '\0') {
				found_dot = 1;
			} else if (cp[1] == '.' && cp[2] == '\0') {
				/* found ".." */
				SB_LOG(SB_LOGLEVEL_NOISE,
					"is_clean_path: dirty, found ..");
				return(2);
			}
		}
		work = work->pe_next;
	}

	if (found_dot) {
		SB_LOG(SB_LOGLEVEL_NOISE, "is_clean_path: dirty, found .");
		return(1);
	}
	SB_LOG(SB_LOGLEVEL_NOISE, "is_clean_path: clean");
	return(0);
}

static void sb_path_resolution(
	const path_mapping_context_t *ctx,
	mapping_results_t *resolved_virtual_path_res,
	int nest_count,
	struct path_entry_list *abs_virtual_clean_source_path_list);

/* "complex" path cleaning: cleans ".." components from the
 * path. This may require resolving the parent entries, to
 * be sure that symlinks in the path are not removed
 * by accident.
 * Can be used for both virtual paths and host paths.
*/
static void clean_dotdots_from_path(
	const path_mapping_context_t *ctx,
	struct path_entry_list *abs_path)
{
	struct path_entry *work;
	int	path_has_nontrivial_dotdots = 0;

	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE)) {
		char *tmp_path_buf = path_list_to_string(abs_path);
		SB_LOG(SB_LOGLEVEL_NOISE,
			"clean_dotdots_from_path: '%s'", tmp_path_buf);
		free(tmp_path_buf);
	}
	if (!(abs_path->pl_flags & PATH_FLAGS_ABSOLUTE)) {
		SB_LOG(SB_LOGLEVEL_ERROR,
			"FATAL: clean_dotdots_from_path called with relative path");
		assert(0);
	}

	/* step 1: remove leading ".." entries, ".." in the
	 * root directory points to itself.
	*/
	SB_LOG(SB_LOGLEVEL_NOISE, "clean_dotdots_from_path: <1>");
	work = abs_path->pl_first;
	while (work) {
		if ((work->pe_path_component[0] == '.') &&
		    (work->pe_path_component[1] == '.') &&
		    (work->pe_path_component[2] == '\0')) {
			if (!work->pe_next) {
				/* last component */
				abs_path->pl_flags |= PATH_FLAGS_HAS_TRAILING_SLASH;
			}
			/* remove this node */
			work = remove_path_entry(abs_path, work);
		} else {
			/* now first component != ".." */
			break;
		}
	}

	if ((work == NULL) || is_clean_path(abs_path) < 2) goto done;

	/* step 2: 
	 * remove all ".." componets where the previous component
	 * is already known to be a real directory (well, actually
	 * known to be something else than a symlink)
	*/
	SB_LOG(SB_LOGLEVEL_NOISE, "clean_dotdots_from_path: <2>");
	work = abs_path->pl_first;
	while (work) {
		SB_LOG(SB_LOGLEVEL_NOISE3,
			"clean_dotdots_from_path: check %lX '%s'",
			(long)work, work->pe_path_component);
		if ((work->pe_path_component[0] == '.') &&
		    (work->pe_path_component[1] == '.') &&
		    (work->pe_path_component[2] == '\0')) {
			if ((work->pe_prev == NULL) ||
			    (work->pe_prev->pe_flags & PATH_FLAGS_NOT_SYMLINK)) {
				/* Either "/work" or "x/work", where "x"
				 * is not a symlink => safe to remove this node */
				if (!work->pe_next) {
					/* last component */
					abs_path->pl_flags |= PATH_FLAGS_HAS_TRAILING_SLASH;
				}
				SB_LOG(SB_LOGLEVEL_NOISE3,
					"clean_dotdots_from_path: remove at %lX",
					(long)work);
				work = remove_dotdot_entry_and_prev_entry(abs_path, work);
				SB_LOG(SB_LOGLEVEL_NOISE3,
					"clean_dotdots_from_path: removed, now at %lX",
					(long)work);
			} else {
				/* keep it there, remove it later */
				path_has_nontrivial_dotdots = 1;
				work = work->pe_next;
			}
		} else {
			work = work->pe_next;
		}
	}

	if (path_has_nontrivial_dotdots == 0) goto done;

	/* step 3: 
	 * remove all remaining ".." componets.
	 * the previous component might be a symlink, so
	 * path resolution must be done.
	*/
	SB_LOG(SB_LOGLEVEL_NOISE, "clean_dotdots_from_path: <3>");
	work = abs_path->pl_first;
	while (work) {
		if ((work->pe_path_component[0] == '.') &&
		    (work->pe_path_component[1] == '.') &&
		    (work->pe_path_component[2] == '\0')) {
			struct path_entry_list	abs_path_to_parent;
			mapping_results_t	resolved_parent_location;
			char			*orig_path_to_parent;

			clear_path_entry_list(&abs_path_to_parent);
			if (work->pe_prev) {
				duplicate_path_list_until(work->pe_prev,
					&abs_path_to_parent,
					abs_path);
			} else {
				/* else parent is the root directory;
				 * abs_path_to_parent is now empty,
				 * get flags from the parent (this 
				 * will get the "absolute" flag, among
				 * other things) */
				abs_path_to_parent.pl_flags = abs_path->pl_flags;
			}
			orig_path_to_parent = path_list_to_string(&abs_path_to_parent);

			SB_LOG(SB_LOGLEVEL_NOISE, "clean_dotdots_from_path: <3>: parent is '%s'",
				orig_path_to_parent);

			/* abs_path_to_parent is clean, isn't it?
			 * doublecheck to be sure.
			*/
			if (is_clean_path(&abs_path_to_parent) != 0) {
				SB_LOG(SB_LOGLEVEL_ERROR,
					"FATAL: clean_dotdots_from_path '%s' in not clean!",
					orig_path_to_parent);
				assert(0);
			}

			/* Now resolve the path. */
			clear_mapping_results_struct(&resolved_parent_location);

			if (abs_path->pl_flags & PATH_FLAGS_HOST_PATH) {
				char	rp[PATH_MAX+1];
				/* host path - enough to call realpath */
				SB_LOG(SB_LOGLEVEL_NOISE,
					"clean_dotdots_from_path: <3>: call realpath(%s)",
					orig_path_to_parent);
				realpath_nomap(orig_path_to_parent, rp);
				resolved_parent_location.mres_result_buf =
					resolved_parent_location.mres_result_path =
					strdup(rp);
			} else {
				/* virtual path */
				sb_path_resolution(ctx, &resolved_parent_location,
					0, &abs_path_to_parent);
			}
			free_path_list(&abs_path_to_parent);

			/* mres_result_buf contains an absolute path,
			 * unless the result was longer than PATH_MAX */
			if (strcmp(orig_path_to_parent,
			    resolved_parent_location.mres_result_buf)) {
				/* not same - a symlink was found & resolved */
				struct path_entry *real_virtual_path_to_parent;
				struct path_entry *prefix_to_be_removed;
				struct path_entry *remaining_suffix;

				SB_LOG(SB_LOGLEVEL_NOISE,
					"clean_dotdots_from_path: <3>:orig='%s'",
					orig_path_to_parent);
				SB_LOG(SB_LOGLEVEL_NOISE,
					"clean_dotdots_from_path: <3>:real='%s'",
					resolved_parent_location.mres_result_buf);

				real_virtual_path_to_parent = split_path_to_path_entries(
					resolved_parent_location.mres_result_buf, NULL);

				/* resolved_parent_location does not contain symlinks: */
				set_flags_in_path_entries(real_virtual_path_to_parent,
					PATH_FLAGS_NOT_SYMLINK);

				/* work points to ".." inside abs_path;
				 * cut abs_path to two:
				*/
				prefix_to_be_removed = abs_path->pl_first;
				work->pe_prev->pe_next = NULL;

				remaining_suffix = work;
				remaining_suffix->pe_prev = NULL;

				abs_path->pl_first = append_path_entries(
					real_virtual_path_to_parent, remaining_suffix);

				free_path_entries(prefix_to_be_removed);
				free(orig_path_to_parent);
				free_mapping_results(&resolved_parent_location);

				/* restart from the beginning of the new path: */
				clean_dotdots_from_path(ctx, abs_path);
				return;
			} 

			SB_LOG(SB_LOGLEVEL_NOISE,
				"clean_dotdots_from_path: <3>:same='%s'",
				orig_path_to_parent);

			free(orig_path_to_parent);
			free_mapping_results(&resolved_parent_location);

			if (!work->pe_next) {
				/* last component */
				abs_path->pl_flags |= PATH_FLAGS_HAS_TRAILING_SLASH;
			}
			work = remove_dotdot_entry_and_prev_entry(abs_path, work);
		} else {
			work = work->pe_next;
		}
	}

    done:
	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE)) {
		char *tmp_path_buf = path_list_to_string(abs_path);

		SB_LOG(SB_LOGLEVEL_NOISE,
			"clean_dotdots_from_path: result->'%s'", tmp_path_buf);
		free(tmp_path_buf);
	}
}

/* ========== Other helper functions: ========== */

#if 0
static char last_char_in_str(const char *str)
{
	if (!str || !*str) return('\0');
	while (str[1]) str++;
	return(*str);
}
#endif

static void check_mapping_flags(int flags, const char *fn)
{
	if (flags & (~SB2_MAPPING_RULE_ALL_FLAGS)) {
		SB_LOG(SB_LOGLEVEL_WARNING,
			"%s returned unknown flags (0%o)",
			fn, flags & (~SB2_MAPPING_RULE_ALL_FLAGS));
	}
}

/* ========== Interfaces to Lua functions: ========== */

/* note: this expects that the lua stack already contains the mapping rule,
 * needed by sbox_translate_path (lua code).
 * at exit this always leaves the rule AND exec policy to stack!
*/
static char *call_lua_function_sbox_translate_path(
	const path_mapping_context_t *ctx,
	int result_log_level,
	const char *abs_clean_virtual_path,
	int *flagsp)
{
	struct lua_instance	*luaif = ctx->pmc_luaif;
	int flags;
	char *host_path = NULL;

	SB_LOG(SB_LOGLEVEL_NOISE, "calling sbox_translate_path for %s(%s)",
		ctx->pmc_func_name, abs_clean_virtual_path);
	SB_LOG(SB_LOGLEVEL_NOISE,
		"call_lua_function_sbox_translate_path: gettop=%d",
		lua_gettop(luaif->lua));
	if(SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE3)) {
		dump_lua_stack("call_lua_function_sbox_translate_path entry",
			luaif->lua);
	}

	lua_getfield(luaif->lua, LUA_GLOBALSINDEX, "sbox_translate_path");
	/* stack now contains the rule object and string "sbox_translate_path",
         * move the string to the bottom: */
	lua_insert(luaif->lua, -2);
	/* add other parameters */
	lua_pushstring(luaif->lua, ctx->pmc_binary_name);
	lua_pushstring(luaif->lua, ctx->pmc_func_name);
	lua_pushstring(luaif->lua, abs_clean_virtual_path);
	 /* 4 arguments, returns rule,policy,path,flags */
	lua_call(luaif->lua, 4, 4);

	host_path = (char *)lua_tostring(luaif->lua, -2);
	if (host_path && (*host_path != '/')) {
		SB_LOG(SB_LOGLEVEL_ERROR,
			"Mapping failed: Result is not absolute ('%s'->'%s')",
			abs_clean_virtual_path, host_path);
		host_path = NULL;
	} else if (host_path) {
		host_path = strdup(host_path);
	}
	flags = lua_tointeger(luaif->lua, -1);
	check_mapping_flags(flags, "sbox_translate_path");
	if (flagsp) *flagsp = flags;
	lua_pop(luaif->lua, 2); /* leave rule and policy to the stack */

	if (host_path) {
		/* sometimes a mapping rule may create paths that contain
		 * doubled slashes ("//") or end with a slash. We'll
		 * need to clean the path here.
		*/
		char *cleaned_host_path;
		struct path_entry_list list;

		split_path_to_path_list(host_path, &list);
		list.pl_flags|= PATH_FLAGS_HOST_PATH;

		switch (is_clean_path(&list)) {
		case 0: /* clean */
			break;
		case 1: /* . */
			remove_dots_from_path_list(&list);
			break;
		case 2: /* .. */
			/* The rule inserted ".." to the path?
			 * not very wise move, maybe we should even log
			 * warning about this? However, cleaning is
			 * easy in this case; the result is a host
			 * path => cleanup doesn't need to make
			 * recursive calls to sb_path_resolution.
			*/
			remove_dots_from_path_list(&list);
			clean_dotdots_from_path(ctx, &list);
			break;
		}
		cleaned_host_path = path_list_to_string(&list);
		free_path_list(&list);

		if (*cleaned_host_path != '/') {
			/* oops, got a relative path. CWD is too long. */
			SB_LOG(SB_LOGLEVEL_DEBUG,
				"OOPS, call_lua_function_sbox_translate_path:"
				" relative");
		}
		free(host_path);
		host_path = NULL;

		/* log the result */
		if (strcmp(cleaned_host_path, abs_clean_virtual_path) == 0) {
			/* NOTE: Following SB_LOG() call is used by the log
			 *       postprocessor script "sb2logz". Do not change
			 *       without making a corresponding change to
			 *       the script!
			*/
			SB_LOG(result_log_level, "pass: %s '%s'%s",
				ctx->pmc_func_name, abs_clean_virtual_path,
				((flags & SB2_MAPPING_RULE_FLAGS_READONLY) ? " (readonly)" : ""));
		} else {
			/* NOTE: Following SB_LOG() call is used by the log
			 *       postprocessor script "sb2logz". Do not change
			 *       without making a corresponding change to
			 *       the script!
			*/
			SB_LOG(result_log_level, "mapped: %s '%s' -> '%s'%s",
				ctx->pmc_func_name, abs_clean_virtual_path, cleaned_host_path,
				((flags & SB2_MAPPING_RULE_FLAGS_READONLY) ? " (readonly)" : ""));
		}
		host_path = cleaned_host_path;
	}
	if (!host_path) {
		SB_LOG(SB_LOGLEVEL_ERROR,
			"No result from sbox_translate_path for: %s '%s'",
			ctx->pmc_func_name, abs_clean_virtual_path);
	}
	SB_LOG(SB_LOGLEVEL_NOISE,
		"call_lua_function_sbox_translate_path: at exit, gettop=%d",
		lua_gettop(luaif->lua));
	if(SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE3)) {
		dump_lua_stack("call_lua_function_sbox_translate_path exit",
			luaif->lua);
	}
	return(host_path);
}

/* - returns 1 if ok (then *min_path_lenp is valid)
 * - returns 0 if failed to find the rule
 * Note: this leave the rule to the stack!
*/
static int call_lua_function_sbox_get_mapping_requirements(
	const path_mapping_context_t *ctx,
	const struct path_entry_list *abs_virtual_source_path_list,
	int *min_path_lenp,
	int *call_translate_for_all_p)
{
	struct lua_instance	*luaif = ctx->pmc_luaif;
	int rule_found;
	int min_path_len;
	int flags;
	char	*abs_virtual_source_path_string;

	abs_virtual_source_path_string = path_list_to_string(abs_virtual_source_path_list);

	SB_LOG(SB_LOGLEVEL_NOISE,
		"calling sbox_get_mapping_requirements for %s(%s)",
		ctx->pmc_func_name, abs_virtual_source_path_string);
	SB_LOG(SB_LOGLEVEL_NOISE,
		"call_lua_function_sbox_get_mapping_requirements: gettop=%d",
		lua_gettop(luaif->lua));

	lua_getfield(luaif->lua, LUA_GLOBALSINDEX,
		"sbox_get_mapping_requirements");
	lua_pushstring(luaif->lua, ctx->pmc_binary_name);
	lua_pushstring(luaif->lua, ctx->pmc_func_name);
	lua_pushstring(luaif->lua, abs_virtual_source_path_string);
	/* 3 arguments, returns 4: (rule, rule_found_flag,
	 * min_path_len, flags) */
	lua_call(luaif->lua, 3, 4);

	rule_found = lua_toboolean(luaif->lua, -3);
	min_path_len = lua_tointeger(luaif->lua, -2);
	flags = lua_tointeger(luaif->lua, -1);
	check_mapping_flags(flags, "sbox_get_mapping_requirements");
	if (min_path_lenp) *min_path_lenp = min_path_len;
	if (call_translate_for_all_p)
		*call_translate_for_all_p =
			(flags & SB2_MAPPING_RULE_FLAGS_CALL_TRANSLATE_FOR_ALL);

	/* remove last 3 values; leave "rule" to the stack */
	lua_pop(luaif->lua, 3);

	SB_LOG(SB_LOGLEVEL_DEBUG, "sbox_get_mapping_requirements -> %d,%d,0%o",
		rule_found, min_path_len, flags);

	SB_LOG(SB_LOGLEVEL_NOISE,
		"call_lua_function_sbox_get_mapping_requirements:"
		" at exit, gettop=%d",
		lua_gettop(luaif->lua));
	free(abs_virtual_source_path_string);
	return(rule_found);
}

/* returns virtual_path */
static char *call_lua_function_sbox_reverse_path(
	const path_mapping_context_t *ctx,
	const char *abs_host_path)
{
	struct lua_instance	*luaif = ctx->pmc_luaif;
	char *virtual_path = NULL;
	int flags;

	SB_LOG(SB_LOGLEVEL_NOISE, "calling sbox_reverse_path for %s(%s)",
		ctx->pmc_func_name, abs_host_path);

	lua_getfield(luaif->lua, LUA_GLOBALSINDEX, "sbox_reverse_path");
	lua_pushstring(luaif->lua, ctx->pmc_binary_name);
	lua_pushstring(luaif->lua, ctx->pmc_func_name);
	lua_pushstring(luaif->lua, abs_host_path);
	 /* 3 arguments, returns virtual_path and flags */
	lua_call(luaif->lua, 3, 2);

	virtual_path = (char *)lua_tostring(luaif->lua, -2);
	if (virtual_path) {
		virtual_path = strdup(virtual_path);
	}

	flags = lua_tointeger(luaif->lua, -1);
	check_mapping_flags(flags, "sbox_reverse_path");
	/* Note: "flags" is not yet used for anything, intentionally */
 
	lua_pop(luaif->lua, 2); /* remove return values */

	if (virtual_path) {
		SB_LOG(SB_LOGLEVEL_DEBUG, "virtual_path='%s'", virtual_path);
	} else {
		SB_LOG(SB_LOGLEVEL_INFO,
			"No result from sbox_reverse_path for: %s '%s'",
			ctx->pmc_func_name, abs_host_path);
	}
	return(virtual_path);
}

/* ========== Path resolution: ========== */

/* clean up path resolution environment from lua stack */
#define drop_policy_from_lua_stack(luaif) drop_from_lua_stack(luaif,"policy")
#define drop_rule_from_lua_stack(luaif) drop_from_lua_stack(luaif,"rule")

static void drop_from_lua_stack(struct lua_instance *luaif,
	const char *o_name)
{
	lua_pop(luaif->lua, 1);

	SB_LOG(SB_LOGLEVEL_NOISE,
		"drop %s from stack: at exit, gettop=%d", o_name,
		lua_gettop(luaif->lua));
}

static void sb_path_resolution_resolve_symlink(
	const path_mapping_context_t *ctx,
	const char *link_dest,
	const struct path_entry_list *virtual_source_path_list,
	const struct path_entry *virtual_path_work_ptr,
	mapping_results_t *resolved_virtual_path_res,
	int nest_count);

/* sb_path_resolution():  This is the place where symlinks are followed.
 *
 * Returns an allocated buffer containing the resolved path (or NULL if error)
 *
 * Note: when this function returns, lua stack contains the rule which was
 *       used to do the path resolution. drop_rule_from_lua_stack() must
 *       be called after it is not needed anymore!
*/
static void sb_path_resolution(
	const path_mapping_context_t *ctx,
	mapping_results_t *resolved_virtual_path_res,
	int nest_count,
	struct path_entry_list *abs_virtual_clean_source_path_list)
{
	struct path_entry *virtual_path_work_ptr;
	int	component_index = 0;
	int	min_path_len_to_check;
	char	*prefix_mapping_result_host_path = NULL;
	int	prefix_mapping_result_host_path_flags;
	int	call_translate_for_all = 0;
	int	abs_virtual_source_path_has_trailing_slash;

	if (!abs_virtual_clean_source_path_list) {
		SB_LOG(SB_LOGLEVEL_ERROR,
			"sb_path_resolution called with NULL path");
		return;
	}

	if (nest_count > 16) {
		char *avsp = path_list_to_string(abs_virtual_clean_source_path_list);
		SB_LOG(SB_LOGLEVEL_ERROR,
			"Detected too deep nesting "
			"(too many symbolic links, path='%s')",
			avsp);
		free(avsp);

		/* return ELOOP to the calling program */
		resolved_virtual_path_res->mres_errno = ELOOP;
		return;
	}

	if (!(abs_virtual_clean_source_path_list->pl_flags & PATH_FLAGS_ABSOLUTE)) {
		char *tmp_path_buf = path_list_to_string(abs_virtual_clean_source_path_list);
		SB_LOG(SB_LOGLEVEL_ERROR,
			"FATAL: sb_path_resolution called with relative path (%s)",
			tmp_path_buf);
		assert(0);
	}

	if (is_clean_path(abs_virtual_clean_source_path_list) != 0) {
		char *tmp_path_buf = path_list_to_string(abs_virtual_clean_source_path_list);
		SB_LOG(SB_LOGLEVEL_ERROR,
			"FATAL: sb_path_resolution must be called with a clean path (%s)",
			tmp_path_buf);
		assert(0);
	}

	virtual_path_work_ptr = abs_virtual_clean_source_path_list->pl_first;
	abs_virtual_source_path_has_trailing_slash =
		(abs_virtual_clean_source_path_list->pl_flags & PATH_FLAGS_HAS_TRAILING_SLASH);

	if (call_lua_function_sbox_get_mapping_requirements(
		ctx, abs_virtual_clean_source_path_list,
		&min_path_len_to_check, &call_translate_for_all)) {
		/* has requirements:
		 * skip over path components that we are not supposed to check,
		 * because otherwise rule recognition & execution could fail.
		*/
		int	skipped_len = 1; /* start from 1, abs path has '/' in the beginning */
		SB_LOG(SB_LOGLEVEL_NOISE2,
			"min_path_len_to_check=%d", min_path_len_to_check);

		while (skipped_len < min_path_len_to_check) {
			SB_LOG(SB_LOGLEVEL_NOISE2, "skipping [%d] '%s' (%d,%d)",
				component_index, virtual_path_work_ptr->pe_path_component,
				skipped_len, virtual_path_work_ptr->pe_path_component_len);
			component_index++;
			skipped_len += virtual_path_work_ptr->pe_path_component_len;
			skipped_len++; /* add one due to the slash which lays between components */
			virtual_path_work_ptr = virtual_path_work_ptr->pe_next;
		}
	}

	SB_LOG(SB_LOGLEVEL_NOISE, "Path resolutions starts from [%d] '%s'",
		component_index, (virtual_path_work_ptr ?
			virtual_path_work_ptr->pe_path_component : ""));

	/* (the source path is clean.) */
	{
		char	*clean_virtual_path_prefix_tmp = NULL;
		path_mapping_context_t	ctx_copy = *ctx;

		ctx_copy.pmc_binary_name = "PATH_RESOLUTION";

		clean_virtual_path_prefix_tmp = path_entries_to_string_until(
			abs_virtual_clean_source_path_list->pl_first,
			virtual_path_work_ptr, PATH_FLAGS_ABSOLUTE);

		SB_LOG(SB_LOGLEVEL_NOISE, "clean_virtual_path_prefix_tmp => %s",
			clean_virtual_path_prefix_tmp);

		prefix_mapping_result_host_path = call_lua_function_sbox_translate_path(
			&ctx_copy, SB_LOGLEVEL_NOISE,
			clean_virtual_path_prefix_tmp, &prefix_mapping_result_host_path_flags);
		drop_policy_from_lua_stack(ctx->pmc_luaif);
		free(clean_virtual_path_prefix_tmp);
	}

	SB_LOG(SB_LOGLEVEL_NOISE, "prefix_mapping_result_host_path before loop => %s",
		prefix_mapping_result_host_path);

	/* Path resolution loop = walk thru directories, and if a symlink
	 * is found, recurse..
	*/
	while (virtual_path_work_ptr) {
		char	link_dest[PATH_MAX+1];

		if (prefix_mapping_result_host_path_flags &
		    SB2_MAPPING_RULE_FLAGS_FORCE_ORIG_PATH) {
			/* "force_orig_path" is set when symlinks MUST NOT
			 * be followed. */
			SB_LOG(SB_LOGLEVEL_NOISE,
				"force_orig_path set => path resolution finished");
			break;
		}

		if (abs_virtual_source_path_has_trailing_slash) {
			/* a trailing slash and "/." at end of path
			 * are equivalent; in both cases the last
			 * real component of the path must be resolved.
			*/
		} else {
			/* test if the last component must be resolved: */
			if (ctx->pmc_dont_resolve_final_symlink &&
			    (virtual_path_work_ptr->pe_next == NULL)) {
				/* this is last component, but here a final symlink
				 * must not be resolved (calls like lstat(), rename(),
				 * etc)
				*/
				SB_LOG(SB_LOGLEVEL_NOISE2,
					"Won't check last component [%d] '%s'",
					component_index, virtual_path_work_ptr->pe_path_component);
				break;
			}
		}

		SB_LOG(SB_LOGLEVEL_NOISE, "path_resolution: test if symlink [%d] '%s'",
			component_index, prefix_mapping_result_host_path);

		if ((virtual_path_work_ptr->pe_flags & 
		     (PATH_FLAGS_IS_SYMLINK | PATH_FLAGS_NOT_SYMLINK)) == 0) {
			/* status unknow.
			 * determine if "prefix_mapping_result_host_path" is a symbolic link.
			 * this can't be done with lstat(), because lstat() does not
			 * exist as a function on Linux => lstat_nomap() can not be
			 * used eiher. fortunately readlink() is an ordinary function.
			*/
			int	link_len;

			link_len = readlink_nomap(prefix_mapping_result_host_path, link_dest, PATH_MAX);

			if (link_len > 0) {
				/* was a symlink */
				link_dest[link_len] = '\0';
				virtual_path_work_ptr->pe_link_dest = strdup(link_dest);
				virtual_path_work_ptr->pe_flags |= PATH_FLAGS_IS_SYMLINK;
			} else {
				virtual_path_work_ptr->pe_flags |= PATH_FLAGS_NOT_SYMLINK;
			}
		}

		if (virtual_path_work_ptr->pe_flags & PATH_FLAGS_IS_SYMLINK) {
			/* symlink */

			SB_LOG(SB_LOGLEVEL_NOISE,
				"Path resolution found symlink '%s' "
				"-> '%s'",
				prefix_mapping_result_host_path, link_dest);
			free(prefix_mapping_result_host_path);
			prefix_mapping_result_host_path = NULL;

			sb_path_resolution_resolve_symlink(ctx,
				virtual_path_work_ptr->pe_link_dest,
				abs_virtual_clean_source_path_list, virtual_path_work_ptr,
				resolved_virtual_path_res, nest_count);
			return;
		}

		/* not a symlink */
		virtual_path_work_ptr = virtual_path_work_ptr->pe_next;
		if (virtual_path_work_ptr) {
			if (call_translate_for_all) {
				/* call_translate_for_all is set when
				 * path resolution must call
				 * sbox_translate_path() for each component;
				 * this happens when a "custom_map_funct" has
				 * been set. "custom_map_funct" may use any
				 * kind of strategy to decide when mapping
				 * needs to be done, for example, the /proc
				 * mapping function looks at the suffix, and
				 * not at the prefix...
				*/
				char *virtual_path_prefix_to_map;
				path_mapping_context_t	ctx_copy = *ctx;

				ctx_copy.pmc_binary_name = "PATH_RESOLUTION/2";
				if (prefix_mapping_result_host_path) {
					free(prefix_mapping_result_host_path);
					prefix_mapping_result_host_path = NULL;
				}
				virtual_path_prefix_to_map = path_entries_to_string_until(
						abs_virtual_clean_source_path_list->pl_first,
						virtual_path_work_ptr,
						abs_virtual_clean_source_path_list->pl_flags);
				prefix_mapping_result_host_path =
					call_lua_function_sbox_translate_path(
						&ctx_copy, SB_LOGLEVEL_NOISE,
						virtual_path_prefix_to_map,
						&prefix_mapping_result_host_path_flags);
				free (virtual_path_prefix_to_map);
				drop_policy_from_lua_stack(ctx->pmc_luaif);
			} else {
				/* "standard mapping", based on prefix or
				 * exact match. Ok to skip sbox_translate_path()
				 * because here it would just add the component
				 * to end of the path; instead we'll do that
				 * here. This is a performance optimization.
				*/
				char	*next_dir = NULL;

				if (asprintf(&next_dir, "%s/%s",
					prefix_mapping_result_host_path,
					virtual_path_work_ptr->pe_path_component) < 0) {
					SB_LOG(SB_LOGLEVEL_ERROR,
						"asprintf failed");
				}
				if (prefix_mapping_result_host_path) {
					free(prefix_mapping_result_host_path);
					prefix_mapping_result_host_path = NULL;
				}
				prefix_mapping_result_host_path = next_dir;
			}
		} else {
			free(prefix_mapping_result_host_path);
			prefix_mapping_result_host_path = NULL;
		}
		component_index++;
	}
	if (prefix_mapping_result_host_path) {
		free(prefix_mapping_result_host_path);
		prefix_mapping_result_host_path = NULL;
	}

	/* All symbolic links have been resolved. */
	{
		char	*resolved_virtual_path_buf = NULL;

		resolved_virtual_path_buf = path_list_to_string(abs_virtual_clean_source_path_list);

		SB_LOG(SB_LOGLEVEL_NOISE,
			"sb_path_resolution returns '%s'", resolved_virtual_path_buf);
		resolved_virtual_path_res->mres_result_buf =
			resolved_virtual_path_res->mres_result_path =
			resolved_virtual_path_buf;
	}
}

static void sb_path_resolution_resolve_symlink(
	const path_mapping_context_t *ctx,
	const char *link_dest,
	const struct path_entry_list *virtual_source_path_list,
	const struct path_entry *virtual_path_work_ptr,
	mapping_results_t *resolved_virtual_path_res,
	int nest_count)
{
	struct path_entry *rest_of_virtual_path = NULL;
	struct path_entry_list new_abs_virtual_link_dest_path_list;

	new_abs_virtual_link_dest_path_list.pl_first = NULL;

	if (virtual_path_work_ptr->pe_next) {
		/* path has components after the symlink:
		 * duplicate remaining path, it needs to 
		 * be attached to symlink contents. 
		*/
		rest_of_virtual_path = duplicate_path_entries_until(
			NULL, virtual_path_work_ptr->pe_next);
	} /* else last component of the path was a symlink. */

	if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_NOISE) && rest_of_virtual_path) {
		char *tmp_path_buf = path_entries_to_string(rest_of_virtual_path, 0);
		SB_LOG(SB_LOGLEVEL_NOISE2, "resolve_symlink: rest='%s'", tmp_path_buf);
		free(tmp_path_buf);
	}

	if (*link_dest == '/') {
		/* absolute symlink.
		 * This is easy: just join the symlink
		 * and rest of path, and further mapping
		 * operations will take care of the rest.
		*/
		struct path_entry *symlink_entries = NULL;
		int flags = 0;

		SB_LOG(SB_LOGLEVEL_NOISE, "absolute symlink");
		symlink_entries = split_path_to_path_entries(link_dest, &flags);

		/* If we aren't resolving last component of path
		 * then we have to clear out the "trailing slash
		 * flag".  Without this we would end up with '/'
		 * appended when any of the symlinks we traverse
		 * ends up with slash ("a -> b/").
		 *
		 * We have to do the same for relative symlinks
		 * (see below).
		 */
		if (virtual_path_work_ptr->pe_next)
		     flags &= ~PATH_FLAGS_HAS_TRAILING_SLASH;

		if (rest_of_virtual_path) {
			append_path_entries(symlink_entries, rest_of_virtual_path);
			rest_of_virtual_path = NULL;
		}
		new_abs_virtual_link_dest_path_list.pl_first = symlink_entries;
		new_abs_virtual_link_dest_path_list.pl_flags = flags;
	} else {
		/* A relative symlink. Somewhat complex:
		 * We must still build the full path from the
		 * place where we pretend to be - otherwise
		 * path mapping code would fail to find the
		 * correct location. Hence "dirnam" is
		 * based on what was mapped, and not based on
		 * were the mapping took us.
		*/
		struct path_entry *symlink_entries = NULL;
		struct path_entry *dirnam_entries;
		struct path_entry *link_dest_entries;
		int flags = 0;

		SB_LOG(SB_LOGLEVEL_NOISE, "relative symlink");
		/* first, set dirnam_entries to be
		 * the path to the parent directory.
		*/
		if (virtual_path_work_ptr->pe_prev) {
			dirnam_entries = duplicate_path_entries_until(
				virtual_path_work_ptr->pe_prev,
				virtual_source_path_list->pl_first);
		} else {
			/* else parent directory = rootdir, 
			 * dirnam_entries=NULL signifies that. */
			dirnam_entries = NULL;
		}

		link_dest_entries = split_path_to_path_entries(link_dest, &flags);

		/* Avoid problems with symlinks containing trailing
		 * slash ("a -> b/").
		 */
		if (virtual_path_work_ptr->pe_next)
		     flags &= ~PATH_FLAGS_HAS_TRAILING_SLASH;

		symlink_entries = append_path_entries(
			dirnam_entries, link_dest_entries);
		link_dest_entries = NULL;
		dirnam_entries = NULL;

		if (rest_of_virtual_path) {
			symlink_entries = append_path_entries(
				symlink_entries, rest_of_virtual_path);
			rest_of_virtual_path = NULL;;
		}

		new_abs_virtual_link_dest_path_list.pl_first = symlink_entries;
		new_abs_virtual_link_dest_path_list.pl_flags =
			virtual_source_path_list->pl_flags | flags;
	}

	/* double-check the result. We MUST use absolute
	 * paths here.
	*/
	if (!(new_abs_virtual_link_dest_path_list.pl_flags & PATH_FLAGS_ABSOLUTE)) {
		/* this should never happen */
		SB_LOG(SB_LOGLEVEL_ERROR,
			"FATAL: symlink resolved to "
			"a relative path (internal error)");
		assert(0);
	}

	/* recursively call sb_path_resolution() to perform path
	 * resolution steps for the symlink target.
	*/

	/* First, forget the old rule: */
	drop_rule_from_lua_stack(ctx->pmc_luaif);

	/* sb_path_resolution() needs to get clean path, but
	 * new_abs_virtual_link_dest_path is not necessarily clean.
	 * it may contain . or .. as a result of symbolic link expansion
	*/
	switch (is_clean_path(&new_abs_virtual_link_dest_path_list)) {
	case 0: /* clean */
		break;
	case 1: /* . */
		remove_dots_from_path_list(&new_abs_virtual_link_dest_path_list);
		break;
	case 2: /* .. */
		remove_dots_from_path_list(&new_abs_virtual_link_dest_path_list);
		clean_dotdots_from_path(ctx, &new_abs_virtual_link_dest_path_list);
		break;
	}

	/* Then the recursion.
	 * NOTE: new_abs_virtual_link_dest_path is not necessarily
	 * a clean path, because the symlink may have pointed to .. */
	sb_path_resolution(ctx, resolved_virtual_path_res, nest_count + 1,
		&new_abs_virtual_link_dest_path_list);

	/* and finally, cleanup */
	free_path_list(&new_abs_virtual_link_dest_path_list);
}

/* ========== Mapping & path resolution, internal implementation: ========== */

/* path_list is relative in the beginning;
 * returns -1 ir error,
 * or 0 if OK and path_list has been converted to absolute.
*/
static int relative_virtual_path_to_abs_path(
	const path_mapping_context_t *ctx,
	char *host_cwd,
	size_t host_cwd_size,
	struct path_entry_list *path_list)
{
	struct lua_instance	*luaif = ctx->pmc_luaif;
	char *virtual_reversed_cwd = NULL;
	struct path_entry	*cwd_entries;
	int			cwd_flags;

	if (!getcwd_nomap_nolog(host_cwd, host_cwd_size)) {
		/* getcwd() returns NULL if the path is really long.
		 * In this case the path can not be mapped.
		 *
		 * Added 2009-01-16: This actually happens sometimes;
		 * there are configure scripts that find out MAX_PATH 
		 * the hard way. So, if this process has already
		 * logged this error, we'll suppress further messages.
		 * [decided to add this check after I had seen 3686
		 * error messages from "conftest" :-) /LTA]
		*/
		static int absolute_path_failed_message_logged = 0;

		if (!absolute_path_failed_message_logged) {
			absolute_path_failed_message_logged = 1;
			SB_LOG(SB_LOGLEVEL_ERROR,
			    "absolute_path failed to get current dir");
		}
		return(-1);
	}
	SB_LOG(SB_LOGLEVEL_DEBUG,
		"sbox_map_path_internal: converting to abs.path cwd=%s",
		host_cwd);
	
	/* reversing of paths is expensive...try if a previous
	 * result can be used, and call the reversing logic only if
	 * CWD has been changed.
	*/
	if (luaif->host_cwd && luaif->virtual_reversed_cwd &&
	    !strcmp(host_cwd, luaif->host_cwd)) {
		/* "cache hit" */
		virtual_reversed_cwd = luaif->virtual_reversed_cwd;
		SB_LOG(SB_LOGLEVEL_DEBUG,
			"sbox_map_path_internal: using cached rev_cwd=%s",
			virtual_reversed_cwd);
	} else {
		/* "cache miss" */
		if ( (host_cwd[1]=='\0') && (*host_cwd=='/') ) {
			SB_LOG(SB_LOGLEVEL_DEBUG,
				"sbox_map_path_internal: no need to reverse, '/' is always '/'");
			/* reversed "/" is always "/" */
			virtual_reversed_cwd = strdup(host_cwd);
		} else {
			SB_LOG(SB_LOGLEVEL_DEBUG,
				"sbox_map_path_internal: reversing cwd(%s)", host_cwd);
			virtual_reversed_cwd = call_lua_function_sbox_reverse_path(
				ctx, host_cwd);
			if (virtual_reversed_cwd == NULL) {
				/*
				 * In case reverse path couldn't be resolved
				 * we fallback into host_cwd.  This is the
				 * way it used to work before.
				 */
				virtual_reversed_cwd = strdup(host_cwd);
				SB_LOG(SB_LOGLEVEL_DEBUG,
				    "unable to reverse, using reversed_cwd=%s",
				    virtual_reversed_cwd);
			}
		}
		/* put the reversed CWD to our one-slot cache: */
		if (luaif->host_cwd) free(luaif->host_cwd);
		if (luaif->virtual_reversed_cwd) free(luaif->virtual_reversed_cwd);
		luaif->host_cwd = strdup(host_cwd);
		luaif->virtual_reversed_cwd = virtual_reversed_cwd;
	}
	cwd_entries = split_path_to_path_entries(virtual_reversed_cwd, &cwd_flags);
	/* getcwd() always returns a real path. Assume that the
	 * reversed path is also real (if it isn't, then the reversing
	 * rules are buggy! the bug isn't here in that case!)
	*/
	set_flags_in_path_entries(cwd_entries, PATH_FLAGS_NOT_SYMLINK);
	path_list->pl_first = append_path_entries(
		cwd_entries, path_list->pl_first);
	path_list->pl_flags |= cwd_flags;
#if 0	
	SB_LOG(SB_LOGLEVEL_DEBUG,
		"sbox_map_path_internal: abs.path is '%s'",
		*abs_virtual_path_buffer_p);
#endif
	return(0);
}

/* make sure to use disable_mapping(m); 
 * to prevent recursive calls to this function.
 * Returns a pointer to an allocated buffer which contains the result.
 */
static void sbox_map_path_internal(
	const char *binary_name,
	const char *func_name,
	const char *virtual_orig_path,
	int dont_resolve_final_symlink,
	int process_path_for_exec,
	mapping_results_t *res)
{
	char *mapping_result = NULL;
	path_mapping_context_t	ctx;
	char host_cwd[PATH_MAX + 1]; /* used only if virtual_orig_path is relative */
	struct path_entry_list	abs_virtual_path_for_rule_selection_list;

	clear_path_entry_list(&abs_virtual_path_for_rule_selection_list);
	clear_path_mapping_context(&ctx);
	ctx.pmc_binary_name = binary_name;
	ctx.pmc_func_name = func_name;
	ctx.pmc_virtual_orig_path = virtual_orig_path;
	ctx.pmc_dont_resolve_final_symlink = dont_resolve_final_symlink;

	SB_LOG(SB_LOGLEVEL_DEBUG, "sbox_map_path_internal: %s(%s)", func_name, virtual_orig_path);

#ifdef EXTREME_DEBUGGING
	#define SIZE 100
	void *buffer[SIZE];
	char **strings;
	int i, nptrs;

	nptrs = backtrace(buffer, SIZE);
	strings = backtrace_symbols(buffer, nptrs);
	for (i = 0; i < nptrs; i++)
		SB_LOG(SB_LOGLEVEL_DEBUG, "%s\n", strings[i]);
	free(strings);
#endif
	if (!virtual_orig_path || !*virtual_orig_path) {
		/* an empty path shall always remain empty */
		res->mres_result_buf = res->mres_result_path = strdup("");
		return;
	}

	if (getenv("SBOX_DISABLE_MAPPING")) {
		/* NOTE: Following SB_LOG() call is used by the log
		 *       postprocessor script "sb2logz". Do not change
		 *       without making a corresponding change to the script!
		*/
		SB_LOG(SB_LOGLEVEL_INFO, "disabled(E): %s '%s'",
			func_name, virtual_orig_path);
		goto use_orig_path_as_result_and_exit;
	}

	ctx.pmc_luaif = get_lua();
	if (!ctx.pmc_luaif) {
		/* init in progress? */
		goto use_orig_path_as_result_and_exit;
	}
	if (ctx.pmc_luaif->mapping_disabled) {
		/* NOTE: Following SB_LOG() call is used by the log
		 *       postprocessor script "sb2logz". Do not change
		 *       without making a corresponding change to the script!
		*/
		SB_LOG(SB_LOGLEVEL_INFO, "disabled(%d): %s '%s'",
			ctx.pmc_luaif->mapping_disabled, func_name, virtual_orig_path);
		goto use_orig_path_as_result_and_exit;
	}

	split_path_to_path_list(virtual_orig_path,
		&abs_virtual_path_for_rule_selection_list);

	/* Going to map it. The mapping logic must get clean absolute paths: */
	if (*virtual_orig_path != '/') {
		/* convert to absolute path. */
		if (relative_virtual_path_to_abs_path(
			&ctx, host_cwd, sizeof(host_cwd),
			&abs_virtual_path_for_rule_selection_list) < 0)
			goto use_orig_path_as_result_and_exit;

		/* return the virtual cwd to the caller. It is needed
		 * at least if the mapped path must be registered to
		 * the fdpathdb. */
		res->mres_virtual_cwd = strdup(ctx.pmc_luaif->virtual_reversed_cwd);
	}

	switch (is_clean_path(&abs_virtual_path_for_rule_selection_list)) {
	case 0: /* clean */
		break;
	case 1: /* . */
		remove_dots_from_path_list(&abs_virtual_path_for_rule_selection_list);
		break;
	case 2: /* .. */
		remove_dots_from_path_list(&abs_virtual_path_for_rule_selection_list);
		clean_dotdots_from_path(&ctx, &abs_virtual_path_for_rule_selection_list);
		break;
	}

	disable_mapping(ctx.pmc_luaif);
	{
		/* Mapping disabled inside this block - do not use "return"!! */
		mapping_results_t	resolved_virtual_path_res;

		clear_mapping_results_struct(&resolved_virtual_path_res);

		if (!(abs_virtual_path_for_rule_selection_list.pl_flags &
				PATH_FLAGS_ABSOLUTE)) {
			mapping_result = path_list_to_string(
				&abs_virtual_path_for_rule_selection_list);
			SB_LOG(SB_LOGLEVEL_ERROR,
				"sbox_map_path_internal: "
				"conversion to absolute path failed "
				"(can't map '%s')", mapping_result);
			if (process_path_for_exec) {
				/* can't map, but still need to leave "rule"
				 * (string) and "policy" (nil) to the stack */
				lua_pushstring(ctx.pmc_luaif->lua,
					"mapping failed (failed to make it absolute)");
				lua_pushnil(ctx.pmc_luaif->lua);
			}
			goto forget_mapping;
		}

		if (SB_LOG_IS_ACTIVE(SB_LOGLEVEL_DEBUG)) {
			char *tmp_path = path_list_to_string(
				&abs_virtual_path_for_rule_selection_list);
			SB_LOG(SB_LOGLEVEL_DEBUG,
				"sbox_map_path_internal: process '%s', n='%s'",
				virtual_orig_path, tmp_path);
			free(tmp_path);
		}

		/* sb_path_resolution() leaves the rule to the stack... */
		sb_path_resolution(&ctx, &resolved_virtual_path_res, 0,
			&abs_virtual_path_for_rule_selection_list);

		if (resolved_virtual_path_res.mres_errno) {
			SB_LOG(SB_LOGLEVEL_DEBUG,
				"path mapping fails, "
				" errno = %d",
				resolved_virtual_path_res.mres_errno);
			res->mres_errno = resolved_virtual_path_res.mres_errno;
			goto forget_mapping;
		}

		if (!resolved_virtual_path_res.mres_result_path) {
			SB_LOG(SB_LOGLEVEL_ERROR,
				"sbox_map_path_internal: "
				"path resolution failed [%s]",
				func_name);
			mapping_result = NULL;
			if (process_path_for_exec) {
				/* can't map, but still need to leave "rule"
				 * (string) and "policy" (nil) to the stack */
				lua_pushstring(ctx.pmc_luaif->lua, 
					"mapping failed (path resolution path failed)");
				lua_pushnil(ctx.pmc_luaif->lua);
			}
		} else {
			int	flags;

			SB_LOG(SB_LOGLEVEL_NOISE2,
				"sbox_map_path_internal: resolved_virtua='%s'",
				resolved_virtual_path_res.mres_result_path);

			mapping_result = call_lua_function_sbox_translate_path(
				&ctx, SB_LOGLEVEL_INFO,
				resolved_virtual_path_res.mres_result_path, &flags);
			res->mres_readonly = (flags & SB2_MAPPING_RULE_FLAGS_READONLY);

			if (process_path_for_exec == 0) {
				/* ...and remove rule and policy from stack */
				drop_policy_from_lua_stack(ctx.pmc_luaif);
				drop_rule_from_lua_stack(ctx.pmc_luaif);
			}
		}
	forget_mapping:
		free_mapping_results(&resolved_virtual_path_res);
	}
	enable_mapping(ctx.pmc_luaif);

	free_path_list(&abs_virtual_path_for_rule_selection_list);

	res->mres_result_buf = res->mres_result_path = mapping_result;

	/* now "mapping_result" is an absolute path.
	 * sb2's exec logic needs absolute paths, and absolute paths are also
	 * needed when registering paths to the "fdpathdb".  but otherwise,
	 * we'll try to return a relative path if the original path was
	 * relative (abs.path is still available in mres_result_buf).
	*/
	if ((process_path_for_exec == 0) &&
	    (virtual_orig_path[0] != '/') &&
	    mapping_result &&
	    (*mapping_result == '/')) {
		/* virtual_orig_path was relative. host_cwd has been filled above */
		int	host_cwd_len = strlen(host_cwd);
		int	result_len = strlen(mapping_result);

		if ((result_len == host_cwd_len) &&
		    !strcmp(host_cwd, mapping_result)) {
			SB_LOG(SB_LOGLEVEL_DEBUG,
				"sbox_map_path_internal: result==CWD");
			res->mres_result_path = strdup(".");
			res->mres_result_path_was_allocated = 1;
		} else if ((result_len > host_cwd_len) &&
			   (mapping_result[host_cwd_len] == '/') &&
			   (mapping_result[host_cwd_len+1] != '/') &&
			   (mapping_result[host_cwd_len+1] != '\0') &&
			   !strncmp(host_cwd, mapping_result, host_cwd_len)) {
			/* host_cwd is a prefix of result; convert result
			 * back to a relative path
			*/
			char *relative_result = mapping_result+host_cwd_len+1;
			res->mres_result_path = relative_result;
			SB_LOG(SB_LOGLEVEL_DEBUG,
				"sbox_map_path_internal: result==relative (%s) (%s)",
				relative_result, mapping_result);
		}
	}

	SB_LOG(SB_LOGLEVEL_NOISE, "sbox_map_path_internal: mapping_result='%s'",
		mapping_result ? mapping_result : "<No result>");
	release_lua(ctx.pmc_luaif);
	return;

    use_orig_path_as_result_and_exit:
	if(ctx.pmc_luaif) release_lua(ctx.pmc_luaif);
	res->mres_result_buf = res->mres_result_path = strdup(virtual_orig_path);
	return;
}

/* ========== Public interfaces to the mapping & resolution code: ========== */

void sbox_map_path_for_sb2show(
	const char *binary_name,
	const char *func_name,
	const char *virtual_path,
	mapping_results_t *res)
{
	if (!virtual_path) {
		res->mres_result_buf = res->mres_result_path = NULL;
		res->mres_readonly = 1;
	} else {
		sbox_map_path_internal(binary_name, func_name, virtual_path,
			0/*dont_resolve_final_symlink*/, 0, res);
	}
}

void sbox_map_path(
	const char *func_name,
	const char *virtual_path,
	int dont_resolve_final_symlink,
	mapping_results_t *res)
{
	if (!virtual_path) {
		res->mres_result_buf = res->mres_result_path = NULL;
		res->mres_readonly = 1;
	} else {
		sbox_map_path_internal(
			(sbox_binary_name ? sbox_binary_name : "UNKNOWN"),
			func_name, virtual_path, dont_resolve_final_symlink, 0, res);
	}
}

void sbox_map_path_at(
	const char *func_name,
	int dirfd,
	const char *virtual_path,
	int dont_resolve_final_symlink,
	mapping_results_t *res)
{
	const char *dirfd_path;

	if (!virtual_path) {
		res->mres_result_buf = res->mres_result_path = NULL;
		res->mres_readonly = 1;
		return;
	}

	if ((*virtual_path == '/')
#ifdef AT_FDCWD
	    || (dirfd == AT_FDCWD)
#endif
	   ) {
		/* same as sbox_map_path() */
		sbox_map_path_internal(
			(sbox_binary_name ? sbox_binary_name : "UNKNOWN"),
			func_name,
			virtual_path, dont_resolve_final_symlink, 0, res);
		return;
	}

	/* relative to something else than CWD */
	dirfd_path = fdpathdb_find_path(dirfd);

	if (dirfd_path) {
		/* pathname found */
		char *virtual_abs_path_at_fd = NULL;

		if (asprintf(&virtual_abs_path_at_fd, "%s/%s", dirfd_path, virtual_path) < 0) {
			/* asprintf failed */
			abort();
		}
		SB_LOG(SB_LOGLEVEL_DEBUG,
			"Synthetic path for %s(%d,'%s') => '%s'",
			func_name, dirfd, virtual_path, virtual_abs_path_at_fd);

		sbox_map_path_internal(
			(sbox_binary_name ? sbox_binary_name : "UNKNOWN"),
			func_name,
			virtual_abs_path_at_fd, dont_resolve_final_symlink, 0, res);
		free(virtual_abs_path_at_fd);

		return;
	}

	/* name not found. Can't do much here, log a warning and return
	 * the original relative path. That will work if we are lucky, but
	 * not always..  */
	SB_LOG(SB_LOGLEVEL_WARNING, "Path not found for FD %d, for %s(%s)",
		dirfd, func_name, virtual_path);
	res->mres_result_buf = res->mres_result_path = strdup(virtual_path);
	res->mres_readonly = 0;
}

/* this maps the path and then leaves "rule" and "exec_policy" to the stack, 
 * because exec post-processing needs them
*/
void sbox_map_path_for_exec(
	const char *func_name,
	const char *virtual_path,
	mapping_results_t *res)
{
	sbox_map_path_internal(
		(sbox_binary_name ? sbox_binary_name : "UNKNOWN"), func_name,
		virtual_path, 0/*dont_resolve_final_symlink*/, 1/*exec mode*/, res);
}

char *scratchbox_reverse_path(
	const char *func_name,
	const char *abs_host_path)
{
	char *virtual_path;
	path_mapping_context_t	ctx;

	clear_path_mapping_context(&ctx);
	ctx.pmc_binary_name = (sbox_binary_name ? sbox_binary_name : "UNKNOWN");
	ctx.pmc_func_name = func_name;
	ctx.pmc_virtual_orig_path = "";
	ctx.pmc_dont_resolve_final_symlink = 0;
	ctx.pmc_luaif = get_lua();

	virtual_path = call_lua_function_sbox_reverse_path(&ctx, abs_host_path);
	release_lua(ctx.pmc_luaif);
	return(virtual_path);
}

void	clear_mapping_results_struct(mapping_results_t *res)
{
	res->mres_result_buf = res->mres_result_path = NULL;
	res->mres_readonly = 0;
	res->mres_result_path_was_allocated = 0;
	res->mres_errno = 0;
	res->mres_virtual_cwd = NULL;
}

void	free_mapping_results(mapping_results_t *res)
{
	if (res->mres_result_buf) free(res->mres_result_buf);
	if (res->mres_result_path_was_allocated && res->mres_result_path)
		free(res->mres_result_path);
	if (res->mres_virtual_cwd) free(res->mres_virtual_cwd);
	clear_mapping_results_struct(res);
}

