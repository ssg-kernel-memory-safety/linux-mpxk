/*
 * scripts/gcc-plugins/mpxk.c
 *
 * Author: Hans Liljestrand <LiljestrandH@gmail.com>
 * Copyright: Secure Systems Group, Aalto University, ssg.aalto.fi
 *
 * This file is released under the GPLv2.
 */
#include "mpxk.h"
#include <tree-chkp.h>
#include <ipa-chkp.h>

static void mpxk_plugin_finish(void *gcc_data, void *user_data);
static tree get_load_fndecl(void);

struct mpxk_bound_store_stats mpxk_stats = {
	.dropped_ldx = 0,
	.dropped_stx = 0,
	.dropped_stx_brute = 0,
	.wrappers_added = 0,
	.sweep_ldx = 0,
	.cfun_ldx = 0,
	.sweep_stx = 0
};

#ifndef __visible
#define __visible
#endif
__visible int plugin_is_GPL_compatible;

static struct plugin_info mpxk_plugin_info = {
	.version	= "20170308",
	.help		= "MPX support for kernel space\n"
};

__visible int plugin_init(
		struct plugin_name_args *plugin_info,
		struct plugin_gcc_version *version)
{
	const char * const plugin_name = plugin_info->base_name;

	if (!plugin_default_version_check(version, &gcc_version)) {
		error(G_("incompatible gcc/plugin versions"));
		return 1;
	}

	/* First run some sanity checks... */
	mpxk_builitins_sanity_check();

	/* Register some generic plugin callbacks */
	register_callback(plugin_name, PLUGIN_INFO, NULL, &mpxk_plugin_info);
	register_callback(plugin_name, PLUGIN_FINISH,
			  &mpxk_plugin_finish, NULL);

	/* Insert the specialized MPXK passes */

	/* Replace wrappables with mpxk_wrappers. */
	register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
			  get_mpxk_wrappers_pass_info());

	/* Remove bndldx/bndstx calls.*/
	register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
			  get_mpxk_bnd_store_pass_info());

	/* Handle incoming bounds arguments. */
	register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
			  get_mpxk_cfun_args_pass_info());

	/* Brute force removal of all BNDSTX/BNDLDX instructions */
	register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
			  get_mpxk_sweeper_pass_info());

	return 0;
}

/**
 * mpxk_plugin_finish - display some debug data on plugin finish.
 */
static void mpxk_plugin_finish(void *gcc_data, void *user_data)
{
	(void) gcc_data;
	(void) user_data;

#ifdef MPXK_DEBUG
	expanded_location loc = expand_location(input_location);

	fprintf(stderr,
		"SUMMARY:bndstx[%d+%d=>%d],bndldx[%d+%d=>%d],wraps[%d](%s)\n",
		mpxk_stats.dropped_stx, mpxk_stats.dropped_stx_brute,
		mpxk_stats.sweep_stx, mpxk_stats.dropped_ldx,
		mpxk_stats.cfun_ldx, mpxk_stats.sweep_ldx,
		mpxk_stats.wrappers_added, loc.file);
#endif
}

bool skip_execute(const char *attr)
{
	if (attr != NULL && lookup_attribute(attr,
				DECL_ATTRIBUTES(cfun->decl)) != NULL)
		return true;
	return false;
}

bool cfun_has_attribute(const char *attr)
{
	return (attr != NULL && lookup_attribute(attr,
				DECL_ATTRIBUTES(cfun->decl)) != NULL);
}

void cfun_add_attribute(const char *new_attr)
{
	tree list = DECL_ATTRIBUTES(cfun->decl);
	size_t len;
	tree value;

	if (cfun_has_attribute(new_attr))
		return;

	DECL_ATTRIBUTES(cfun->decl) = tree_cons(get_identifier(new_attr),
			build_tree_list(NULL_TREE, value),
			DECL_ATTRIBUTES(cfun->decl));
}

void cfun_dump_attributes(void)
{
	tree list = DECL_ATTRIBUTES(cfun->decl);

	if (list == NULL)
		return;

	while (list) {
		fprintf(stderr, "%s -> %s\n",
			DECL_NAME_POINTER(current_function_decl),
			IDENTIFIER_POINTER(get_attribute_name(list)));

		list = TREE_CHAIN (list);
	}
}

/**
 * insert_mpxk_bound_load - insert all to the MPXK bound function and bndret.
 * @gsi: The gimple_stmt_iterator for the insert location.
 * @pointer: The pointer based on which the bounds are loaded.
 * @bounds: The bounds variable for storing the loaded bounds.
 *
 * This inserts two GIMPLE statements, a gcall to the bounds load function and
 * a subsequent bndret to store the bounds. The iterator is left on the latter
 * bndret stmt.
 */
void insert_mpxk_bound_load(
		gimple_stmt_iterator *gsi, tree pointer, tree bounds)
{
	tree load_fndecl, tmp_ptr;
	gcall *load_call;

	/* Prepare mpxk load_bounds call => tmp_ptr = load_call(pointer) */
	load_fndecl = get_load_fndecl();
	load_call = gimple_build_call(load_fndecl, 1, pointer);

	/* We need the returned pointer for the latter bndret call */
	tmp_ptr = create_tmp_var(ptr_type_node, "tmp_ptr");
	gimple_call_set_lhs(load_call, tmp_ptr);

	/* Set chkp instrumentation stuff */
	gimple_call_set_with_bounds(load_call, true);
	gimple_set_plf(load_call, GF_PLF_1, false);

	/* Some error checking */
	gcc_assert((load_call->subcode & GF_CALL_INTERNAL) == 0);
	gcc_assert((load_call->subcode & GF_CALL_WITH_BOUNDS) != 0);

	gsi_insert_before(gsi, load_call, GSI_NEW_STMT);
	chkp_insert_retbnd_call(bounds, tmp_ptr, gsi);

	chkp_set_bounds(pointer, bounds);
	gimple_call_with_bounds_p(load_call);

	update_stmt(load_call);
}

/**
 * get_load_fndecl - Return fndecl for the MPXK bounds load function
 */
static tree get_load_fndecl(void)
{
	tree parm_type_list, fntype, fndecl;

	parm_type_list = build_tree_list_stat(NULL_TREE, ptr_type_node);

	fntype = build_function_type(ptr_type_node, parm_type_list);
	fntype = chkp_copy_function_type_adding_bounds(fntype);

	fndecl = build_decl(UNKNOWN_LOCATION, FUNCTION_DECL,
			get_identifier(MPXK_LOAD_BOUNDS_FN_NAME), fntype);

	DECL_EXTERNAL(fndecl) = 1;

	DECL_RESULT(fndecl) = build_decl(UNKNOWN_LOCATION, RESULT_DECL,
			NULL_TREE, ptr_type_node);

	DECL_ATTRIBUTES(fndecl) = tree_cons(get_identifier("bnd_legacy"),
			NULL, DECL_ATTRIBUTES(fndecl));

	TREE_PUBLIC(DECL_RESULT(fndecl)) = 1;

	return fndecl;
}
