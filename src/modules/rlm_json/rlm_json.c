/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_json.c
 * @brief Parses JSON responses
 *
 * @author Arran Cudbard-Bell
 * @author Matthew Newton
 *
 * @copyright 2015 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2015,2020 Network RADIUS SARL (legal@networkradius.com)
 * @copyright 2015 The FreeRADIUS Server Project
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module.h>
#include <freeradius-devel/server/map_proc.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/json/base.h>

#include <ctype.h>

#ifndef HAVE_JSON
#  error "rlm_json should not be built unless json-c is available"
#endif

static fr_sbuff_parse_rules_t const json_arg_parse_rules = {
	.terminals = &FR_SBUFF_TERMS(
		L("\t"),
		L(" "),
		L("!")
	)
};

/** rlm_json module instance
 *
 */
typedef struct {
	fr_json_format_t	*format;
} rlm_json_t;


static CONF_PARSER const module_config[] = {
	{ FR_CONF_OFFSET("encode", FR_TYPE_SUBSECTION, rlm_json_t, format),
	  .subcs_size = sizeof(fr_json_format_t), .subcs_type = "fr_json_format_t",
	  .subcs = (void const *) fr_json_format_config },

	CONF_PARSER_TERMINATOR
};

/** Forms a linked list of jpath head node pointers (a list of jpaths)
 */
typedef struct rlm_json_jpath_cache rlm_json_jpath_cache_t;
struct rlm_json_jpath_cache {
	fr_jpath_node_t		*jpath;		//!< First node in jpath expression.
	rlm_json_jpath_cache_t	*next;		//!< Next jpath cache entry.
};

typedef struct {
	fr_jpath_node_t const	*jpath;
	json_object		*root;
} rlm_json_jpath_to_eval_t;

static xlat_arg_parser_t const json_quote_xlat_arg = {
	.concat = true, .type = FR_TYPE_STRING
};

/** Ensure contents are quoted correctly for a JSON document
 *
 * @ingroup xlat_functions
 *
 */
static xlat_action_t json_quote_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
				     UNUSED xlat_ctx_t const *xctx,
				     request_t *request, fr_value_box_list_t *in)
{
	fr_value_box_t *vb;
	fr_value_box_t *in_head = fr_dlist_head(in);
	char *tmp;

	if (!in_head) return XLAT_ACTION_DONE;	/* Empty input is allowed */

	MEM(vb = fr_value_box_alloc_null(ctx));

	if (!(tmp = fr_json_from_string(vb, in_head->vb_strvalue, false))) {
		REDEBUG("Unable to JSON-quote string");
		talloc_free(vb);
		return XLAT_ACTION_FAIL;
	}
	fr_value_box_bstrdup_buffer_shallow(NULL, vb, NULL, tmp, false);

	fr_dcursor_append(out, vb);

	return XLAT_ACTION_DONE;
}

static xlat_arg_parser_t const jpath_validate_xlat_arg = {
	.required = true, .concat = true, .type = FR_TYPE_STRING
};

/** Determine if a jpath expression is valid
 *
 * @ingroup xlat_functions
 *
 * Writes the output (in the format @verbatim<bytes parsed>[:error]@endverbatim).
 */
static xlat_action_t jpath_validate_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
					 UNUSED xlat_ctx_t const *xctx,
					 request_t *request, fr_value_box_list_t *in)
{
	fr_value_box_t	*path = fr_dlist_head(in);
	fr_jpath_node_t *head;
	ssize_t 	slen;
	char 		*jpath_str;
	fr_value_box_t	*vb;

	MEM(vb = fr_value_box_alloc_null(ctx));

	slen = fr_jpath_parse(request, &head, path->vb_strvalue, path->vb_length);
	if (slen <= 0) {
		fr_value_box_asprintf(ctx, vb, NULL, false, "%zu:%s", -(slen), fr_strerror());
		fr_dcursor_append(out, vb);
		fr_assert(head == NULL);
		return XLAT_ACTION_DONE;
	}
	fr_assert(talloc_get_type_abort(head, fr_jpath_node_t));

	jpath_str = fr_jpath_asprint(request, head);

	fr_value_box_asprintf(ctx, vb, NULL, false, "%zu:%s", (size_t) slen, jpath_str);
	fr_dcursor_append(out, vb);
	talloc_free(head);
	talloc_free(jpath_str);

	return XLAT_ACTION_DONE;
}

static xlat_arg_parser_t const json_encode_xlat_arg = {
	.required = true, .concat = true, .type = FR_TYPE_STRING
};

/** Convert given attributes to a JSON document
 *
 * Usage is `%{json_encode:attr tmpl list}`
 *
 * @ingroup xlat_functions
 */
static xlat_action_t json_encode_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
				      xlat_ctx_t const *xctx,
				      request_t *request, fr_value_box_list_t *in)
{
	rlm_json_t const	*inst = talloc_get_type_abort_const(xctx->mctx->inst->data, rlm_json_t);
	fr_json_format_t const	*format = inst->format;

	ssize_t			slen;
	tmpl_t			*vpt = NULL;
	fr_pair_list_t		json_vps, vps;
	bool			negate;
	char			*json_str = NULL;
	fr_value_box_t		*vb;
	fr_sbuff_t		sbuff;
	fr_value_box_t		*in_head = fr_dlist_head(in);

	fr_pair_list_init(&json_vps);
	fr_pair_list_init(&vps);

	sbuff = FR_SBUFF_IN(in_head->vb_strvalue, in_head->vb_length);
	fr_sbuff_adv_past_whitespace(&sbuff, SIZE_MAX, NULL);

	/*
	 * Iterate through the list of attribute templates in the xlat. For each
	 * one we either add it to the list of attributes for the JSON document
	 * or, if prefixed with '!', remove from the JSON list.
	 */
	while (fr_sbuff_extend(&sbuff)) {
		negate = false;

		/* Check if we should be removing attributes */
		if (fr_sbuff_next_if_char(&sbuff, '!')) negate = true;

		/* Decode next attr template */
		slen = tmpl_afrom_attr_substr(ctx, NULL, &vpt,
					      &sbuff,
					      &json_arg_parse_rules,
					      &(tmpl_attr_rules_t){
					      	.dict_def = request->dict
					      });
		if (slen <= 0) {
			fr_sbuff_set(&sbuff, (size_t)(slen * -1));
			REMARKER(fr_sbuff_start(&sbuff), fr_sbuff_used(&sbuff), "%s", fr_strerror());
		error:
			fr_pair_list_free(&json_vps);
			talloc_free(vpt);
			return XLAT_ACTION_FAIL;
		}

		/*
		 * Get attributes from the template.
		 * Missing attribute isn't an error (so -1, not 0).
		 */
		if (tmpl_copy_pairs(ctx, &vps, request, vpt) < -1) {
			RPEDEBUG("Error copying attributes");
			goto error;
		}

		if (negate) {
			/* Remove all template attributes from JSON list */
			for (fr_pair_t *vp = fr_pair_list_head(&vps);
			     vp;
			     vp = fr_pair_list_next(&vps, vp)) {

				fr_pair_t *vpm = fr_pair_list_head(&json_vps);
				while (vpm) {
					if (vp->da == vpm->da) {
						fr_pair_t *next = fr_pair_list_next(&json_vps, vpm);
						fr_pair_delete(&json_vps, vpm);
						vpm = next;
						continue;
					}
					vpm = fr_pair_list_next(&json_vps, vpm);
				}
			}

			fr_pair_list_free(&vps);
		} else {
			/* Add template VPs to JSON list */
			fr_pair_list_append(&json_vps, &vps);
		}

		TALLOC_FREE(vpt);

		/* Jump forward to next attr */
		fr_sbuff_adv_past_whitespace(&sbuff, SIZE_MAX, NULL);
	}

	/*
	 * Given the list of attributes we now have in json_vps,
	 * convert them into a JSON document and append it to the
	 * return cursor.
	 */
	MEM(vb = fr_value_box_alloc_null(ctx));

	json_str = fr_json_afrom_pair_list(vb, &json_vps, format);
	if (!json_str) {
		REDEBUG("Failed to generate JSON string");
		goto error;
	}
	fr_value_box_bstrdup_buffer_shallow(NULL, vb, NULL, json_str, false);

	fr_dcursor_append(out, vb);
	fr_pair_list_free(&json_vps);

	return XLAT_ACTION_DONE;
}

/** Pre-parse and validate literal jpath expressions for maps
 *
 * @param[in] cs	#CONF_SECTION that defined the map instance.
 * @param[in] mod_inst	module instance (unused).
 * @param[in] proc_inst	the cache structure to fill.
 * @param[in] src	Where to get the JSON data from.
 * @param[in] maps	set of maps to translate to jpaths.
 * @return
 *	- 0 on success.
 * 	- -1 on failure.
 */
static int mod_map_proc_instantiate(CONF_SECTION *cs, UNUSED void *mod_inst, void *proc_inst,
				    tmpl_t const *src, map_list_t const *maps)
{
	rlm_json_jpath_cache_t	*cache_inst = proc_inst;
	map_t const		*map = NULL;
	ssize_t			slen;
	rlm_json_jpath_cache_t	*cache = cache_inst, **tail = &cache->next;

	if (!src) {
		cf_log_err(cs, "Missing JSON source");

		return -1;
	}

	while ((map = map_list_next(maps, map))) {
		CONF_PAIR	*cp = cf_item_to_pair(map->ci);
		char const	*p;

#ifndef HAVE_JSON_OBJECT_GET_INT64
		if (tmpl_is_attr(map->lhs) && (tmpl_da(map->lhs)->type == FR_TYPE_UINT64)) {
			cf_log_err(cp, "64bit integers are not supported by linked json-c.  "
				      "Upgrade to json-c >= 0.10 to use this feature");
			return -1;
		}
#endif

		switch (map->rhs->type) {
		case TMPL_TYPE_UNRESOLVED:
			p = map->rhs->name;
			slen = fr_jpath_parse(cache, &cache->jpath, p, map->rhs->len);
			if (slen <= 0) {
				char		*spaces, *text;

			error:
				fr_canonicalize_error(cache, &spaces, &text, slen, fr_strerror());

				cf_log_err(cp, "Syntax error");
				cf_log_err(cp, "%s", p);
				cf_log_err(cp, "%s^ %s", spaces, text);

				talloc_free(spaces);
				talloc_free(text);
				return -1;
			}
			break;

		case TMPL_TYPE_DATA:
			if (tmpl_value_type(map->rhs) != FR_TYPE_STRING) {
				cf_log_err(cp, "Right side of map must be a string");
				return -1;
			}
			p = tmpl_value(map->rhs)->vb_strvalue;
			slen = fr_jpath_parse(cache, &cache->jpath, p, tmpl_value_length(map->rhs));
			if (slen <= 0) goto error;
			break;

		default:
			continue;
		}

		/*
		 *	Slightly weird... This is here because our first
		 *	list member was pre-allocated and passed to the
		 *	instantiation callback.
		 */
		if (map_list_next(maps, map)) {
			*tail = cache = talloc_zero(cache, rlm_json_jpath_cache_t);
			tail = &cache->next;
		}
	}

	return 0;
}

/** Converts a string value into a #fr_pair_t
 *
 * @param[in,out] ctx to allocate #fr_pair_t (s).
 * @param[out] out where to write the resulting #fr_pair_t.
 * @param[in] request The current request.
 * @param[in] map to process.
 * @param[in] uctx The json tree/jpath expression to evaluate.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int _json_map_proc_get_value(TALLOC_CTX *ctx, fr_pair_list_t *out, request_t *request,
				    map_t const *map, void *uctx)
{
	fr_pair_t			*vp;
	rlm_json_jpath_to_eval_t	*to_eval = uctx;
	fr_value_box_t			*value;
	fr_value_box_list_t		head;
	int				ret;

	fr_pair_list_free(out);
	fr_value_box_list_init(&head);

	ret = fr_jpath_evaluate_leaf(request, &head, tmpl_da(map->lhs)->type, tmpl_da(map->lhs),
			     	     to_eval->root, to_eval->jpath);
	if (ret < 0) {
		RPEDEBUG("Failed evaluating jpath");
		return -1;
	}
	if (ret == 0) return 0;
	fr_assert(!fr_dlist_empty(&head));

	for (value = fr_dlist_head(&head);
	     value;
	     fr_pair_append(out, vp), value = fr_dlist_next(&head, value)) {
		MEM(vp = fr_pair_afrom_da(ctx, tmpl_da(map->lhs)));
		vp->op = map->op;

		if (fr_value_box_steal(vp, &vp->data, value) < 0) {
			RPEDEBUG("Copying data to attribute failed");
			talloc_free(vp);
			fr_pair_list_free(out);
			return -1;
		}
	}

	return 0;
}

/** Parses a JSON string, and executes jpath queries against it to map values to attributes
 *
 * @param mod_inst	unused.
 * @param proc_inst	cached jpath sequences.
 * @param request	The current request.
 * @param json		JSON string to parse.
 * @param maps		Head of the map list.
 * @return
 *	- #RLM_MODULE_NOOP no rows were returned or columns matched.
 *	- #RLM_MODULE_UPDATED if one or more #fr_pair_t were added to the #request_t.
 *	- #RLM_MODULE_FAIL if a fault occurred.
 */
static rlm_rcode_t mod_map_proc(UNUSED void *mod_inst, void *proc_inst, request_t *request,
			      	fr_value_box_list_t *json, map_list_t const *maps)
{
	rlm_rcode_t			rcode = RLM_MODULE_UPDATED;
	struct json_tokener		*tok;

	rlm_json_jpath_cache_t		*cache = proc_inst;
	map_t const			*map = NULL;

	rlm_json_jpath_to_eval_t	to_eval;

	char const			*json_str = NULL;
	fr_value_box_t			*json_head = fr_dlist_head(json);

	if (!json_head) {
		REDEBUG("JSON map input cannot be (null)");
		return RLM_MODULE_FAIL;
	}

	if (fr_value_box_list_concat_in_place(request,
					      json_head, json, FR_TYPE_STRING,
					      FR_VALUE_BOX_LIST_FREE, true,
					      SIZE_MAX) < 0) {
		REDEBUG("Failed concatenating input");
		return RLM_MODULE_FAIL;
	}
	json_str = json_head->vb_strvalue;

	if ((talloc_array_length(json_str) - 1) == 0) {
		REDEBUG("JSON map input length must be > 0");
		return RLM_MODULE_FAIL;
	}

	tok = json_tokener_new();
	to_eval.root = json_tokener_parse_ex(tok, json_str, (int)(talloc_array_length(json_str) - 1));
	if (!to_eval.root) {
		REMARKER(json_str, tok->char_offset, "%s", json_tokener_error_desc(json_tokener_get_error(tok)));
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	while ((map = map_list_next(maps, map))) {
		switch (map->rhs->type) {
		/*
		 *	Cached types
		 */
		case TMPL_TYPE_UNRESOLVED:
		case TMPL_TYPE_DATA:
			to_eval.jpath = cache->jpath;

			if (map_to_request(request, map, _json_map_proc_get_value, &to_eval) < 0) {
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			cache = cache->next;
			break;

		/*
		 *	Dynamic types
		 */
		default:
		{
			ssize_t		slen;
			fr_jpath_node_t	*node;
			char		*to_parse;

			if (tmpl_aexpand(request, &to_parse, request, map->rhs, fr_jpath_escape_func, NULL) < 0) {
				RPERROR("Failed getting jpath data");
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			slen = fr_jpath_parse(request, &node, to_parse, talloc_array_length(to_parse) - 1);
			if (slen <= 0) {
				REMARKER(to_parse, -(slen), "%s", fr_strerror());
				talloc_free(to_parse);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			to_eval.jpath = node;

			if (map_to_request(request, map, _json_map_proc_get_value, &to_eval) < 0) {
				talloc_free(node);
				talloc_free(to_parse);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			talloc_free(node);
		}
			break;
		}
	}


finish:
	json_object_put(to_eval.root);
	json_tokener_free(tok);

	return rcode;
}

static int mod_bootstrap(module_inst_ctx_t const *mctx)
{
	rlm_json_t		*inst = talloc_get_type_abort(mctx->inst->data, rlm_json_t);
	CONF_SECTION		*conf = mctx->inst->conf;
	xlat_t			*xlat;
	char 			*name;
	fr_json_format_t	*format = inst->format;

	xlat = xlat_register_module(inst, mctx, "jsonquote", json_quote_xlat, NULL);
	if (xlat) xlat_func_mono(xlat, &json_quote_xlat_arg);
	xlat = xlat_register_module(inst, mctx, "jpathvalidate", jpath_validate_xlat, NULL);
	if (xlat) xlat_func_mono(xlat, &jpath_validate_xlat_arg);

	name = talloc_asprintf(inst, "%s_encode", mctx->inst->name);
	xlat = xlat_register_module(inst, mctx, name, json_encode_xlat, NULL);
	xlat_func_mono(xlat, &json_encode_xlat_arg);
	talloc_free(name);

	/*
	 *	Check the output format type and warn on unused
	 *	format options
	 */
	format->output_mode = fr_table_value_by_str(fr_json_format_table, format->output_mode_str, JSON_MODE_UNSET);
	if (format->output_mode == JSON_MODE_UNSET) {
		cf_log_err(conf, "output_mode value \"%s\" is invalid", format->output_mode_str);
		return -1;
	}
	fr_json_format_verify(format, true);

	if (map_proc_register(inst, "json", mod_map_proc,
			      mod_map_proc_instantiate, sizeof(rlm_json_jpath_cache_t)) < 0) return -1;
	return 0;
}

static int mod_load(void)
{
	fr_json_version_print();

	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern module_t rlm_json;
module_t rlm_json = {
	.magic		= RLM_MODULE_INIT,
	.name		= "json",
	.type		= RLM_TYPE_THREAD_SAFE,
	.onload		= mod_load,
	.config		= module_config,
	.inst_size	= sizeof(rlm_json_t),
	.bootstrap	= mod_bootstrap,
};
