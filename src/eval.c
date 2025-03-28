/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * eval.c: Expression evaluation.
 */
#define USING_FLOAT_STUFF

#include "vim.h"

#if defined(FEAT_EVAL) || defined(PROTO)

#ifdef VMS
# include <float.h>
#endif

static char *e_missbrac = N_("E111: Missing ']'");
static char *e_dictrange = N_("E719: Cannot use [:] with a Dictionary");
#ifdef FEAT_FLOAT
static char *e_float_as_string = N_("E806: using Float as a String");
#endif
static char *e_nowhitespace = N_("E274: No white space allowed before parenthesis");

#define NAMESPACE_CHAR	(char_u *)"abglstvw"

/*
 * When recursively copying lists and dicts we need to remember which ones we
 * have done to avoid endless recursiveness.  This unique ID is used for that.
 * The last bit is used for previous_funccal, ignored when comparing.
 */
static int current_copyID = 0;

static int echo_attr = 0;   // attributes used for ":echo"

/*
 * Info used by a ":for" loop.
 */
typedef struct
{
    int		fi_semicolon;	// TRUE if ending in '; var]'
    int		fi_varcount;	// nr of variables in the list
    listwatch_T	fi_lw;		// keep an eye on the item used.
    list_T	*fi_list;	// list being used
    int		fi_bi;		// index of blob
    blob_T	*fi_blob;	// blob being used
} forinfo_T;

static int tv_op(typval_T *tv1, typval_T *tv2, char_u  *op);
static int eval2(char_u **arg, typval_T *rettv, int evaluate);
static int eval3(char_u **arg, typval_T *rettv, int evaluate);
static int eval4(char_u **arg, typval_T *rettv, int evaluate);
static int eval5(char_u **arg, typval_T *rettv, int evaluate);
static int eval6(char_u **arg, typval_T *rettv, int evaluate, int want_string);
static int eval7(char_u **arg, typval_T *rettv, int evaluate, int want_string);
static int eval7_leader(typval_T *rettv, char_u *start_leader, char_u **end_leaderp);

static int get_string_tv(char_u **arg, typval_T *rettv, int evaluate);
static int get_lit_string_tv(char_u **arg, typval_T *rettv, int evaluate);
static int free_unref_items(int copyID);
static int get_env_tv(char_u **arg, typval_T *rettv, int evaluate);
static char_u *make_expanded_name(char_u *in_start, char_u *expr_start, char_u *expr_end, char_u *in_end);
static int tv_check_lock(typval_T *tv, char_u *name, int use_gettext);

/*
 * Return "n1" divided by "n2", taking care of dividing by zero.
 */
	varnumber_T
num_divide(varnumber_T n1, varnumber_T n2)
{
    varnumber_T	result;

    if (n2 == 0)	// give an error message?
    {
	if (n1 == 0)
	    result = VARNUM_MIN; // similar to NaN
	else if (n1 < 0)
	    result = -VARNUM_MAX;
	else
	    result = VARNUM_MAX;
    }
    else
	result = n1 / n2;

    return result;
}

/*
 * Return "n1" modulus "n2", taking care of dividing by zero.
 */
	varnumber_T
num_modulus(varnumber_T n1, varnumber_T n2)
{
    // Give an error when n2 is 0?
    return (n2 == 0) ? 0 : (n1 % n2);
}

#if defined(EBCDIC) || defined(PROTO)
/*
 * Compare struct fst by function name.
 */
    static int
compare_func_name(const void *s1, const void *s2)
{
    struct fst *p1 = (struct fst *)s1;
    struct fst *p2 = (struct fst *)s2;

    return STRCMP(p1->f_name, p2->f_name);
}

/*
 * Sort the function table by function name.
 * The sorting of the table above is ASCII dependent.
 * On machines using EBCDIC we have to sort it.
 */
    static void
sortFunctions(void)
{
    int		funcCnt = (int)(sizeof(functions) / sizeof(struct fst)) - 1;

    qsort(functions, (size_t)funcCnt, sizeof(struct fst), compare_func_name);
}
#endif

/*
 * Initialize the global and v: variables.
 */
    void
eval_init(void)
{
    evalvars_init();
    func_init();

#ifdef EBCDIC
    /*
     * Sort the function table, to enable binary search.
     */
    sortFunctions();
#endif
}

#if defined(EXITFREE) || defined(PROTO)
    void
eval_clear(void)
{
    evalvars_clear();

    free_scriptnames();
    free_locales();

    // autoloaded script names
    free_autoload_scriptnames();

    // unreferenced lists and dicts
    (void)garbage_collect(FALSE);

    // functions not garbage collected
    free_all_functions();
}
#endif

/*
 * Top level evaluation function, returning a boolean.
 * Sets "error" to TRUE if there was an error.
 * Return TRUE or FALSE.
 */
    int
eval_to_bool(
    char_u	*arg,
    int		*error,
    char_u	**nextcmd,
    int		skip)	    // only parse, don't execute
{
    typval_T	tv;
    varnumber_T	retval = FALSE;

    if (skip)
	++emsg_skip;
    if (eval0(arg, &tv, nextcmd, !skip) == FAIL)
	*error = TRUE;
    else
    {
	*error = FALSE;
	if (!skip)
	{
	    retval = (tv_get_number_chk(&tv, error) != 0);
	    clear_tv(&tv);
	}
    }
    if (skip)
	--emsg_skip;

    return (int)retval;
}

/*
 * Call eval1() and give an error message if not done at a lower level.
 */
    static int
eval1_emsg(char_u **arg, typval_T *rettv, int evaluate)
{
    char_u	*start = *arg;
    int		ret;
    int		did_emsg_before = did_emsg;
    int		called_emsg_before = called_emsg;

    ret = eval1(arg, rettv, evaluate);
    if (ret == FAIL)
    {
	// Report the invalid expression unless the expression evaluation has
	// been cancelled due to an aborting error, an interrupt, or an
	// exception, or we already gave a more specific error.
	// Also check called_emsg for when using assert_fails().
	if (!aborting() && did_emsg == did_emsg_before
					  && called_emsg == called_emsg_before)
	    semsg(_(e_invexpr2), start);
    }
    return ret;
}

    int
eval_expr_typval(typval_T *expr, typval_T *argv, int argc, typval_T *rettv)
{
    char_u	*s;
    char_u	buf[NUMBUFLEN];
    funcexe_T	funcexe;

    if (expr->v_type == VAR_FUNC)
    {
	s = expr->vval.v_string;
	if (s == NULL || *s == NUL)
	    return FAIL;
	vim_memset(&funcexe, 0, sizeof(funcexe));
	funcexe.evaluate = TRUE;
	if (call_func(s, -1, rettv, argc, argv, &funcexe) == FAIL)
	    return FAIL;
    }
    else if (expr->v_type == VAR_PARTIAL)
    {
	partial_T   *partial = expr->vval.v_partial;

	s = partial_name(partial);
	if (s == NULL || *s == NUL)
	    return FAIL;
	vim_memset(&funcexe, 0, sizeof(funcexe));
	funcexe.evaluate = TRUE;
	funcexe.partial = partial;
	if (call_func(s, -1, rettv, argc, argv, &funcexe) == FAIL)
	    return FAIL;
    }
    else
    {
	s = tv_get_string_buf_chk(expr, buf);
	if (s == NULL)
	    return FAIL;
	s = skipwhite(s);
	if (eval1_emsg(&s, rettv, TRUE) == FAIL)
	    return FAIL;
	if (*s != NUL)  // check for trailing chars after expr
	{
	    clear_tv(rettv);
	    semsg(_(e_invexpr2), s);
	    return FAIL;
	}
    }
    return OK;
}

/*
 * Like eval_to_bool() but using a typval_T instead of a string.
 * Works for string, funcref and partial.
 */
    int
eval_expr_to_bool(typval_T *expr, int *error)
{
    typval_T	rettv;
    int		res;

    if (eval_expr_typval(expr, NULL, 0, &rettv) == FAIL)
    {
	*error = TRUE;
	return FALSE;
    }
    res = (tv_get_number_chk(&rettv, error) != 0);
    clear_tv(&rettv);
    return res;
}

/*
 * Top level evaluation function, returning a string.  If "skip" is TRUE,
 * only parsing to "nextcmd" is done, without reporting errors.  Return
 * pointer to allocated memory, or NULL for failure or when "skip" is TRUE.
 */
    char_u *
eval_to_string_skip(
    char_u	*arg,
    char_u	**nextcmd,
    int		skip)	    // only parse, don't execute
{
    typval_T	tv;
    char_u	*retval;

    if (skip)
	++emsg_skip;
    if (eval0(arg, &tv, nextcmd, !skip) == FAIL || skip)
	retval = NULL;
    else
    {
	retval = vim_strsave(tv_get_string(&tv));
	clear_tv(&tv);
    }
    if (skip)
	--emsg_skip;

    return retval;
}

/*
 * Skip over an expression at "*pp".
 * Return FAIL for an error, OK otherwise.
 */
    int
skip_expr(char_u **pp)
{
    typval_T	rettv;

    *pp = skipwhite(*pp);
    return eval1(pp, &rettv, FALSE);
}

/*
 * Top level evaluation function, returning a string.
 * When "convert" is TRUE convert a List into a sequence of lines and convert
 * a Float to a String.
 * Return pointer to allocated memory, or NULL for failure.
 */
    char_u *
eval_to_string(
    char_u	*arg,
    char_u	**nextcmd,
    int		convert)
{
    typval_T	tv;
    char_u	*retval;
    garray_T	ga;
#ifdef FEAT_FLOAT
    char_u	numbuf[NUMBUFLEN];
#endif

    if (eval0(arg, &tv, nextcmd, TRUE) == FAIL)
	retval = NULL;
    else
    {
	if (convert && tv.v_type == VAR_LIST)
	{
	    ga_init2(&ga, (int)sizeof(char), 80);
	    if (tv.vval.v_list != NULL)
	    {
		list_join(&ga, tv.vval.v_list, (char_u *)"\n", TRUE, FALSE, 0);
		if (tv.vval.v_list->lv_len > 0)
		    ga_append(&ga, NL);
	    }
	    ga_append(&ga, NUL);
	    retval = (char_u *)ga.ga_data;
	}
#ifdef FEAT_FLOAT
	else if (convert && tv.v_type == VAR_FLOAT)
	{
	    vim_snprintf((char *)numbuf, NUMBUFLEN, "%g", tv.vval.v_float);
	    retval = vim_strsave(numbuf);
	}
#endif
	else
	    retval = vim_strsave(tv_get_string(&tv));
	clear_tv(&tv);
    }

    return retval;
}

/*
 * Call eval_to_string() without using current local variables and using
 * textlock.  When "use_sandbox" is TRUE use the sandbox.
 */
    char_u *
eval_to_string_safe(
    char_u	*arg,
    char_u	**nextcmd,
    int		use_sandbox)
{
    char_u	*retval;
    funccal_entry_T funccal_entry;

    save_funccal(&funccal_entry);
    if (use_sandbox)
	++sandbox;
    ++textlock;
    retval = eval_to_string(arg, nextcmd, FALSE);
    if (use_sandbox)
	--sandbox;
    --textlock;
    restore_funccal();
    return retval;
}

/*
 * Top level evaluation function, returning a number.
 * Evaluates "expr" silently.
 * Returns -1 for an error.
 */
    varnumber_T
eval_to_number(char_u *expr)
{
    typval_T	rettv;
    varnumber_T	retval;
    char_u	*p = skipwhite(expr);

    ++emsg_off;

    if (eval1(&p, &rettv, TRUE) == FAIL)
	retval = -1;
    else
    {
	retval = tv_get_number_chk(&rettv, NULL);
	clear_tv(&rettv);
    }
    --emsg_off;

    return retval;
}

/*
 * Top level evaluation function.
 * Returns an allocated typval_T with the result.
 * Returns NULL when there is an error.
 */
    typval_T *
eval_expr(char_u *arg, char_u **nextcmd)
{
    typval_T	*tv;

    tv = ALLOC_ONE(typval_T);
    if (tv != NULL && eval0(arg, tv, nextcmd, TRUE) == FAIL)
	VIM_CLEAR(tv);

    return tv;
}

/*
 * Call some Vim script function and return the result in "*rettv".
 * Uses argv[0] to argv[argc - 1] for the function arguments.  argv[argc]
 * should have type VAR_UNKNOWN.
 * Returns OK or FAIL.
 */
    int
call_vim_function(
    char_u      *func,
    int		argc,
    typval_T	*argv,
    typval_T	*rettv)
{
    int		ret;
    funcexe_T	funcexe;

    rettv->v_type = VAR_UNKNOWN;		// clear_tv() uses this
    vim_memset(&funcexe, 0, sizeof(funcexe));
    funcexe.firstline = curwin->w_cursor.lnum;
    funcexe.lastline = curwin->w_cursor.lnum;
    funcexe.evaluate = TRUE;
    ret = call_func(func, -1, rettv, argc, argv, &funcexe);
    if (ret == FAIL)
	clear_tv(rettv);

    return ret;
}

/*
 * Call Vim script function "func" and return the result as a number.
 * Returns -1 when calling the function fails.
 * Uses argv[0] to argv[argc - 1] for the function arguments. argv[argc] should
 * have type VAR_UNKNOWN.
 */
    varnumber_T
call_func_retnr(
    char_u      *func,
    int		argc,
    typval_T	*argv)
{
    typval_T	rettv;
    varnumber_T	retval;

    if (call_vim_function(func, argc, argv, &rettv) == FAIL)
	return -1;

    retval = tv_get_number_chk(&rettv, NULL);
    clear_tv(&rettv);
    return retval;
}

/*
 * Call Vim script function "func" and return the result as a string.
 * Returns NULL when calling the function fails.
 * Uses argv[0] to argv[argc - 1] for the function arguments. argv[argc] should
 * have type VAR_UNKNOWN.
 */
    void *
call_func_retstr(
    char_u      *func,
    int		argc,
    typval_T	*argv)
{
    typval_T	rettv;
    char_u	*retval;

    if (call_vim_function(func, argc, argv, &rettv) == FAIL)
	return NULL;

    retval = vim_strsave(tv_get_string(&rettv));
    clear_tv(&rettv);
    return retval;
}

/*
 * Call Vim script function "func" and return the result as a List.
 * Uses argv[0] to argv[argc - 1] for the function arguments. argv[argc] should
 * have type VAR_UNKNOWN.
 * Returns NULL when there is something wrong.
 */
    void *
call_func_retlist(
    char_u      *func,
    int		argc,
    typval_T	*argv)
{
    typval_T	rettv;

    if (call_vim_function(func, argc, argv, &rettv) == FAIL)
	return NULL;

    if (rettv.v_type != VAR_LIST)
    {
	clear_tv(&rettv);
	return NULL;
    }

    return rettv.vval.v_list;
}

#ifdef FEAT_FOLDING
/*
 * Evaluate 'foldexpr'.  Returns the foldlevel, and any character preceding
 * it in "*cp".  Doesn't give error messages.
 */
    int
eval_foldexpr(char_u *arg, int *cp)
{
    typval_T	tv;
    varnumber_T	retval;
    char_u	*s;
    int		use_sandbox = was_set_insecurely((char_u *)"foldexpr",
								   OPT_LOCAL);

    ++emsg_off;
    if (use_sandbox)
	++sandbox;
    ++textlock;
    *cp = NUL;
    if (eval0(arg, &tv, NULL, TRUE) == FAIL)
	retval = 0;
    else
    {
	// If the result is a number, just return the number.
	if (tv.v_type == VAR_NUMBER)
	    retval = tv.vval.v_number;
	else if (tv.v_type != VAR_STRING || tv.vval.v_string == NULL)
	    retval = 0;
	else
	{
	    // If the result is a string, check if there is a non-digit before
	    // the number.
	    s = tv.vval.v_string;
	    if (!VIM_ISDIGIT(*s) && *s != '-')
		*cp = *s++;
	    retval = atol((char *)s);
	}
	clear_tv(&tv);
    }
    --emsg_off;
    if (use_sandbox)
	--sandbox;
    --textlock;

    return (int)retval;
}
#endif

/*
 * Get an lval: variable, Dict item or List item that can be assigned a value
 * to: "name", "na{me}", "name[expr]", "name[expr:expr]", "name[expr][expr]",
 * "name.key", "name.key[expr]" etc.
 * Indexing only works if "name" is an existing List or Dictionary.
 * "name" points to the start of the name.
 * If "rettv" is not NULL it points to the value to be assigned.
 * "unlet" is TRUE for ":unlet": slightly different behavior when something is
 * wrong; must end in space or cmd separator.
 *
 * flags:
 *  GLV_QUIET:       do not give error messages
 *  GLV_READ_ONLY:   will not change the variable
 *  GLV_NO_AUTOLOAD: do not use script autoloading
 *
 * Returns a pointer to just after the name, including indexes.
 * When an evaluation error occurs "lp->ll_name" is NULL;
 * Returns NULL for a parsing error.  Still need to free items in "lp"!
 */
    char_u *
get_lval(
    char_u	*name,
    typval_T	*rettv,
    lval_T	*lp,
    int		unlet,
    int		skip,
    int		flags,	    // GLV_ values
    int		fne_flags)  // flags for find_name_end()
{
    char_u	*p;
    char_u	*expr_start, *expr_end;
    int		cc;
    dictitem_T	*v;
    typval_T	var1;
    typval_T	var2;
    int		empty1 = FALSE;
    listitem_T	*ni;
    char_u	*key = NULL;
    int		len;
    hashtab_T	*ht;
    int		quiet = flags & GLV_QUIET;

    // Clear everything in "lp".
    vim_memset(lp, 0, sizeof(lval_T));

    if (skip)
    {
	// When skipping just find the end of the name.
	lp->ll_name = name;
	return find_name_end(name, NULL, NULL, FNE_INCL_BR | fne_flags);
    }

    // Find the end of the name.
    p = find_name_end(name, &expr_start, &expr_end, fne_flags);
    if (expr_start != NULL)
    {
	// Don't expand the name when we already know there is an error.
	if (unlet && !VIM_ISWHITE(*p) && !ends_excmd(*p)
						    && *p != '[' && *p != '.')
	{
	    emsg(_(e_trailing));
	    return NULL;
	}

	lp->ll_exp_name = make_expanded_name(name, expr_start, expr_end, p);
	if (lp->ll_exp_name == NULL)
	{
	    // Report an invalid expression in braces, unless the
	    // expression evaluation has been cancelled due to an
	    // aborting error, an interrupt, or an exception.
	    if (!aborting() && !quiet)
	    {
		emsg_severe = TRUE;
		semsg(_(e_invarg2), name);
		return NULL;
	    }
	}
	lp->ll_name = lp->ll_exp_name;
    }
    else
	lp->ll_name = name;

    // Without [idx] or .key we are done.
    if ((*p != '[' && *p != '.') || lp->ll_name == NULL)
	return p;

    cc = *p;
    *p = NUL;
    // Only pass &ht when we would write to the variable, it prevents autoload
    // as well.
    v = find_var(lp->ll_name, (flags & GLV_READ_ONLY) ? NULL : &ht,
						      flags & GLV_NO_AUTOLOAD);
    if (v == NULL && !quiet)
	semsg(_(e_undefvar), lp->ll_name);
    *p = cc;
    if (v == NULL)
	return NULL;

    /*
     * Loop until no more [idx] or .key is following.
     */
    lp->ll_tv = &v->di_tv;
    var1.v_type = VAR_UNKNOWN;
    var2.v_type = VAR_UNKNOWN;
    while (*p == '[' || (*p == '.' && lp->ll_tv->v_type == VAR_DICT))
    {
	if (!(lp->ll_tv->v_type == VAR_LIST && lp->ll_tv->vval.v_list != NULL)
		&& !(lp->ll_tv->v_type == VAR_DICT
					   && lp->ll_tv->vval.v_dict != NULL)
		&& !(lp->ll_tv->v_type == VAR_BLOB
					   && lp->ll_tv->vval.v_blob != NULL))
	{
	    if (!quiet)
		emsg(_("E689: Can only index a List, Dictionary or Blob"));
	    return NULL;
	}
	if (lp->ll_range)
	{
	    if (!quiet)
		emsg(_("E708: [:] must come last"));
	    return NULL;
	}

	len = -1;
	if (*p == '.')
	{
	    key = p + 1;
	    for (len = 0; ASCII_ISALNUM(key[len]) || key[len] == '_'; ++len)
		;
	    if (len == 0)
	    {
		if (!quiet)
		    emsg(_(e_emptykey));
		return NULL;
	    }
	    p = key + len;
	}
	else
	{
	    // Get the index [expr] or the first index [expr: ].
	    p = skipwhite(p + 1);
	    if (*p == ':')
		empty1 = TRUE;
	    else
	    {
		empty1 = FALSE;
		if (eval1(&p, &var1, TRUE) == FAIL)	// recursive!
		    return NULL;
		if (tv_get_string_chk(&var1) == NULL)
		{
		    // not a number or string
		    clear_tv(&var1);
		    return NULL;
		}
	    }

	    // Optionally get the second index [ :expr].
	    if (*p == ':')
	    {
		if (lp->ll_tv->v_type == VAR_DICT)
		{
		    if (!quiet)
			emsg(_(e_dictrange));
		    clear_tv(&var1);
		    return NULL;
		}
		if (rettv != NULL
			&& !(rettv->v_type == VAR_LIST
						 && rettv->vval.v_list != NULL)
			&& !(rettv->v_type == VAR_BLOB
						&& rettv->vval.v_blob != NULL))
		{
		    if (!quiet)
			emsg(_("E709: [:] requires a List or Blob value"));
		    clear_tv(&var1);
		    return NULL;
		}
		p = skipwhite(p + 1);
		if (*p == ']')
		    lp->ll_empty2 = TRUE;
		else
		{
		    lp->ll_empty2 = FALSE;
		    if (eval1(&p, &var2, TRUE) == FAIL)	// recursive!
		    {
			clear_tv(&var1);
			return NULL;
		    }
		    if (tv_get_string_chk(&var2) == NULL)
		    {
			// not a number or string
			clear_tv(&var1);
			clear_tv(&var2);
			return NULL;
		    }
		}
		lp->ll_range = TRUE;
	    }
	    else
		lp->ll_range = FALSE;

	    if (*p != ']')
	    {
		if (!quiet)
		    emsg(_(e_missbrac));
		clear_tv(&var1);
		clear_tv(&var2);
		return NULL;
	    }

	    // Skip to past ']'.
	    ++p;
	}

	if (lp->ll_tv->v_type == VAR_DICT)
	{
	    if (len == -1)
	    {
		// "[key]": get key from "var1"
		key = tv_get_string_chk(&var1);	// is number or string
		if (key == NULL)
		{
		    clear_tv(&var1);
		    return NULL;
		}
	    }
	    lp->ll_list = NULL;
	    lp->ll_dict = lp->ll_tv->vval.v_dict;
	    lp->ll_di = dict_find(lp->ll_dict, key, len);

	    // When assigning to a scope dictionary check that a function and
	    // variable name is valid (only variable name unless it is l: or
	    // g: dictionary). Disallow overwriting a builtin function.
	    if (rettv != NULL && lp->ll_dict->dv_scope != 0)
	    {
		int prevval;
		int wrong;

		if (len != -1)
		{
		    prevval = key[len];
		    key[len] = NUL;
		}
		else
		    prevval = 0; // avoid compiler warning
		wrong = (lp->ll_dict->dv_scope == VAR_DEF_SCOPE
			       && rettv->v_type == VAR_FUNC
			       && var_check_func_name(key, lp->ll_di == NULL))
			|| !valid_varname(key);
		if (len != -1)
		    key[len] = prevval;
		if (wrong)
		    return NULL;
	    }

	    if (lp->ll_di == NULL)
	    {
		// Can't add "v:" or "a:" variable.
		if (lp->ll_dict == get_vimvar_dict()
			 || &lp->ll_dict->dv_hashtab == get_funccal_args_ht())
		{
		    semsg(_(e_illvar), name);
		    clear_tv(&var1);
		    return NULL;
		}

		// Key does not exist in dict: may need to add it.
		if (*p == '[' || *p == '.' || unlet)
		{
		    if (!quiet)
			semsg(_(e_dictkey), key);
		    clear_tv(&var1);
		    return NULL;
		}
		if (len == -1)
		    lp->ll_newkey = vim_strsave(key);
		else
		    lp->ll_newkey = vim_strnsave(key, len);
		clear_tv(&var1);
		if (lp->ll_newkey == NULL)
		    p = NULL;
		break;
	    }
	    // existing variable, need to check if it can be changed
	    else if ((flags & GLV_READ_ONLY) == 0
			     && var_check_ro(lp->ll_di->di_flags, name, FALSE))
	    {
		clear_tv(&var1);
		return NULL;
	    }

	    clear_tv(&var1);
	    lp->ll_tv = &lp->ll_di->di_tv;
	}
	else if (lp->ll_tv->v_type == VAR_BLOB)
	{
	    long bloblen = blob_len(lp->ll_tv->vval.v_blob);

	    /*
	     * Get the number and item for the only or first index of the List.
	     */
	    if (empty1)
		lp->ll_n1 = 0;
	    else
		// is number or string
		lp->ll_n1 = (long)tv_get_number(&var1);
	    clear_tv(&var1);

	    if (lp->ll_n1 < 0
		    || lp->ll_n1 > bloblen
		    || (lp->ll_range && lp->ll_n1 == bloblen))
	    {
		if (!quiet)
		    semsg(_(e_blobidx), lp->ll_n1);
		clear_tv(&var2);
		return NULL;
	    }
	    if (lp->ll_range && !lp->ll_empty2)
	    {
		lp->ll_n2 = (long)tv_get_number(&var2);
		clear_tv(&var2);
		if (lp->ll_n2 < 0
			|| lp->ll_n2 >= bloblen
			|| lp->ll_n2 < lp->ll_n1)
		{
		    if (!quiet)
			semsg(_(e_blobidx), lp->ll_n2);
		    return NULL;
		}
	    }
	    lp->ll_blob = lp->ll_tv->vval.v_blob;
	    lp->ll_tv = NULL;
	    break;
	}
	else
	{
	    /*
	     * Get the number and item for the only or first index of the List.
	     */
	    if (empty1)
		lp->ll_n1 = 0;
	    else
		// is number or string
		lp->ll_n1 = (long)tv_get_number(&var1);
	    clear_tv(&var1);

	    lp->ll_dict = NULL;
	    lp->ll_list = lp->ll_tv->vval.v_list;
	    lp->ll_li = list_find(lp->ll_list, lp->ll_n1);
	    if (lp->ll_li == NULL)
	    {
		if (lp->ll_n1 < 0)
		{
		    lp->ll_n1 = 0;
		    lp->ll_li = list_find(lp->ll_list, lp->ll_n1);
		}
	    }
	    if (lp->ll_li == NULL)
	    {
		clear_tv(&var2);
		if (!quiet)
		    semsg(_(e_listidx), lp->ll_n1);
		return NULL;
	    }

	    /*
	     * May need to find the item or absolute index for the second
	     * index of a range.
	     * When no index given: "lp->ll_empty2" is TRUE.
	     * Otherwise "lp->ll_n2" is set to the second index.
	     */
	    if (lp->ll_range && !lp->ll_empty2)
	    {
		lp->ll_n2 = (long)tv_get_number(&var2);
						    // is number or string
		clear_tv(&var2);
		if (lp->ll_n2 < 0)
		{
		    ni = list_find(lp->ll_list, lp->ll_n2);
		    if (ni == NULL)
		    {
			if (!quiet)
			    semsg(_(e_listidx), lp->ll_n2);
			return NULL;
		    }
		    lp->ll_n2 = list_idx_of_item(lp->ll_list, ni);
		}

		// Check that lp->ll_n2 isn't before lp->ll_n1.
		if (lp->ll_n1 < 0)
		    lp->ll_n1 = list_idx_of_item(lp->ll_list, lp->ll_li);
		if (lp->ll_n2 < lp->ll_n1)
		{
		    if (!quiet)
			semsg(_(e_listidx), lp->ll_n2);
		    return NULL;
		}
	    }

	    lp->ll_tv = &lp->ll_li->li_tv;
	}
    }

    clear_tv(&var1);
    return p;
}

/*
 * Clear lval "lp" that was filled by get_lval().
 */
    void
clear_lval(lval_T *lp)
{
    vim_free(lp->ll_exp_name);
    vim_free(lp->ll_newkey);
}

/*
 * Set a variable that was parsed by get_lval() to "rettv".
 * "endp" points to just after the parsed name.
 * "op" is NULL, "+" for "+=", "-" for "-=", "*" for "*=", "/" for "/=",
 * "%" for "%=", "." for ".=" or "=" for "=".
 */
    void
set_var_lval(
    lval_T	*lp,
    char_u	*endp,
    typval_T	*rettv,
    int		copy,
    int		is_const,    // Disallow to modify existing variable for :const
    char_u	*op)
{
    int		cc;
    listitem_T	*ri;
    dictitem_T	*di;

    if (lp->ll_tv == NULL)
    {
	cc = *endp;
	*endp = NUL;
	if (lp->ll_blob != NULL)
	{
	    int	    error = FALSE, val;

	    if (op != NULL && *op != '=')
	    {
		semsg(_(e_letwrong), op);
		return;
	    }

	    if (lp->ll_range && rettv->v_type == VAR_BLOB)
	    {
		int	il, ir;

		if (lp->ll_empty2)
		    lp->ll_n2 = blob_len(lp->ll_blob) - 1;

		if (lp->ll_n2 - lp->ll_n1 + 1 != blob_len(rettv->vval.v_blob))
		{
		    emsg(_("E972: Blob value does not have the right number of bytes"));
		    return;
		}
		if (lp->ll_empty2)
		    lp->ll_n2 = blob_len(lp->ll_blob);

		ir = 0;
		for (il = lp->ll_n1; il <= lp->ll_n2; il++)
		    blob_set(lp->ll_blob, il,
			    blob_get(rettv->vval.v_blob, ir++));
	    }
	    else
	    {
		val = (int)tv_get_number_chk(rettv, &error);
		if (!error)
		{
		    garray_T *gap = &lp->ll_blob->bv_ga;

		    // Allow for appending a byte.  Setting a byte beyond
		    // the end is an error otherwise.
		    if (lp->ll_n1 < gap->ga_len
			    || (lp->ll_n1 == gap->ga_len
				&& ga_grow(&lp->ll_blob->bv_ga, 1) == OK))
		    {
			blob_set(lp->ll_blob, lp->ll_n1, val);
			if (lp->ll_n1 == gap->ga_len)
			    ++gap->ga_len;
		    }
		    // error for invalid range was already given in get_lval()
		}
	    }
	}
	else if (op != NULL && *op != '=')
	{
	    typval_T tv;

	    if (is_const)
	    {
		emsg(_(e_cannot_mod));
		*endp = cc;
		return;
	    }

	    // handle +=, -=, *=, /=, %= and .=
	    di = NULL;
	    if (get_var_tv(lp->ll_name, (int)STRLEN(lp->ll_name),
					     &tv, &di, TRUE, FALSE) == OK)
	    {
		if ((di == NULL
			 || (!var_check_ro(di->di_flags, lp->ll_name, FALSE)
			   && !tv_check_lock(&di->di_tv, lp->ll_name, FALSE)))
			&& tv_op(&tv, rettv, op) == OK)
		    set_var(lp->ll_name, &tv, FALSE);
		clear_tv(&tv);
	    }
	}
	else
	    set_var_const(lp->ll_name, rettv, copy, is_const);
	*endp = cc;
    }
    else if (var_check_lock(lp->ll_newkey == NULL
		? lp->ll_tv->v_lock
		: lp->ll_tv->vval.v_dict->dv_lock, lp->ll_name, FALSE))
	;
    else if (lp->ll_range)
    {
	listitem_T *ll_li = lp->ll_li;
	int	    ll_n1 = lp->ll_n1;

	if (is_const)
	{
	    emsg(_("E996: Cannot lock a range"));
	    return;
	}

	/*
	 * Check whether any of the list items is locked
	 */
	for (ri = rettv->vval.v_list->lv_first; ri != NULL && ll_li != NULL; )
	{
	    if (var_check_lock(ll_li->li_tv.v_lock, lp->ll_name, FALSE))
		return;
	    ri = ri->li_next;
	    if (ri == NULL || (!lp->ll_empty2 && lp->ll_n2 == ll_n1))
		break;
	    ll_li = ll_li->li_next;
	    ++ll_n1;
	}

	/*
	 * Assign the List values to the list items.
	 */
	for (ri = rettv->vval.v_list->lv_first; ri != NULL; )
	{
	    if (op != NULL && *op != '=')
		tv_op(&lp->ll_li->li_tv, &ri->li_tv, op);
	    else
	    {
		clear_tv(&lp->ll_li->li_tv);
		copy_tv(&ri->li_tv, &lp->ll_li->li_tv);
	    }
	    ri = ri->li_next;
	    if (ri == NULL || (!lp->ll_empty2 && lp->ll_n2 == lp->ll_n1))
		break;
	    if (lp->ll_li->li_next == NULL)
	    {
		// Need to add an empty item.
		if (list_append_number(lp->ll_list, 0) == FAIL)
		{
		    ri = NULL;
		    break;
		}
	    }
	    lp->ll_li = lp->ll_li->li_next;
	    ++lp->ll_n1;
	}
	if (ri != NULL)
	    emsg(_("E710: List value has more items than target"));
	else if (lp->ll_empty2
		? (lp->ll_li != NULL && lp->ll_li->li_next != NULL)
		: lp->ll_n1 != lp->ll_n2)
	    emsg(_("E711: List value has not enough items"));
    }
    else
    {
	/*
	 * Assign to a List or Dictionary item.
	 */
	if (is_const)
	{
	    emsg(_("E996: Cannot lock a list or dict"));
	    return;
	}
	if (lp->ll_newkey != NULL)
	{
	    if (op != NULL && *op != '=')
	    {
		semsg(_(e_letwrong), op);
		return;
	    }

	    // Need to add an item to the Dictionary.
	    di = dictitem_alloc(lp->ll_newkey);
	    if (di == NULL)
		return;
	    if (dict_add(lp->ll_tv->vval.v_dict, di) == FAIL)
	    {
		vim_free(di);
		return;
	    }
	    lp->ll_tv = &di->di_tv;
	}
	else if (op != NULL && *op != '=')
	{
	    tv_op(lp->ll_tv, rettv, op);
	    return;
	}
	else
	    clear_tv(lp->ll_tv);

	/*
	 * Assign the value to the variable or list item.
	 */
	if (copy)
	    copy_tv(rettv, lp->ll_tv);
	else
	{
	    *lp->ll_tv = *rettv;
	    lp->ll_tv->v_lock = 0;
	    init_tv(rettv);
	}
    }
}

/*
 * Handle "tv1 += tv2", "tv1 -= tv2", "tv1 *= tv2", "tv1 /= tv2", "tv1 %= tv2"
 * and "tv1 .= tv2"
 * Returns OK or FAIL.
 */
    static int
tv_op(typval_T *tv1, typval_T *tv2, char_u *op)
{
    varnumber_T	n;
    char_u	numbuf[NUMBUFLEN];
    char_u	*s;

    // Can't do anything with a Funcref, Dict, v:true on the right.
    if (tv2->v_type != VAR_FUNC && tv2->v_type != VAR_DICT
						&& tv2->v_type != VAR_SPECIAL)
    {
	switch (tv1->v_type)
	{
	    case VAR_UNKNOWN:
	    case VAR_DICT:
	    case VAR_FUNC:
	    case VAR_PARTIAL:
	    case VAR_SPECIAL:
	    case VAR_JOB:
	    case VAR_CHANNEL:
		break;

	    case VAR_BLOB:
		if (*op != '+' || tv2->v_type != VAR_BLOB)
		    break;
		// BLOB += BLOB
		if (tv1->vval.v_blob != NULL && tv2->vval.v_blob != NULL)
		{
		    blob_T  *b1 = tv1->vval.v_blob;
		    blob_T  *b2 = tv2->vval.v_blob;
		    int	i, len = blob_len(b2);
		    for (i = 0; i < len; i++)
			ga_append(&b1->bv_ga, blob_get(b2, i));
		}
		return OK;

	    case VAR_LIST:
		if (*op != '+' || tv2->v_type != VAR_LIST)
		    break;
		// List += List
		if (tv1->vval.v_list != NULL && tv2->vval.v_list != NULL)
		    list_extend(tv1->vval.v_list, tv2->vval.v_list, NULL);
		return OK;

	    case VAR_NUMBER:
	    case VAR_STRING:
		if (tv2->v_type == VAR_LIST)
		    break;
		if (vim_strchr((char_u *)"+-*/%", *op) != NULL)
		{
		    // nr += nr , nr -= nr , nr *=nr , nr /= nr , nr %= nr
		    n = tv_get_number(tv1);
#ifdef FEAT_FLOAT
		    if (tv2->v_type == VAR_FLOAT)
		    {
			float_T f = n;

			if (*op == '%')
			    break;
			switch (*op)
			{
			    case '+': f += tv2->vval.v_float; break;
			    case '-': f -= tv2->vval.v_float; break;
			    case '*': f *= tv2->vval.v_float; break;
			    case '/': f /= tv2->vval.v_float; break;
			}
			clear_tv(tv1);
			tv1->v_type = VAR_FLOAT;
			tv1->vval.v_float = f;
		    }
		    else
#endif
		    {
			switch (*op)
			{
			    case '+': n += tv_get_number(tv2); break;
			    case '-': n -= tv_get_number(tv2); break;
			    case '*': n *= tv_get_number(tv2); break;
			    case '/': n = num_divide(n, tv_get_number(tv2)); break;
			    case '%': n = num_modulus(n, tv_get_number(tv2)); break;
			}
			clear_tv(tv1);
			tv1->v_type = VAR_NUMBER;
			tv1->vval.v_number = n;
		    }
		}
		else
		{
		    if (tv2->v_type == VAR_FLOAT)
			break;

		    // str .= str
		    s = tv_get_string(tv1);
		    s = concat_str(s, tv_get_string_buf(tv2, numbuf));
		    clear_tv(tv1);
		    tv1->v_type = VAR_STRING;
		    tv1->vval.v_string = s;
		}
		return OK;

	    case VAR_FLOAT:
#ifdef FEAT_FLOAT
		{
		    float_T f;

		    if (*op == '%' || *op == '.'
				   || (tv2->v_type != VAR_FLOAT
				    && tv2->v_type != VAR_NUMBER
				    && tv2->v_type != VAR_STRING))
			break;
		    if (tv2->v_type == VAR_FLOAT)
			f = tv2->vval.v_float;
		    else
			f = tv_get_number(tv2);
		    switch (*op)
		    {
			case '+': tv1->vval.v_float += f; break;
			case '-': tv1->vval.v_float -= f; break;
			case '*': tv1->vval.v_float *= f; break;
			case '/': tv1->vval.v_float /= f; break;
		    }
		}
#endif
		return OK;
	}
    }

    semsg(_(e_letwrong), op);
    return FAIL;
}

/*
 * Evaluate the expression used in a ":for var in expr" command.
 * "arg" points to "var".
 * Set "*errp" to TRUE for an error, FALSE otherwise;
 * Return a pointer that holds the info.  Null when there is an error.
 */
    void *
eval_for_line(
    char_u	*arg,
    int		*errp,
    char_u	**nextcmdp,
    int		skip)
{
    forinfo_T	*fi;
    char_u	*expr;
    typval_T	tv;
    list_T	*l;

    *errp = TRUE;	// default: there is an error

    fi = ALLOC_CLEAR_ONE(forinfo_T);
    if (fi == NULL)
	return NULL;

    expr = skip_var_list(arg, &fi->fi_varcount, &fi->fi_semicolon);
    if (expr == NULL)
	return fi;

    expr = skipwhite(expr);
    if (expr[0] != 'i' || expr[1] != 'n' || !VIM_ISWHITE(expr[2]))
    {
	emsg(_("E690: Missing \"in\" after :for"));
	return fi;
    }

    if (skip)
	++emsg_skip;
    if (eval0(skipwhite(expr + 2), &tv, nextcmdp, !skip) == OK)
    {
	*errp = FALSE;
	if (!skip)
	{
	    if (tv.v_type == VAR_LIST)
	    {
		l = tv.vval.v_list;
		if (l == NULL)
		{
		    // a null list is like an empty list: do nothing
		    clear_tv(&tv);
		}
		else
		{
		    // No need to increment the refcount, it's already set for
		    // the list being used in "tv".
		    fi->fi_list = l;
		    list_add_watch(l, &fi->fi_lw);
		    fi->fi_lw.lw_item = l->lv_first;
		}
	    }
	    else if (tv.v_type == VAR_BLOB)
	    {
		fi->fi_bi = 0;
		if (tv.vval.v_blob != NULL)
		{
		    typval_T btv;

		    // Make a copy, so that the iteration still works when the
		    // blob is changed.
		    blob_copy(&tv, &btv);
		    fi->fi_blob = btv.vval.v_blob;
		}
		clear_tv(&tv);
	    }
	    else
	    {
		emsg(_(e_listreq));
		clear_tv(&tv);
	    }
	}
    }
    if (skip)
	--emsg_skip;

    return fi;
}

/*
 * Use the first item in a ":for" list.  Advance to the next.
 * Assign the values to the variable (list).  "arg" points to the first one.
 * Return TRUE when a valid item was found, FALSE when at end of list or
 * something wrong.
 */
    int
next_for_item(void *fi_void, char_u *arg)
{
    forinfo_T	*fi = (forinfo_T *)fi_void;
    int		result;
    listitem_T	*item;

    if (fi->fi_blob != NULL)
    {
	typval_T	tv;

	if (fi->fi_bi >= blob_len(fi->fi_blob))
	    return FALSE;
	tv.v_type = VAR_NUMBER;
	tv.v_lock = VAR_FIXED;
	tv.vval.v_number = blob_get(fi->fi_blob, fi->fi_bi);
	++fi->fi_bi;
	return ex_let_vars(arg, &tv, TRUE, fi->fi_semicolon,
					   fi->fi_varcount, FALSE, NULL) == OK;
    }

    item = fi->fi_lw.lw_item;
    if (item == NULL)
	result = FALSE;
    else
    {
	fi->fi_lw.lw_item = item->li_next;
	result = (ex_let_vars(arg, &item->li_tv, TRUE, fi->fi_semicolon,
					  fi->fi_varcount, FALSE, NULL) == OK);
    }
    return result;
}

/*
 * Free the structure used to store info used by ":for".
 */
    void
free_for_info(void *fi_void)
{
    forinfo_T    *fi = (forinfo_T *)fi_void;

    if (fi != NULL && fi->fi_list != NULL)
    {
	list_rem_watch(fi->fi_list, &fi->fi_lw);
	list_unref(fi->fi_list);
    }
    if (fi != NULL && fi->fi_blob != NULL)
	blob_unref(fi->fi_blob);
    vim_free(fi);
}

    void
set_context_for_expression(
    expand_T	*xp,
    char_u	*arg,
    cmdidx_T	cmdidx)
{
    int		got_eq = FALSE;
    int		c;
    char_u	*p;

    if (cmdidx == CMD_let || cmdidx == CMD_const)
    {
	xp->xp_context = EXPAND_USER_VARS;
	if (vim_strpbrk(arg, (char_u *)"\"'+-*/%.=!?~|&$([<>,#") == NULL)
	{
	    // ":let var1 var2 ...": find last space.
	    for (p = arg + STRLEN(arg); p >= arg; )
	    {
		xp->xp_pattern = p;
		MB_PTR_BACK(arg, p);
		if (VIM_ISWHITE(*p))
		    break;
	    }
	    return;
	}
    }
    else
	xp->xp_context = cmdidx == CMD_call ? EXPAND_FUNCTIONS
							  : EXPAND_EXPRESSION;
    while ((xp->xp_pattern = vim_strpbrk(arg,
				  (char_u *)"\"'+-*/%.=!?~|&$([<>,#")) != NULL)
    {
	c = *xp->xp_pattern;
	if (c == '&')
	{
	    c = xp->xp_pattern[1];
	    if (c == '&')
	    {
		++xp->xp_pattern;
		xp->xp_context = cmdidx != CMD_let || got_eq
					 ? EXPAND_EXPRESSION : EXPAND_NOTHING;
	    }
	    else if (c != ' ')
	    {
		xp->xp_context = EXPAND_SETTINGS;
		if ((c == 'l' || c == 'g') && xp->xp_pattern[2] == ':')
		    xp->xp_pattern += 2;

	    }
	}
	else if (c == '$')
	{
	    // environment variable
	    xp->xp_context = EXPAND_ENV_VARS;
	}
	else if (c == '=')
	{
	    got_eq = TRUE;
	    xp->xp_context = EXPAND_EXPRESSION;
	}
	else if (c == '#'
		&& xp->xp_context == EXPAND_EXPRESSION)
	{
	    // Autoload function/variable contains '#'.
	    break;
	}
	else if ((c == '<' || c == '#')
		&& xp->xp_context == EXPAND_FUNCTIONS
		&& vim_strchr(xp->xp_pattern, '(') == NULL)
	{
	    // Function name can start with "<SNR>" and contain '#'.
	    break;
	}
	else if (cmdidx != CMD_let || got_eq)
	{
	    if (c == '"')	    // string
	    {
		while ((c = *++xp->xp_pattern) != NUL && c != '"')
		    if (c == '\\' && xp->xp_pattern[1] != NUL)
			++xp->xp_pattern;
		xp->xp_context = EXPAND_NOTHING;
	    }
	    else if (c == '\'')	    // literal string
	    {
		// Trick: '' is like stopping and starting a literal string.
		while ((c = *++xp->xp_pattern) != NUL && c != '\'')
		    /* skip */ ;
		xp->xp_context = EXPAND_NOTHING;
	    }
	    else if (c == '|')
	    {
		if (xp->xp_pattern[1] == '|')
		{
		    ++xp->xp_pattern;
		    xp->xp_context = EXPAND_EXPRESSION;
		}
		else
		    xp->xp_context = EXPAND_COMMANDS;
	    }
	    else
		xp->xp_context = EXPAND_EXPRESSION;
	}
	else
	    // Doesn't look like something valid, expand as an expression
	    // anyway.
	    xp->xp_context = EXPAND_EXPRESSION;
	arg = xp->xp_pattern;
	if (*arg != NUL)
	    while ((c = *++arg) != NUL && (c == ' ' || c == '\t'))
		/* skip */ ;
    }
    xp->xp_pattern = arg;
}

/*
 * Return TRUE if "pat" matches "text".
 * Does not use 'cpo' and always uses 'magic'.
 */
    int
pattern_match(char_u *pat, char_u *text, int ic)
{
    int		matches = FALSE;
    char_u	*save_cpo;
    regmatch_T	regmatch;

    // avoid 'l' flag in 'cpoptions'
    save_cpo = p_cpo;
    p_cpo = (char_u *)"";
    regmatch.regprog = vim_regcomp(pat, RE_MAGIC + RE_STRING);
    if (regmatch.regprog != NULL)
    {
	regmatch.rm_ic = ic;
	matches = vim_regexec_nl(&regmatch, text, (colnr_T)0);
	vim_regfree(regmatch.regprog);
    }
    p_cpo = save_cpo;
    return matches;
}

/*
 * Handle a name followed by "(".  Both for just "name(arg)" and for
 * "expr->name(arg)".
 * Returns OK or FAIL.
 */
    static int
eval_func(
	char_u	    **arg,	// points to "(", will be advanced
	char_u	    *name,
	int	    name_len,
	typval_T    *rettv,
	int	    evaluate,
	typval_T    *basetv)	// "expr" for "expr->name(arg)"
{
    char_u	*s = name;
    int		len = name_len;
    partial_T	*partial;
    int		ret = OK;

    if (!evaluate)
	check_vars(s, len);

    // If "s" is the name of a variable of type VAR_FUNC
    // use its contents.
    s = deref_func_name(s, &len, &partial, !evaluate);

    // Need to make a copy, in case evaluating the arguments makes
    // the name invalid.
    s = vim_strsave(s);
    if (s == NULL)
	ret = FAIL;
    else
    {
	funcexe_T funcexe;

	// Invoke the function.
	vim_memset(&funcexe, 0, sizeof(funcexe));
	funcexe.firstline = curwin->w_cursor.lnum;
	funcexe.lastline = curwin->w_cursor.lnum;
	funcexe.evaluate = evaluate;
	funcexe.partial = partial;
	funcexe.basetv = basetv;
	ret = get_func_tv(s, len, rettv, arg, &funcexe);
    }
    vim_free(s);

    // If evaluate is FALSE rettv->v_type was not set in
    // get_func_tv, but it's needed in handle_subscript() to parse
    // what follows. So set it here.
    if (rettv->v_type == VAR_UNKNOWN && !evaluate && **arg == '(')
    {
	rettv->vval.v_string = NULL;
	rettv->v_type = VAR_FUNC;
    }

    // Stop the expression evaluation when immediately
    // aborting on error, or when an interrupt occurred or
    // an exception was thrown but not caught.
    if (evaluate && aborting())
    {
	if (ret == OK)
	    clear_tv(rettv);
	ret = FAIL;
    }
    return ret;
}

/*
 * The "evaluate" argument: When FALSE, the argument is only parsed but not
 * executed.  The function may return OK, but the rettv will be of type
 * VAR_UNKNOWN.  The function still returns FAIL for a syntax error.
 */

/*
 * Handle zero level expression.
 * This calls eval1() and handles error message and nextcmd.
 * Put the result in "rettv" when returning OK and "evaluate" is TRUE.
 * Note: "rettv.v_lock" is not set.
 * Return OK or FAIL.
 */
    int
eval0(
    char_u	*arg,
    typval_T	*rettv,
    char_u	**nextcmd,
    int		evaluate)
{
    int		ret;
    char_u	*p;
    int		did_emsg_before = did_emsg;
    int		called_emsg_before = called_emsg;

    p = skipwhite(arg);
    ret = eval1(&p, rettv, evaluate);
    if (ret == FAIL || !ends_excmd(*p))
    {
	if (ret != FAIL)
	    clear_tv(rettv);
	/*
	 * Report the invalid expression unless the expression evaluation has
	 * been cancelled due to an aborting error, an interrupt, or an
	 * exception, or we already gave a more specific error.
	 * Also check called_emsg for when using assert_fails().
	 */
	if (!aborting() && did_emsg == did_emsg_before
					  && called_emsg == called_emsg_before)
	    semsg(_(e_invexpr2), arg);
	ret = FAIL;
    }
    if (nextcmd != NULL)
	*nextcmd = check_nextcmd(p);

    return ret;
}

/*
 * Handle top level expression:
 *	expr2 ? expr1 : expr1
 *
 * "arg" must point to the first non-white of the expression.
 * "arg" is advanced to the next non-white after the recognized expression.
 *
 * Note: "rettv.v_lock" is not set.
 *
 * Return OK or FAIL.
 */
    int
eval1(char_u **arg, typval_T *rettv, int evaluate)
{
    int		result;
    typval_T	var2;

    /*
     * Get the first variable.
     */
    if (eval2(arg, rettv, evaluate) == FAIL)
	return FAIL;

    if ((*arg)[0] == '?')
    {
	result = FALSE;
	if (evaluate)
	{
	    int		error = FALSE;

	    if (tv_get_number_chk(rettv, &error) != 0)
		result = TRUE;
	    clear_tv(rettv);
	    if (error)
		return FAIL;
	}

	/*
	 * Get the second variable.
	 */
	*arg = skipwhite(*arg + 1);
	if (eval1(arg, rettv, evaluate && result) == FAIL) // recursive!
	    return FAIL;

	/*
	 * Check for the ":".
	 */
	if ((*arg)[0] != ':')
	{
	    emsg(_("E109: Missing ':' after '?'"));
	    if (evaluate && result)
		clear_tv(rettv);
	    return FAIL;
	}

	/*
	 * Get the third variable.
	 */
	*arg = skipwhite(*arg + 1);
	if (eval1(arg, &var2, evaluate && !result) == FAIL) // recursive!
	{
	    if (evaluate && result)
		clear_tv(rettv);
	    return FAIL;
	}
	if (evaluate && !result)
	    *rettv = var2;
    }

    return OK;
}

/*
 * Handle first level expression:
 *	expr2 || expr2 || expr2	    logical OR
 *
 * "arg" must point to the first non-white of the expression.
 * "arg" is advanced to the next non-white after the recognized expression.
 *
 * Return OK or FAIL.
 */
    static int
eval2(char_u **arg, typval_T *rettv, int evaluate)
{
    typval_T	var2;
    long	result;
    int		first;
    int		error = FALSE;

    /*
     * Get the first variable.
     */
    if (eval3(arg, rettv, evaluate) == FAIL)
	return FAIL;

    /*
     * Repeat until there is no following "||".
     */
    first = TRUE;
    result = FALSE;
    while ((*arg)[0] == '|' && (*arg)[1] == '|')
    {
	if (evaluate && first)
	{
	    if (tv_get_number_chk(rettv, &error) != 0)
		result = TRUE;
	    clear_tv(rettv);
	    if (error)
		return FAIL;
	    first = FALSE;
	}

	/*
	 * Get the second variable.
	 */
	*arg = skipwhite(*arg + 2);
	if (eval3(arg, &var2, evaluate && !result) == FAIL)
	    return FAIL;

	/*
	 * Compute the result.
	 */
	if (evaluate && !result)
	{
	    if (tv_get_number_chk(&var2, &error) != 0)
		result = TRUE;
	    clear_tv(&var2);
	    if (error)
		return FAIL;
	}
	if (evaluate)
	{
	    rettv->v_type = VAR_NUMBER;
	    rettv->vval.v_number = result;
	}
    }

    return OK;
}

/*
 * Handle second level expression:
 *	expr3 && expr3 && expr3	    logical AND
 *
 * "arg" must point to the first non-white of the expression.
 * "arg" is advanced to the next non-white after the recognized expression.
 *
 * Return OK or FAIL.
 */
    static int
eval3(char_u **arg, typval_T *rettv, int evaluate)
{
    typval_T	var2;
    long	result;
    int		first;
    int		error = FALSE;

    /*
     * Get the first variable.
     */
    if (eval4(arg, rettv, evaluate) == FAIL)
	return FAIL;

    /*
     * Repeat until there is no following "&&".
     */
    first = TRUE;
    result = TRUE;
    while ((*arg)[0] == '&' && (*arg)[1] == '&')
    {
	if (evaluate && first)
	{
	    if (tv_get_number_chk(rettv, &error) == 0)
		result = FALSE;
	    clear_tv(rettv);
	    if (error)
		return FAIL;
	    first = FALSE;
	}

	/*
	 * Get the second variable.
	 */
	*arg = skipwhite(*arg + 2);
	if (eval4(arg, &var2, evaluate && result) == FAIL)
	    return FAIL;

	/*
	 * Compute the result.
	 */
	if (evaluate && result)
	{
	    if (tv_get_number_chk(&var2, &error) == 0)
		result = FALSE;
	    clear_tv(&var2);
	    if (error)
		return FAIL;
	}
	if (evaluate)
	{
	    rettv->v_type = VAR_NUMBER;
	    rettv->vval.v_number = result;
	}
    }

    return OK;
}

/*
 * Handle third level expression:
 *	var1 == var2
 *	var1 =~ var2
 *	var1 != var2
 *	var1 !~ var2
 *	var1 > var2
 *	var1 >= var2
 *	var1 < var2
 *	var1 <= var2
 *	var1 is var2
 *	var1 isnot var2
 *
 * "arg" must point to the first non-white of the expression.
 * "arg" is advanced to the next non-white after the recognized expression.
 *
 * Return OK or FAIL.
 */
    static int
eval4(char_u **arg, typval_T *rettv, int evaluate)
{
    typval_T	var2;
    char_u	*p;
    int		i;
    exptype_T	type = TYPE_UNKNOWN;
    int		type_is = FALSE;    // TRUE for "is" and "isnot"
    int		len = 2;
    int		ic;

    /*
     * Get the first variable.
     */
    if (eval5(arg, rettv, evaluate) == FAIL)
	return FAIL;

    p = *arg;
    switch (p[0])
    {
	case '=':   if (p[1] == '=')
			type = TYPE_EQUAL;
		    else if (p[1] == '~')
			type = TYPE_MATCH;
		    break;
	case '!':   if (p[1] == '=')
			type = TYPE_NEQUAL;
		    else if (p[1] == '~')
			type = TYPE_NOMATCH;
		    break;
	case '>':   if (p[1] != '=')
		    {
			type = TYPE_GREATER;
			len = 1;
		    }
		    else
			type = TYPE_GEQUAL;
		    break;
	case '<':   if (p[1] != '=')
		    {
			type = TYPE_SMALLER;
			len = 1;
		    }
		    else
			type = TYPE_SEQUAL;
		    break;
	case 'i':   if (p[1] == 's')
		    {
			if (p[2] == 'n' && p[3] == 'o' && p[4] == 't')
			    len = 5;
			i = p[len];
			if (!isalnum(i) && i != '_')
			{
			    type = len == 2 ? TYPE_EQUAL : TYPE_NEQUAL;
			    type_is = TRUE;
			}
		    }
		    break;
    }

    /*
     * If there is a comparative operator, use it.
     */
    if (type != TYPE_UNKNOWN)
    {
	// extra question mark appended: ignore case
	if (p[len] == '?')
	{
	    ic = TRUE;
	    ++len;
	}
	// extra '#' appended: match case
	else if (p[len] == '#')
	{
	    ic = FALSE;
	    ++len;
	}
	// nothing appended: use 'ignorecase'
	else
	    ic = p_ic;

	/*
	 * Get the second variable.
	 */
	*arg = skipwhite(p + len);
	if (eval5(arg, &var2, evaluate) == FAIL)
	{
	    clear_tv(rettv);
	    return FAIL;
	}
	if (evaluate)
	{
	    int ret = typval_compare(rettv, &var2, type, type_is, ic);

	    clear_tv(&var2);
	    return ret;
	}
    }

    return OK;
}

/*
 * Handle fourth level expression:
 *	+	number addition
 *	-	number subtraction
 *	.	string concatenation (if script version is 1)
 *	..	string concatenation
 *
 * "arg" must point to the first non-white of the expression.
 * "arg" is advanced to the next non-white after the recognized expression.
 *
 * Return OK or FAIL.
 */
    static int
eval5(char_u **arg, typval_T *rettv, int evaluate)
{
    typval_T	var2;
    typval_T	var3;
    int		op;
    varnumber_T	n1, n2;
#ifdef FEAT_FLOAT
    float_T	f1 = 0, f2 = 0;
#endif
    char_u	*s1, *s2;
    char_u	buf1[NUMBUFLEN], buf2[NUMBUFLEN];
    char_u	*p;
    int		concat;

    /*
     * Get the first variable.
     */
    if (eval6(arg, rettv, evaluate, FALSE) == FAIL)
	return FAIL;

    /*
     * Repeat computing, until no '+', '-' or '.' is following.
     */
    for (;;)
    {
	// "." is only string concatenation when scriptversion is 1
	op = **arg;
	concat = op == '.'
			&& (*(*arg + 1) == '.' || current_sctx.sc_version < 2);
	if (op != '+' && op != '-' && !concat)
	    break;

	if ((op != '+' || (rettv->v_type != VAR_LIST
						 && rettv->v_type != VAR_BLOB))
#ifdef FEAT_FLOAT
		&& (op == '.' || rettv->v_type != VAR_FLOAT)
#endif
		)
	{
	    // For "list + ...", an illegal use of the first operand as
	    // a number cannot be determined before evaluating the 2nd
	    // operand: if this is also a list, all is ok.
	    // For "something . ...", "something - ..." or "non-list + ...",
	    // we know that the first operand needs to be a string or number
	    // without evaluating the 2nd operand.  So check before to avoid
	    // side effects after an error.
	    if (evaluate && tv_get_string_chk(rettv) == NULL)
	    {
		clear_tv(rettv);
		return FAIL;
	    }
	}

	/*
	 * Get the second variable.
	 */
	if (op == '.' && *(*arg + 1) == '.')  // .. string concatenation
	    ++*arg;
	*arg = skipwhite(*arg + 1);
	if (eval6(arg, &var2, evaluate, op == '.') == FAIL)
	{
	    clear_tv(rettv);
	    return FAIL;
	}

	if (evaluate)
	{
	    /*
	     * Compute the result.
	     */
	    if (op == '.')
	    {
		s1 = tv_get_string_buf(rettv, buf1);	// already checked
		s2 = tv_get_string_buf_chk(&var2, buf2);
		if (s2 == NULL)		// type error ?
		{
		    clear_tv(rettv);
		    clear_tv(&var2);
		    return FAIL;
		}
		p = concat_str(s1, s2);
		clear_tv(rettv);
		rettv->v_type = VAR_STRING;
		rettv->vval.v_string = p;
	    }
	    else if (op == '+' && rettv->v_type == VAR_BLOB
						   && var2.v_type == VAR_BLOB)
	    {
		blob_T  *b1 = rettv->vval.v_blob;
		blob_T  *b2 = var2.vval.v_blob;
		blob_T	*b = blob_alloc();
		int	i;

		if (b != NULL)
		{
		    for (i = 0; i < blob_len(b1); i++)
			ga_append(&b->bv_ga, blob_get(b1, i));
		    for (i = 0; i < blob_len(b2); i++)
			ga_append(&b->bv_ga, blob_get(b2, i));

		    clear_tv(rettv);
		    rettv_blob_set(rettv, b);
		}
	    }
	    else if (op == '+' && rettv->v_type == VAR_LIST
						   && var2.v_type == VAR_LIST)
	    {
		// concatenate Lists
		if (list_concat(rettv->vval.v_list, var2.vval.v_list,
							       &var3) == FAIL)
		{
		    clear_tv(rettv);
		    clear_tv(&var2);
		    return FAIL;
		}
		clear_tv(rettv);
		*rettv = var3;
	    }
	    else
	    {
		int	    error = FALSE;

#ifdef FEAT_FLOAT
		if (rettv->v_type == VAR_FLOAT)
		{
		    f1 = rettv->vval.v_float;
		    n1 = 0;
		}
		else
#endif
		{
		    n1 = tv_get_number_chk(rettv, &error);
		    if (error)
		    {
			// This can only happen for "list + non-list".  For
			// "non-list + ..." or "something - ...", we returned
			// before evaluating the 2nd operand.
			clear_tv(rettv);
			return FAIL;
		    }
#ifdef FEAT_FLOAT
		    if (var2.v_type == VAR_FLOAT)
			f1 = n1;
#endif
		}
#ifdef FEAT_FLOAT
		if (var2.v_type == VAR_FLOAT)
		{
		    f2 = var2.vval.v_float;
		    n2 = 0;
		}
		else
#endif
		{
		    n2 = tv_get_number_chk(&var2, &error);
		    if (error)
		    {
			clear_tv(rettv);
			clear_tv(&var2);
			return FAIL;
		    }
#ifdef FEAT_FLOAT
		    if (rettv->v_type == VAR_FLOAT)
			f2 = n2;
#endif
		}
		clear_tv(rettv);

#ifdef FEAT_FLOAT
		// If there is a float on either side the result is a float.
		if (rettv->v_type == VAR_FLOAT || var2.v_type == VAR_FLOAT)
		{
		    if (op == '+')
			f1 = f1 + f2;
		    else
			f1 = f1 - f2;
		    rettv->v_type = VAR_FLOAT;
		    rettv->vval.v_float = f1;
		}
		else
#endif
		{
		    if (op == '+')
			n1 = n1 + n2;
		    else
			n1 = n1 - n2;
		    rettv->v_type = VAR_NUMBER;
		    rettv->vval.v_number = n1;
		}
	    }
	    clear_tv(&var2);
	}
    }
    return OK;
}

/*
 * Handle fifth level expression:
 *	*	number multiplication
 *	/	number division
 *	%	number modulo
 *
 * "arg" must point to the first non-white of the expression.
 * "arg" is advanced to the next non-white after the recognized expression.
 *
 * Return OK or FAIL.
 */
    static int
eval6(
    char_u	**arg,
    typval_T	*rettv,
    int		evaluate,
    int		want_string)  // after "." operator
{
    typval_T	var2;
    int		op;
    varnumber_T	n1, n2;
#ifdef FEAT_FLOAT
    int		use_float = FALSE;
    float_T	f1 = 0, f2 = 0;
#endif
    int		error = FALSE;

    /*
     * Get the first variable.
     */
    if (eval7(arg, rettv, evaluate, want_string) == FAIL)
	return FAIL;

    /*
     * Repeat computing, until no '*', '/' or '%' is following.
     */
    for (;;)
    {
	op = **arg;
	if (op != '*' && op != '/' && op != '%')
	    break;

	if (evaluate)
	{
#ifdef FEAT_FLOAT
	    if (rettv->v_type == VAR_FLOAT)
	    {
		f1 = rettv->vval.v_float;
		use_float = TRUE;
		n1 = 0;
	    }
	    else
#endif
		n1 = tv_get_number_chk(rettv, &error);
	    clear_tv(rettv);
	    if (error)
		return FAIL;
	}
	else
	    n1 = 0;

	/*
	 * Get the second variable.
	 */
	*arg = skipwhite(*arg + 1);
	if (eval7(arg, &var2, evaluate, FALSE) == FAIL)
	    return FAIL;

	if (evaluate)
	{
#ifdef FEAT_FLOAT
	    if (var2.v_type == VAR_FLOAT)
	    {
		if (!use_float)
		{
		    f1 = n1;
		    use_float = TRUE;
		}
		f2 = var2.vval.v_float;
		n2 = 0;
	    }
	    else
#endif
	    {
		n2 = tv_get_number_chk(&var2, &error);
		clear_tv(&var2);
		if (error)
		    return FAIL;
#ifdef FEAT_FLOAT
		if (use_float)
		    f2 = n2;
#endif
	    }

	    /*
	     * Compute the result.
	     * When either side is a float the result is a float.
	     */
#ifdef FEAT_FLOAT
	    if (use_float)
	    {
		if (op == '*')
		    f1 = f1 * f2;
		else if (op == '/')
		{
# ifdef VMS
		    // VMS crashes on divide by zero, work around it
		    if (f2 == 0.0)
		    {
			if (f1 == 0)
			    f1 = -1 * __F_FLT_MAX - 1L;   // similar to NaN
			else if (f1 < 0)
			    f1 = -1 * __F_FLT_MAX;
			else
			    f1 = __F_FLT_MAX;
		    }
		    else
			f1 = f1 / f2;
# else
		    // We rely on the floating point library to handle divide
		    // by zero to result in "inf" and not a crash.
		    f1 = f1 / f2;
# endif
		}
		else
		{
		    emsg(_("E804: Cannot use '%' with Float"));
		    return FAIL;
		}
		rettv->v_type = VAR_FLOAT;
		rettv->vval.v_float = f1;
	    }
	    else
#endif
	    {
		if (op == '*')
		    n1 = n1 * n2;
		else if (op == '/')
		    n1 = num_divide(n1, n2);
		else
		    n1 = num_modulus(n1, n2);

		rettv->v_type = VAR_NUMBER;
		rettv->vval.v_number = n1;
	    }
	}
    }

    return OK;
}

/*
 * Handle sixth level expression:
 *  number		number constant
 *  0zFFFFFFFF		Blob constant
 *  "string"		string constant
 *  'string'		literal string constant
 *  &option-name	option value
 *  @r			register contents
 *  identifier		variable value
 *  function()		function call
 *  $VAR		environment variable
 *  (expression)	nested expression
 *  [expr, expr]	List
 *  {key: val, key: val}   Dictionary
 *  #{key: val, key: val}  Dictionary with literal keys
 *
 *  Also handle:
 *  ! in front		logical NOT
 *  - in front		unary minus
 *  + in front		unary plus (ignored)
 *  trailing []		subscript in String or List
 *  trailing .name	entry in Dictionary
 *  trailing ->name()	method call
 *
 * "arg" must point to the first non-white of the expression.
 * "arg" is advanced to the next non-white after the recognized expression.
 *
 * Return OK or FAIL.
 */
    static int
eval7(
    char_u	**arg,
    typval_T	*rettv,
    int		evaluate,
    int		want_string UNUSED)	// after "." operator
{
    varnumber_T	n;
    int		len;
    char_u	*s;
    char_u	*start_leader, *end_leader;
    int		ret = OK;
    char_u	*alias;

    /*
     * Initialise variable so that clear_tv() can't mistake this for a
     * string and free a string that isn't there.
     */
    rettv->v_type = VAR_UNKNOWN;

    /*
     * Skip '!', '-' and '+' characters.  They are handled later.
     */
    start_leader = *arg;
    while (**arg == '!' || **arg == '-' || **arg == '+')
	*arg = skipwhite(*arg + 1);
    end_leader = *arg;

    if (**arg == '.' && (!isdigit(*(*arg + 1))
#ifdef FEAT_FLOAT
	    || current_sctx.sc_version < 2
#endif
	    ))
    {
	semsg(_(e_invexpr2), *arg);
	++*arg;
	return FAIL;
    }

    switch (**arg)
    {
    /*
     * Number constant.
     */
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '.':
	{
#ifdef FEAT_FLOAT
		char_u *p;
		int    get_float = FALSE;

		// We accept a float when the format matches
		// "[0-9]\+\.[0-9]\+\([eE][+-]\?[0-9]\+\)\?".  This is very
		// strict to avoid backwards compatibility problems.
		// With script version 2 and later the leading digit can be
		// omitted.
		// Don't look for a float after the "." operator, so that
		// ":let vers = 1.2.3" doesn't fail.
		if (**arg == '.')
		    p = *arg;
		else
		    p = skipdigits(*arg + 1);
		if (!want_string && p[0] == '.' && vim_isdigit(p[1]))
		{
		    get_float = TRUE;
		    p = skipdigits(p + 2);
		    if (*p == 'e' || *p == 'E')
		    {
			++p;
			if (*p == '-' || *p == '+')
			    ++p;
			if (!vim_isdigit(*p))
			    get_float = FALSE;
			else
			    p = skipdigits(p + 1);
		    }
		    if (ASCII_ISALPHA(*p) || *p == '.')
			get_float = FALSE;
		}
		if (get_float)
		{
		    float_T	f;

		    *arg += string2float(*arg, &f);
		    if (evaluate)
		    {
			rettv->v_type = VAR_FLOAT;
			rettv->vval.v_float = f;
		    }
		}
		else
#endif
		if (**arg == '0' && ((*arg)[1] == 'z' || (*arg)[1] == 'Z'))
		{
		    char_u  *bp;
		    blob_T  *blob = NULL;  // init for gcc

		    // Blob constant: 0z0123456789abcdef
		    if (evaluate)
			blob = blob_alloc();
		    for (bp = *arg + 2; vim_isxdigit(bp[0]); bp += 2)
		    {
			if (!vim_isxdigit(bp[1]))
			{
			    if (blob != NULL)
			    {
				emsg(_("E973: Blob literal should have an even number of hex characters"));
				ga_clear(&blob->bv_ga);
				VIM_CLEAR(blob);
			    }
			    ret = FAIL;
			    break;
			}
			if (blob != NULL)
			    ga_append(&blob->bv_ga,
					 (hex2nr(*bp) << 4) + hex2nr(*(bp+1)));
			if (bp[2] == '.' && vim_isxdigit(bp[3]))
			    ++bp;
		    }
		    if (blob != NULL)
			rettv_blob_set(rettv, blob);
		    *arg = bp;
		}
		else
		{
		    // decimal, hex or octal number
		    vim_str2nr(*arg, NULL, &len, current_sctx.sc_version >= 4
				  ? STR2NR_NO_OCT + STR2NR_QUOTE
				  : STR2NR_ALL, &n, NULL, 0, TRUE);
		    if (len == 0)
		    {
			semsg(_(e_invexpr2), *arg);
			ret = FAIL;
			break;
		    }
		    *arg += len;
		    if (evaluate)
		    {
			rettv->v_type = VAR_NUMBER;
			rettv->vval.v_number = n;
		    }
		}
		break;
	}

    /*
     * String constant: "string".
     */
    case '"':	ret = get_string_tv(arg, rettv, evaluate);
		break;

    /*
     * Literal string constant: 'str''ing'.
     */
    case '\'':	ret = get_lit_string_tv(arg, rettv, evaluate);
		break;

    /*
     * List: [expr, expr]
     */
    case '[':	ret = get_list_tv(arg, rettv, evaluate);
		break;

    /*
     * Dictionary: #{key: val, key: val}
     */
    case '#':	if ((*arg)[1] == '{')
		{
		    ++*arg;
		    ret = dict_get_tv(arg, rettv, evaluate, TRUE);
		}
		else
		    ret = NOTDONE;
		break;

    /*
     * Lambda: {arg, arg -> expr}
     * Dictionary: {'key': val, 'key': val}
     */
    case '{':	ret = get_lambda_tv(arg, rettv, evaluate);
		if (ret == NOTDONE)
		    ret = dict_get_tv(arg, rettv, evaluate, FALSE);
		break;

    /*
     * Option value: &name
     */
    case '&':	ret = get_option_tv(arg, rettv, evaluate);
		break;

    /*
     * Environment variable: $VAR.
     */
    case '$':	ret = get_env_tv(arg, rettv, evaluate);
		break;

    /*
     * Register contents: @r.
     */
    case '@':	++*arg;
		if (evaluate)
		{
		    rettv->v_type = VAR_STRING;
		    rettv->vval.v_string = get_reg_contents(**arg,
							    GREG_EXPR_SRC);
		}
		if (**arg != NUL)
		    ++*arg;
		break;

    /*
     * nested expression: (expression).
     */
    case '(':	*arg = skipwhite(*arg + 1);
		ret = eval1(arg, rettv, evaluate);	// recursive!
		if (**arg == ')')
		    ++*arg;
		else if (ret == OK)
		{
		    emsg(_("E110: Missing ')'"));
		    clear_tv(rettv);
		    ret = FAIL;
		}
		break;

    default:	ret = NOTDONE;
		break;
    }

    if (ret == NOTDONE)
    {
	/*
	 * Must be a variable or function name.
	 * Can also be a curly-braces kind of name: {expr}.
	 */
	s = *arg;
	len = get_name_len(arg, &alias, evaluate, TRUE);
	if (alias != NULL)
	    s = alias;

	if (len <= 0)
	    ret = FAIL;
	else
	{
	    if (**arg == '(')		// recursive!
		ret = eval_func(arg, s, len, rettv, evaluate, NULL);
	    else if (evaluate)
		ret = get_var_tv(s, len, rettv, NULL, TRUE, FALSE);
	    else
	    {
		check_vars(s, len);
		ret = OK;
	    }
	}
	vim_free(alias);
    }

    *arg = skipwhite(*arg);

    // Handle following '[', '(' and '.' for expr[expr], expr.name,
    // expr(expr), expr->name(expr)
    if (ret == OK)
	ret = handle_subscript(arg, rettv, evaluate, TRUE,
						    start_leader, &end_leader);

    /*
     * Apply logical NOT and unary '-', from right to left, ignore '+'.
     */
    if (ret == OK && evaluate && end_leader > start_leader)
	ret = eval7_leader(rettv, start_leader, &end_leader);
    return ret;
}

/*
 * Apply the leading "!" and "-" before an eval7 expression to "rettv".
 * Adjusts "end_leaderp" until it is at "start_leader".
 */
    static int
eval7_leader(typval_T *rettv, char_u *start_leader, char_u **end_leaderp)
{
    char_u	*end_leader = *end_leaderp;
    int		ret = OK;
    int		error = FALSE;
    varnumber_T val = 0;
#ifdef FEAT_FLOAT
    float_T	    f = 0.0;

    if (rettv->v_type == VAR_FLOAT)
	f = rettv->vval.v_float;
    else
#endif
	val = tv_get_number_chk(rettv, &error);
    if (error)
    {
	clear_tv(rettv);
	ret = FAIL;
    }
    else
    {
	while (end_leader > start_leader)
	{
	    --end_leader;
	    if (*end_leader == '!')
	    {
#ifdef FEAT_FLOAT
		if (rettv->v_type == VAR_FLOAT)
		    f = !f;
		else
#endif
		    val = !val;
	    }
	    else if (*end_leader == '-')
	    {
#ifdef FEAT_FLOAT
		if (rettv->v_type == VAR_FLOAT)
		    f = -f;
		else
#endif
		    val = -val;
	    }
	}
#ifdef FEAT_FLOAT
	if (rettv->v_type == VAR_FLOAT)
	{
	    clear_tv(rettv);
	    rettv->vval.v_float = f;
	}
	else
#endif
	{
	    clear_tv(rettv);
	    rettv->v_type = VAR_NUMBER;
	    rettv->vval.v_number = val;
	}
    }
    *end_leaderp = end_leader;
    return ret;
}

/*
 * Call the function referred to in "rettv".
 */
    static int
call_func_rettv(
	char_u	    **arg,
	typval_T    *rettv,
	int	    evaluate,
	dict_T	    *selfdict,
	typval_T    *basetv)
{
    partial_T	*pt = NULL;
    funcexe_T	funcexe;
    typval_T	functv;
    char_u	*s;
    int		ret;

    // need to copy the funcref so that we can clear rettv
    if (evaluate)
    {
	functv = *rettv;
	rettv->v_type = VAR_UNKNOWN;

	// Invoke the function.  Recursive!
	if (functv.v_type == VAR_PARTIAL)
	{
	    pt = functv.vval.v_partial;
	    s = partial_name(pt);
	}
	else
	    s = functv.vval.v_string;
    }
    else
	s = (char_u *)"";

    vim_memset(&funcexe, 0, sizeof(funcexe));
    funcexe.firstline = curwin->w_cursor.lnum;
    funcexe.lastline = curwin->w_cursor.lnum;
    funcexe.evaluate = evaluate;
    funcexe.partial = pt;
    funcexe.selfdict = selfdict;
    funcexe.basetv = basetv;
    ret = get_func_tv(s, -1, rettv, arg, &funcexe);

    // Clear the funcref afterwards, so that deleting it while
    // evaluating the arguments is possible (see test55).
    if (evaluate)
	clear_tv(&functv);

    return ret;
}

/*
 * Evaluate "->method()".
 * "*arg" points to the '-'.
 * Returns FAIL or OK. "*arg" is advanced to after the ')'.
 */
    static int
eval_lambda(
    char_u	**arg,
    typval_T	*rettv,
    int		evaluate,
    int		verbose)	// give error messages
{
    typval_T	base = *rettv;
    int		ret;

    // Skip over the ->.
    *arg += 2;
    rettv->v_type = VAR_UNKNOWN;

    ret = get_lambda_tv(arg, rettv, evaluate);
    if (ret == NOTDONE)
	return FAIL;
    else if (**arg != '(')
    {
	if (verbose)
	{
	    if (*skipwhite(*arg) == '(')
		semsg(_(e_nowhitespace));
	    else
		semsg(_(e_missingparen), "lambda");
	}
	clear_tv(rettv);
	ret = FAIL;
    }
    else
	ret = call_func_rettv(arg, rettv, evaluate, NULL, &base);

    // Clear the funcref afterwards, so that deleting it while
    // evaluating the arguments is possible (see test55).
    if (evaluate)
	clear_tv(&base);

    return ret;
}

/*
 * Evaluate "->method()".
 * "*arg" points to the '-'.
 * Returns FAIL or OK. "*arg" is advanced to after the ')'.
 */
    static int
eval_method(
    char_u	**arg,
    typval_T	*rettv,
    int		evaluate,
    int		verbose)	// give error messages
{
    char_u	*name;
    long	len;
    char_u	*alias;
    typval_T	base = *rettv;
    int		ret;

    // Skip over the ->.
    *arg += 2;
    rettv->v_type = VAR_UNKNOWN;

    name = *arg;
    len = get_name_len(arg, &alias, evaluate, TRUE);
    if (alias != NULL)
	name = alias;

    if (len <= 0)
    {
	if (verbose)
	    emsg(_("E260: Missing name after ->"));
	ret = FAIL;
    }
    else
    {
	if (**arg != '(')
	{
	    if (verbose)
		semsg(_(e_missingparen), name);
	    ret = FAIL;
	}
	else if (VIM_ISWHITE((*arg)[-1]))
	{
	    if (verbose)
		semsg(_(e_nowhitespace));
	    ret = FAIL;
	}
	else
	    ret = eval_func(arg, name, len, rettv, evaluate, &base);
    }

    // Clear the funcref afterwards, so that deleting it while
    // evaluating the arguments is possible (see test55).
    if (evaluate)
	clear_tv(&base);

    return ret;
}

/*
 * Evaluate an "[expr]" or "[expr:expr]" index.  Also "dict.key".
 * "*arg" points to the '[' or '.'.
 * Returns FAIL or OK. "*arg" is advanced to after the ']'.
 */
    static int
eval_index(
    char_u	**arg,
    typval_T	*rettv,
    int		evaluate,
    int		verbose)	// give error messages
{
    int		empty1 = FALSE, empty2 = FALSE;
    typval_T	var1, var2;
    long	i;
    long	n1, n2 = 0;
    long	len = -1;
    int		range = FALSE;
    char_u	*s;
    char_u	*key = NULL;

    switch (rettv->v_type)
    {
	case VAR_FUNC:
	case VAR_PARTIAL:
	    if (verbose)
		emsg(_("E695: Cannot index a Funcref"));
	    return FAIL;
	case VAR_FLOAT:
#ifdef FEAT_FLOAT
	    if (verbose)
		emsg(_(e_float_as_string));
	    return FAIL;
#endif
	case VAR_SPECIAL:
	case VAR_JOB:
	case VAR_CHANNEL:
	    if (verbose)
		emsg(_("E909: Cannot index a special variable"));
	    return FAIL;
	case VAR_UNKNOWN:
	    if (evaluate)
		return FAIL;
	    // FALLTHROUGH

	case VAR_STRING:
	case VAR_NUMBER:
	case VAR_LIST:
	case VAR_DICT:
	case VAR_BLOB:
	    break;
    }

    init_tv(&var1);
    init_tv(&var2);
    if (**arg == '.')
    {
	/*
	 * dict.name
	 */
	key = *arg + 1;
	for (len = 0; ASCII_ISALNUM(key[len]) || key[len] == '_'; ++len)
	    ;
	if (len == 0)
	    return FAIL;
	*arg = skipwhite(key + len);
    }
    else
    {
	/*
	 * something[idx]
	 *
	 * Get the (first) variable from inside the [].
	 */
	*arg = skipwhite(*arg + 1);
	if (**arg == ':')
	    empty1 = TRUE;
	else if (eval1(arg, &var1, evaluate) == FAIL)	// recursive!
	    return FAIL;
	else if (evaluate && tv_get_string_chk(&var1) == NULL)
	{
	    // not a number or string
	    clear_tv(&var1);
	    return FAIL;
	}

	/*
	 * Get the second variable from inside the [:].
	 */
	if (**arg == ':')
	{
	    range = TRUE;
	    *arg = skipwhite(*arg + 1);
	    if (**arg == ']')
		empty2 = TRUE;
	    else if (eval1(arg, &var2, evaluate) == FAIL)	// recursive!
	    {
		if (!empty1)
		    clear_tv(&var1);
		return FAIL;
	    }
	    else if (evaluate && tv_get_string_chk(&var2) == NULL)
	    {
		// not a number or string
		if (!empty1)
		    clear_tv(&var1);
		clear_tv(&var2);
		return FAIL;
	    }
	}

	// Check for the ']'.
	if (**arg != ']')
	{
	    if (verbose)
		emsg(_(e_missbrac));
	    clear_tv(&var1);
	    if (range)
		clear_tv(&var2);
	    return FAIL;
	}
	*arg = skipwhite(*arg + 1);	// skip the ']'
    }

    if (evaluate)
    {
	n1 = 0;
	if (!empty1 && rettv->v_type != VAR_DICT)
	{
	    n1 = tv_get_number(&var1);
	    clear_tv(&var1);
	}
	if (range)
	{
	    if (empty2)
		n2 = -1;
	    else
	    {
		n2 = tv_get_number(&var2);
		clear_tv(&var2);
	    }
	}

	switch (rettv->v_type)
	{
	    case VAR_UNKNOWN:
	    case VAR_FUNC:
	    case VAR_PARTIAL:
	    case VAR_FLOAT:
	    case VAR_SPECIAL:
	    case VAR_JOB:
	    case VAR_CHANNEL:
		break; // not evaluating, skipping over subscript

	    case VAR_NUMBER:
	    case VAR_STRING:
		s = tv_get_string(rettv);
		len = (long)STRLEN(s);
		if (range)
		{
		    // The resulting variable is a substring.  If the indexes
		    // are out of range the result is empty.
		    if (n1 < 0)
		    {
			n1 = len + n1;
			if (n1 < 0)
			    n1 = 0;
		    }
		    if (n2 < 0)
			n2 = len + n2;
		    else if (n2 >= len)
			n2 = len;
		    if (n1 >= len || n2 < 0 || n1 > n2)
			s = NULL;
		    else
			s = vim_strnsave(s + n1, (int)(n2 - n1 + 1));
		}
		else
		{
		    // The resulting variable is a string of a single
		    // character.  If the index is too big or negative the
		    // result is empty.
		    if (n1 >= len || n1 < 0)
			s = NULL;
		    else
			s = vim_strnsave(s + n1, 1);
		}
		clear_tv(rettv);
		rettv->v_type = VAR_STRING;
		rettv->vval.v_string = s;
		break;

	    case VAR_BLOB:
		len = blob_len(rettv->vval.v_blob);
		if (range)
		{
		    // The resulting variable is a sub-blob.  If the indexes
		    // are out of range the result is empty.
		    if (n1 < 0)
		    {
			n1 = len + n1;
			if (n1 < 0)
			    n1 = 0;
		    }
		    if (n2 < 0)
			n2 = len + n2;
		    else if (n2 >= len)
			n2 = len - 1;
		    if (n1 >= len || n2 < 0 || n1 > n2)
		    {
			clear_tv(rettv);
			rettv->v_type = VAR_BLOB;
			rettv->vval.v_blob = NULL;
		    }
		    else
		    {
			blob_T  *blob = blob_alloc();

			if (blob != NULL)
			{
			    if (ga_grow(&blob->bv_ga, n2 - n1 + 1) == FAIL)
			    {
				blob_free(blob);
				return FAIL;
			    }
			    blob->bv_ga.ga_len = n2 - n1 + 1;
			    for (i = n1; i <= n2; i++)
				blob_set(blob, i - n1,
					      blob_get(rettv->vval.v_blob, i));

			    clear_tv(rettv);
			    rettv_blob_set(rettv, blob);
			}
		    }
		}
		else
		{
		    // The resulting variable is a byte value.
		    // If the index is too big or negative that is an error.
		    if (n1 < 0)
			n1 = len + n1;
		    if (n1 < len && n1 >= 0)
		    {
			int v = blob_get(rettv->vval.v_blob, n1);

			clear_tv(rettv);
			rettv->v_type = VAR_NUMBER;
			rettv->vval.v_number = v;
		    }
		    else
			semsg(_(e_blobidx), n1);
		}
		break;

	    case VAR_LIST:
		len = list_len(rettv->vval.v_list);
		if (n1 < 0)
		    n1 = len + n1;
		if (!empty1 && (n1 < 0 || n1 >= len))
		{
		    // For a range we allow invalid values and return an empty
		    // list.  A list index out of range is an error.
		    if (!range)
		    {
			if (verbose)
			    semsg(_(e_listidx), n1);
			return FAIL;
		    }
		    n1 = len;
		}
		if (range)
		{
		    list_T	*l;
		    listitem_T	*item;

		    if (n2 < 0)
			n2 = len + n2;
		    else if (n2 >= len)
			n2 = len - 1;
		    if (!empty2 && (n2 < 0 || n2 + 1 < n1))
			n2 = -1;
		    l = list_alloc();
		    if (l == NULL)
			return FAIL;
		    for (item = list_find(rettv->vval.v_list, n1);
							       n1 <= n2; ++n1)
		    {
			if (list_append_tv(l, &item->li_tv) == FAIL)
			{
			    list_free(l);
			    return FAIL;
			}
			item = item->li_next;
		    }
		    clear_tv(rettv);
		    rettv_list_set(rettv, l);
		}
		else
		{
		    copy_tv(&list_find(rettv->vval.v_list, n1)->li_tv, &var1);
		    clear_tv(rettv);
		    *rettv = var1;
		}
		break;

	    case VAR_DICT:
		if (range)
		{
		    if (verbose)
			emsg(_(e_dictrange));
		    if (len == -1)
			clear_tv(&var1);
		    return FAIL;
		}
		{
		    dictitem_T	*item;

		    if (len == -1)
		    {
			key = tv_get_string_chk(&var1);
			if (key == NULL)
			{
			    clear_tv(&var1);
			    return FAIL;
			}
		    }

		    item = dict_find(rettv->vval.v_dict, key, (int)len);

		    if (item == NULL && verbose)
			semsg(_(e_dictkey), key);
		    if (len == -1)
			clear_tv(&var1);
		    if (item == NULL)
			return FAIL;

		    copy_tv(&item->di_tv, &var1);
		    clear_tv(rettv);
		    *rettv = var1;
		}
		break;
	}
    }

    return OK;
}

/*
 * Get an option value.
 * "arg" points to the '&' or '+' before the option name.
 * "arg" is advanced to character after the option name.
 * Return OK or FAIL.
 */
    int
get_option_tv(
    char_u	**arg,
    typval_T	*rettv,	// when NULL, only check if option exists
    int		evaluate)
{
    char_u	*option_end;
    long	numval;
    char_u	*stringval;
    int		opt_type;
    int		c;
    int		working = (**arg == '+');    // has("+option")
    int		ret = OK;
    int		opt_flags;

    /*
     * Isolate the option name and find its value.
     */
    option_end = find_option_end(arg, &opt_flags);
    if (option_end == NULL)
    {
	if (rettv != NULL)
	    semsg(_("E112: Option name missing: %s"), *arg);
	return FAIL;
    }

    if (!evaluate)
    {
	*arg = option_end;
	return OK;
    }

    c = *option_end;
    *option_end = NUL;
    opt_type = get_option_value(*arg, &numval,
			       rettv == NULL ? NULL : &stringval, opt_flags);

    if (opt_type == -3)			// invalid name
    {
	if (rettv != NULL)
	    semsg(_("E113: Unknown option: %s"), *arg);
	ret = FAIL;
    }
    else if (rettv != NULL)
    {
	if (opt_type == -2)		// hidden string option
	{
	    rettv->v_type = VAR_STRING;
	    rettv->vval.v_string = NULL;
	}
	else if (opt_type == -1)	// hidden number option
	{
	    rettv->v_type = VAR_NUMBER;
	    rettv->vval.v_number = 0;
	}
	else if (opt_type == 1)		// number option
	{
	    rettv->v_type = VAR_NUMBER;
	    rettv->vval.v_number = numval;
	}
	else				// string option
	{
	    rettv->v_type = VAR_STRING;
	    rettv->vval.v_string = stringval;
	}
    }
    else if (working && (opt_type == -2 || opt_type == -1))
	ret = FAIL;

    *option_end = c;		    // put back for error messages
    *arg = option_end;

    return ret;
}

/*
 * Allocate a variable for a string constant.
 * Return OK or FAIL.
 */
    static int
get_string_tv(char_u **arg, typval_T *rettv, int evaluate)
{
    char_u	*p;
    char_u	*name;
    int		extra = 0;

    /*
     * Find the end of the string, skipping backslashed characters.
     */
    for (p = *arg + 1; *p != NUL && *p != '"'; MB_PTR_ADV(p))
    {
	if (*p == '\\' && p[1] != NUL)
	{
	    ++p;
	    // A "\<x>" form occupies at least 4 characters, and produces up
	    // to 6 characters: reserve space for 2 extra
	    if (*p == '<')
		extra += 2;
	}
    }

    if (*p != '"')
    {
	semsg(_("E114: Missing quote: %s"), *arg);
	return FAIL;
    }

    // If only parsing, set *arg and return here
    if (!evaluate)
    {
	*arg = p + 1;
	return OK;
    }

    /*
     * Copy the string into allocated memory, handling backslashed
     * characters.
     */
    name = alloc(p - *arg + extra);
    if (name == NULL)
	return FAIL;
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = name;

    for (p = *arg + 1; *p != NUL && *p != '"'; )
    {
	if (*p == '\\')
	{
	    switch (*++p)
	    {
		case 'b': *name++ = BS; ++p; break;
		case 'e': *name++ = ESC; ++p; break;
		case 'f': *name++ = FF; ++p; break;
		case 'n': *name++ = NL; ++p; break;
		case 'r': *name++ = CAR; ++p; break;
		case 't': *name++ = TAB; ++p; break;

		case 'X': // hex: "\x1", "\x12"
		case 'x':
		case 'u': // Unicode: "\u0023"
		case 'U':
			  if (vim_isxdigit(p[1]))
			  {
			      int	n, nr;
			      int	c = toupper(*p);

			      if (c == 'X')
				  n = 2;
			      else if (*p == 'u')
				  n = 4;
			      else
				  n = 8;
			      nr = 0;
			      while (--n >= 0 && vim_isxdigit(p[1]))
			      {
				  ++p;
				  nr = (nr << 4) + hex2nr(*p);
			      }
			      ++p;
			      // For "\u" store the number according to
			      // 'encoding'.
			      if (c != 'X')
				  name += (*mb_char2bytes)(nr, name);
			      else
				  *name++ = nr;
			  }
			  break;

			  // octal: "\1", "\12", "\123"
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7': *name = *p++ - '0';
			  if (*p >= '0' && *p <= '7')
			  {
			      *name = (*name << 3) + *p++ - '0';
			      if (*p >= '0' && *p <= '7')
				  *name = (*name << 3) + *p++ - '0';
			  }
			  ++name;
			  break;

			    // Special key, e.g.: "\<C-W>"
		case '<': extra = trans_special(&p, name, TRUE, TRUE,
								   TRUE, NULL);
			  if (extra != 0)
			  {
			      name += extra;
			      break;
			  }
			  // FALLTHROUGH

		default:  MB_COPY_CHAR(p, name);
			  break;
	    }
	}
	else
	    MB_COPY_CHAR(p, name);

    }
    *name = NUL;
    if (*p != NUL) // just in case
	++p;
    *arg = p;

    return OK;
}

/*
 * Allocate a variable for a 'str''ing' constant.
 * Return OK or FAIL.
 */
    static int
get_lit_string_tv(char_u **arg, typval_T *rettv, int evaluate)
{
    char_u	*p;
    char_u	*str;
    int		reduce = 0;

    /*
     * Find the end of the string, skipping ''.
     */
    for (p = *arg + 1; *p != NUL; MB_PTR_ADV(p))
    {
	if (*p == '\'')
	{
	    if (p[1] != '\'')
		break;
	    ++reduce;
	    ++p;
	}
    }

    if (*p != '\'')
    {
	semsg(_("E115: Missing quote: %s"), *arg);
	return FAIL;
    }

    // If only parsing return after setting "*arg"
    if (!evaluate)
    {
	*arg = p + 1;
	return OK;
    }

    /*
     * Copy the string into allocated memory, handling '' to ' reduction.
     */
    str = alloc((p - *arg) - reduce);
    if (str == NULL)
	return FAIL;
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = str;

    for (p = *arg + 1; *p != NUL; )
    {
	if (*p == '\'')
	{
	    if (p[1] != '\'')
		break;
	    ++p;
	}
	MB_COPY_CHAR(p, str);
    }
    *str = NUL;
    *arg = p + 1;

    return OK;
}

/*
 * Return the function name of the partial.
 */
    char_u *
partial_name(partial_T *pt)
{
    if (pt->pt_name != NULL)
	return pt->pt_name;
    return pt->pt_func->uf_name;
}

    static void
partial_free(partial_T *pt)
{
    int i;

    for (i = 0; i < pt->pt_argc; ++i)
	clear_tv(&pt->pt_argv[i]);
    vim_free(pt->pt_argv);
    dict_unref(pt->pt_dict);
    if (pt->pt_name != NULL)
    {
	func_unref(pt->pt_name);
	vim_free(pt->pt_name);
    }
    else
	func_ptr_unref(pt->pt_func);
    vim_free(pt);
}

/*
 * Unreference a closure: decrement the reference count and free it when it
 * becomes zero.
 */
    void
partial_unref(partial_T *pt)
{
    if (pt != NULL && --pt->pt_refcount <= 0)
	partial_free(pt);
}

static int tv_equal_recurse_limit;

    static int
func_equal(
    typval_T *tv1,
    typval_T *tv2,
    int	     ic)	    // ignore case
{
    char_u	*s1, *s2;
    dict_T	*d1, *d2;
    int		a1, a2;
    int		i;

    // empty and NULL function name considered the same
    s1 = tv1->v_type == VAR_FUNC ? tv1->vval.v_string
					   : partial_name(tv1->vval.v_partial);
    if (s1 != NULL && *s1 == NUL)
	s1 = NULL;
    s2 = tv2->v_type == VAR_FUNC ? tv2->vval.v_string
					   : partial_name(tv2->vval.v_partial);
    if (s2 != NULL && *s2 == NUL)
	s2 = NULL;
    if (s1 == NULL || s2 == NULL)
    {
	if (s1 != s2)
	    return FALSE;
    }
    else if (STRCMP(s1, s2) != 0)
	return FALSE;

    // empty dict and NULL dict is different
    d1 = tv1->v_type == VAR_FUNC ? NULL : tv1->vval.v_partial->pt_dict;
    d2 = tv2->v_type == VAR_FUNC ? NULL : tv2->vval.v_partial->pt_dict;
    if (d1 == NULL || d2 == NULL)
    {
	if (d1 != d2)
	    return FALSE;
    }
    else if (!dict_equal(d1, d2, ic, TRUE))
	return FALSE;

    // empty list and no list considered the same
    a1 = tv1->v_type == VAR_FUNC ? 0 : tv1->vval.v_partial->pt_argc;
    a2 = tv2->v_type == VAR_FUNC ? 0 : tv2->vval.v_partial->pt_argc;
    if (a1 != a2)
	return FALSE;
    for (i = 0; i < a1; ++i)
	if (!tv_equal(tv1->vval.v_partial->pt_argv + i,
		      tv2->vval.v_partial->pt_argv + i, ic, TRUE))
	    return FALSE;

    return TRUE;
}

/*
 * Return TRUE if "tv1" and "tv2" have the same value.
 * Compares the items just like "==" would compare them, but strings and
 * numbers are different.  Floats and numbers are also different.
 */
    int
tv_equal(
    typval_T *tv1,
    typval_T *tv2,
    int	     ic,	    // ignore case
    int	     recursive)	    // TRUE when used recursively
{
    char_u	buf1[NUMBUFLEN], buf2[NUMBUFLEN];
    char_u	*s1, *s2;
    static int  recursive_cnt = 0;	    // catch recursive loops
    int		r;

    // Catch lists and dicts that have an endless loop by limiting
    // recursiveness to a limit.  We guess they are equal then.
    // A fixed limit has the problem of still taking an awful long time.
    // Reduce the limit every time running into it. That should work fine for
    // deeply linked structures that are not recursively linked and catch
    // recursiveness quickly.
    if (!recursive)
	tv_equal_recurse_limit = 1000;
    if (recursive_cnt >= tv_equal_recurse_limit)
    {
	--tv_equal_recurse_limit;
	return TRUE;
    }

    // For VAR_FUNC and VAR_PARTIAL compare the function name, bound dict and
    // arguments.
    if ((tv1->v_type == VAR_FUNC
		|| (tv1->v_type == VAR_PARTIAL && tv1->vval.v_partial != NULL))
	    && (tv2->v_type == VAR_FUNC
		|| (tv2->v_type == VAR_PARTIAL && tv2->vval.v_partial != NULL)))
    {
	++recursive_cnt;
	r = func_equal(tv1, tv2, ic);
	--recursive_cnt;
	return r;
    }

    if (tv1->v_type != tv2->v_type)
	return FALSE;

    switch (tv1->v_type)
    {
	case VAR_LIST:
	    ++recursive_cnt;
	    r = list_equal(tv1->vval.v_list, tv2->vval.v_list, ic, TRUE);
	    --recursive_cnt;
	    return r;

	case VAR_DICT:
	    ++recursive_cnt;
	    r = dict_equal(tv1->vval.v_dict, tv2->vval.v_dict, ic, TRUE);
	    --recursive_cnt;
	    return r;

	case VAR_BLOB:
	    return blob_equal(tv1->vval.v_blob, tv2->vval.v_blob);

	case VAR_NUMBER:
	    return tv1->vval.v_number == tv2->vval.v_number;

	case VAR_STRING:
	    s1 = tv_get_string_buf(tv1, buf1);
	    s2 = tv_get_string_buf(tv2, buf2);
	    return ((ic ? MB_STRICMP(s1, s2) : STRCMP(s1, s2)) == 0);

	case VAR_SPECIAL:
	    return tv1->vval.v_number == tv2->vval.v_number;

	case VAR_FLOAT:
#ifdef FEAT_FLOAT
	    return tv1->vval.v_float == tv2->vval.v_float;
#endif
	case VAR_JOB:
#ifdef FEAT_JOB_CHANNEL
	    return tv1->vval.v_job == tv2->vval.v_job;
#endif
	case VAR_CHANNEL:
#ifdef FEAT_JOB_CHANNEL
	    return tv1->vval.v_channel == tv2->vval.v_channel;
#endif
	case VAR_FUNC:
	case VAR_PARTIAL:
	case VAR_UNKNOWN:
	    break;
    }

    // VAR_UNKNOWN can be the result of a invalid expression, let's say it
    // does not equal anything, not even itself.
    return FALSE;
}

/*
 * Return the next (unique) copy ID.
 * Used for serializing nested structures.
 */
    int
get_copyID(void)
{
    current_copyID += COPYID_INC;
    return current_copyID;
}

/*
 * Garbage collection for lists and dictionaries.
 *
 * We use reference counts to be able to free most items right away when they
 * are no longer used.  But for composite items it's possible that it becomes
 * unused while the reference count is > 0: When there is a recursive
 * reference.  Example:
 *	:let l = [1, 2, 3]
 *	:let d = {9: l}
 *	:let l[1] = d
 *
 * Since this is quite unusual we handle this with garbage collection: every
 * once in a while find out which lists and dicts are not referenced from any
 * variable.
 *
 * Here is a good reference text about garbage collection (refers to Python
 * but it applies to all reference-counting mechanisms):
 *	http://python.ca/nas/python/gc/
 */

/*
 * Do garbage collection for lists and dicts.
 * When "testing" is TRUE this is called from test_garbagecollect_now().
 * Return TRUE if some memory was freed.
 */
    int
garbage_collect(int testing)
{
    int		copyID;
    int		abort = FALSE;
    buf_T	*buf;
    win_T	*wp;
    int		did_free = FALSE;
    tabpage_T	*tp;

    if (!testing)
    {
	// Only do this once.
	want_garbage_collect = FALSE;
	may_garbage_collect = FALSE;
	garbage_collect_at_exit = FALSE;
    }

    // We advance by two because we add one for items referenced through
    // previous_funccal.
    copyID = get_copyID();

    /*
     * 1. Go through all accessible variables and mark all lists and dicts
     *    with copyID.
     */

    // Don't free variables in the previous_funccal list unless they are only
    // referenced through previous_funccal.  This must be first, because if
    // the item is referenced elsewhere the funccal must not be freed.
    abort = abort || set_ref_in_previous_funccal(copyID);

    // script-local variables
    abort = abort || garbage_collect_scriptvars(copyID);

    // buffer-local variables
    FOR_ALL_BUFFERS(buf)
	abort = abort || set_ref_in_item(&buf->b_bufvar.di_tv, copyID,
								  NULL, NULL);

    // window-local variables
    FOR_ALL_TAB_WINDOWS(tp, wp)
	abort = abort || set_ref_in_item(&wp->w_winvar.di_tv, copyID,
								  NULL, NULL);
    if (aucmd_win != NULL)
	abort = abort || set_ref_in_item(&aucmd_win->w_winvar.di_tv, copyID,
								  NULL, NULL);
#ifdef FEAT_PROP_POPUP
    for (wp = first_popupwin; wp != NULL; wp = wp->w_next)
	abort = abort || set_ref_in_item(&wp->w_winvar.di_tv, copyID,
								  NULL, NULL);
    FOR_ALL_TABPAGES(tp)
	for (wp = tp->tp_first_popupwin; wp != NULL; wp = wp->w_next)
		abort = abort || set_ref_in_item(&wp->w_winvar.di_tv, copyID,
								  NULL, NULL);
#endif

    // tabpage-local variables
    FOR_ALL_TABPAGES(tp)
	abort = abort || set_ref_in_item(&tp->tp_winvar.di_tv, copyID,
								  NULL, NULL);
    // global variables
    abort = abort || garbage_collect_globvars(copyID);

    // function-local variables
    abort = abort || set_ref_in_call_stack(copyID);

    // named functions (matters for closures)
    abort = abort || set_ref_in_functions(copyID);

    // function call arguments, if v:testing is set.
    abort = abort || set_ref_in_func_args(copyID);

    // v: vars
    abort = abort || garbage_collect_vimvars(copyID);

    // callbacks in buffers
    abort = abort || set_ref_in_buffers(copyID);

#ifdef FEAT_LUA
    abort = abort || set_ref_in_lua(copyID);
#endif

#ifdef FEAT_PYTHON
    abort = abort || set_ref_in_python(copyID);
#endif

#ifdef FEAT_PYTHON3
    abort = abort || set_ref_in_python3(copyID);
#endif

#ifdef FEAT_JOB_CHANNEL
    abort = abort || set_ref_in_channel(copyID);
    abort = abort || set_ref_in_job(copyID);
#endif
#ifdef FEAT_NETBEANS_INTG
    abort = abort || set_ref_in_nb_channel(copyID);
#endif

#ifdef FEAT_TIMERS
    abort = abort || set_ref_in_timer(copyID);
#endif

#ifdef FEAT_QUICKFIX
    abort = abort || set_ref_in_quickfix(copyID);
#endif

#ifdef FEAT_TERMINAL
    abort = abort || set_ref_in_term(copyID);
#endif

#ifdef FEAT_PROP_POPUP
    abort = abort || set_ref_in_popups(copyID);
#endif

    if (!abort)
    {
	/*
	 * 2. Free lists and dictionaries that are not referenced.
	 */
	did_free = free_unref_items(copyID);

	/*
	 * 3. Check if any funccal can be freed now.
	 *    This may call us back recursively.
	 */
	free_unref_funccal(copyID, testing);
    }
    else if (p_verbose > 0)
    {
	verb_msg(_("Not enough memory to set references, garbage collection aborted!"));
    }

    return did_free;
}

/*
 * Free lists, dictionaries, channels and jobs that are no longer referenced.
 */
    static int
free_unref_items(int copyID)
{
    int		did_free = FALSE;

    // Let all "free" functions know that we are here.  This means no
    // dictionaries, lists, channels or jobs are to be freed, because we will
    // do that here.
    in_free_unref_items = TRUE;

    /*
     * PASS 1: free the contents of the items.  We don't free the items
     * themselves yet, so that it is possible to decrement refcount counters
     */

    // Go through the list of dicts and free items without the copyID.
    did_free |= dict_free_nonref(copyID);

    // Go through the list of lists and free items without the copyID.
    did_free |= list_free_nonref(copyID);

#ifdef FEAT_JOB_CHANNEL
    // Go through the list of jobs and free items without the copyID. This
    // must happen before doing channels, because jobs refer to channels, but
    // the reference from the channel to the job isn't tracked.
    did_free |= free_unused_jobs_contents(copyID, COPYID_MASK);

    // Go through the list of channels and free items without the copyID.
    did_free |= free_unused_channels_contents(copyID, COPYID_MASK);
#endif

    /*
     * PASS 2: free the items themselves.
     */
    dict_free_items(copyID);
    list_free_items(copyID);

#ifdef FEAT_JOB_CHANNEL
    // Go through the list of jobs and free items without the copyID. This
    // must happen before doing channels, because jobs refer to channels, but
    // the reference from the channel to the job isn't tracked.
    free_unused_jobs(copyID, COPYID_MASK);

    // Go through the list of channels and free items without the copyID.
    free_unused_channels(copyID, COPYID_MASK);
#endif

    in_free_unref_items = FALSE;

    return did_free;
}

/*
 * Mark all lists and dicts referenced through hashtab "ht" with "copyID".
 * "list_stack" is used to add lists to be marked.  Can be NULL.
 *
 * Returns TRUE if setting references failed somehow.
 */
    int
set_ref_in_ht(hashtab_T *ht, int copyID, list_stack_T **list_stack)
{
    int		todo;
    int		abort = FALSE;
    hashitem_T	*hi;
    hashtab_T	*cur_ht;
    ht_stack_T	*ht_stack = NULL;
    ht_stack_T	*tempitem;

    cur_ht = ht;
    for (;;)
    {
	if (!abort)
	{
	    // Mark each item in the hashtab.  If the item contains a hashtab
	    // it is added to ht_stack, if it contains a list it is added to
	    // list_stack.
	    todo = (int)cur_ht->ht_used;
	    for (hi = cur_ht->ht_array; todo > 0; ++hi)
		if (!HASHITEM_EMPTY(hi))
		{
		    --todo;
		    abort = abort || set_ref_in_item(&HI2DI(hi)->di_tv, copyID,
						       &ht_stack, list_stack);
		}
	}

	if (ht_stack == NULL)
	    break;

	// take an item from the stack
	cur_ht = ht_stack->ht;
	tempitem = ht_stack;
	ht_stack = ht_stack->prev;
	free(tempitem);
    }

    return abort;
}

/*
 * Mark a dict and its items with "copyID".
 * Returns TRUE if setting references failed somehow.
 */
    int
set_ref_in_dict(dict_T *d, int copyID)
{
    if (d != NULL && d->dv_copyID != copyID)
    {
	d->dv_copyID = copyID;
	return set_ref_in_ht(&d->dv_hashtab, copyID, NULL);
    }
    return FALSE;
}

/*
 * Mark a list and its items with "copyID".
 * Returns TRUE if setting references failed somehow.
 */
    int
set_ref_in_list(list_T *ll, int copyID)
{
    if (ll != NULL && ll->lv_copyID != copyID)
    {
	ll->lv_copyID = copyID;
	return set_ref_in_list_items(ll, copyID, NULL);
    }
    return FALSE;
}

/*
 * Mark all lists and dicts referenced through list "l" with "copyID".
 * "ht_stack" is used to add hashtabs to be marked.  Can be NULL.
 *
 * Returns TRUE if setting references failed somehow.
 */
    int
set_ref_in_list_items(list_T *l, int copyID, ht_stack_T **ht_stack)
{
    listitem_T	 *li;
    int		 abort = FALSE;
    list_T	 *cur_l;
    list_stack_T *list_stack = NULL;
    list_stack_T *tempitem;

    cur_l = l;
    for (;;)
    {
	if (!abort)
	    // Mark each item in the list.  If the item contains a hashtab
	    // it is added to ht_stack, if it contains a list it is added to
	    // list_stack.
	    for (li = cur_l->lv_first; !abort && li != NULL; li = li->li_next)
		abort = abort || set_ref_in_item(&li->li_tv, copyID,
						       ht_stack, &list_stack);
	if (list_stack == NULL)
	    break;

	// take an item from the stack
	cur_l = list_stack->list;
	tempitem = list_stack;
	list_stack = list_stack->prev;
	free(tempitem);
    }

    return abort;
}

/*
 * Mark all lists and dicts referenced through typval "tv" with "copyID".
 * "list_stack" is used to add lists to be marked.  Can be NULL.
 * "ht_stack" is used to add hashtabs to be marked.  Can be NULL.
 *
 * Returns TRUE if setting references failed somehow.
 */
    int
set_ref_in_item(
    typval_T	    *tv,
    int		    copyID,
    ht_stack_T	    **ht_stack,
    list_stack_T    **list_stack)
{
    int		abort = FALSE;

    if (tv->v_type == VAR_DICT)
    {
	dict_T	*dd = tv->vval.v_dict;

	if (dd != NULL && dd->dv_copyID != copyID)
	{
	    // Didn't see this dict yet.
	    dd->dv_copyID = copyID;
	    if (ht_stack == NULL)
	    {
		abort = set_ref_in_ht(&dd->dv_hashtab, copyID, list_stack);
	    }
	    else
	    {
		ht_stack_T *newitem = (ht_stack_T*)malloc(sizeof(ht_stack_T));
		if (newitem == NULL)
		    abort = TRUE;
		else
		{
		    newitem->ht = &dd->dv_hashtab;
		    newitem->prev = *ht_stack;
		    *ht_stack = newitem;
		}
	    }
	}
    }
    else if (tv->v_type == VAR_LIST)
    {
	list_T	*ll = tv->vval.v_list;

	if (ll != NULL && ll->lv_copyID != copyID)
	{
	    // Didn't see this list yet.
	    ll->lv_copyID = copyID;
	    if (list_stack == NULL)
	    {
		abort = set_ref_in_list_items(ll, copyID, ht_stack);
	    }
	    else
	    {
		list_stack_T *newitem = (list_stack_T*)malloc(
							sizeof(list_stack_T));
		if (newitem == NULL)
		    abort = TRUE;
		else
		{
		    newitem->list = ll;
		    newitem->prev = *list_stack;
		    *list_stack = newitem;
		}
	    }
	}
    }
    else if (tv->v_type == VAR_FUNC)
    {
	abort = set_ref_in_func(tv->vval.v_string, NULL, copyID);
    }
    else if (tv->v_type == VAR_PARTIAL)
    {
	partial_T	*pt = tv->vval.v_partial;
	int		i;

	// A partial does not have a copyID, because it cannot contain itself.
	if (pt != NULL)
	{
	    abort = set_ref_in_func(pt->pt_name, pt->pt_func, copyID);

	    if (pt->pt_dict != NULL)
	    {
		typval_T dtv;

		dtv.v_type = VAR_DICT;
		dtv.vval.v_dict = pt->pt_dict;
		set_ref_in_item(&dtv, copyID, ht_stack, list_stack);
	    }

	    for (i = 0; i < pt->pt_argc; ++i)
		abort = abort || set_ref_in_item(&pt->pt_argv[i], copyID,
							ht_stack, list_stack);
	}
    }
#ifdef FEAT_JOB_CHANNEL
    else if (tv->v_type == VAR_JOB)
    {
	job_T	    *job = tv->vval.v_job;
	typval_T    dtv;

	if (job != NULL && job->jv_copyID != copyID)
	{
	    job->jv_copyID = copyID;
	    if (job->jv_channel != NULL)
	    {
		dtv.v_type = VAR_CHANNEL;
		dtv.vval.v_channel = job->jv_channel;
		set_ref_in_item(&dtv, copyID, ht_stack, list_stack);
	    }
	    if (job->jv_exit_cb.cb_partial != NULL)
	    {
		dtv.v_type = VAR_PARTIAL;
		dtv.vval.v_partial = job->jv_exit_cb.cb_partial;
		set_ref_in_item(&dtv, copyID, ht_stack, list_stack);
	    }
	}
    }
    else if (tv->v_type == VAR_CHANNEL)
    {
	channel_T   *ch =tv->vval.v_channel;
	ch_part_T   part;
	typval_T    dtv;
	jsonq_T	    *jq;
	cbq_T	    *cq;

	if (ch != NULL && ch->ch_copyID != copyID)
	{
	    ch->ch_copyID = copyID;
	    for (part = PART_SOCK; part < PART_COUNT; ++part)
	    {
		for (jq = ch->ch_part[part].ch_json_head.jq_next; jq != NULL;
							     jq = jq->jq_next)
		    set_ref_in_item(jq->jq_value, copyID, ht_stack, list_stack);
		for (cq = ch->ch_part[part].ch_cb_head.cq_next; cq != NULL;
							     cq = cq->cq_next)
		    if (cq->cq_callback.cb_partial != NULL)
		    {
			dtv.v_type = VAR_PARTIAL;
			dtv.vval.v_partial = cq->cq_callback.cb_partial;
			set_ref_in_item(&dtv, copyID, ht_stack, list_stack);
		    }
		if (ch->ch_part[part].ch_callback.cb_partial != NULL)
		{
		    dtv.v_type = VAR_PARTIAL;
		    dtv.vval.v_partial =
				      ch->ch_part[part].ch_callback.cb_partial;
		    set_ref_in_item(&dtv, copyID, ht_stack, list_stack);
		}
	    }
	    if (ch->ch_callback.cb_partial != NULL)
	    {
		dtv.v_type = VAR_PARTIAL;
		dtv.vval.v_partial = ch->ch_callback.cb_partial;
		set_ref_in_item(&dtv, copyID, ht_stack, list_stack);
	    }
	    if (ch->ch_close_cb.cb_partial != NULL)
	    {
		dtv.v_type = VAR_PARTIAL;
		dtv.vval.v_partial = ch->ch_close_cb.cb_partial;
		set_ref_in_item(&dtv, copyID, ht_stack, list_stack);
	    }
	}
    }
#endif
    return abort;
}

/*
 * Return a string with the string representation of a variable.
 * If the memory is allocated "tofree" is set to it, otherwise NULL.
 * "numbuf" is used for a number.
 * When "copyID" is not NULL replace recursive lists and dicts with "...".
 * When both "echo_style" and "composite_val" are FALSE, put quotes around
 * stings as "string()", otherwise does not put quotes around strings, as
 * ":echo" displays values.
 * When "restore_copyID" is FALSE, repeated items in dictionaries and lists
 * are replaced with "...".
 * May return NULL.
 */
    char_u *
echo_string_core(
    typval_T	*tv,
    char_u	**tofree,
    char_u	*numbuf,
    int		copyID,
    int		echo_style,
    int		restore_copyID,
    int		composite_val)
{
    static int	recurse = 0;
    char_u	*r = NULL;

    if (recurse >= DICT_MAXNEST)
    {
	if (!did_echo_string_emsg)
	{
	    // Only give this message once for a recursive call to avoid
	    // flooding the user with errors.  And stop iterating over lists
	    // and dicts.
	    did_echo_string_emsg = TRUE;
	    emsg(_("E724: variable nested too deep for displaying"));
	}
	*tofree = NULL;
	return (char_u *)"{E724}";
    }
    ++recurse;

    switch (tv->v_type)
    {
	case VAR_STRING:
	    if (echo_style && !composite_val)
	    {
		*tofree = NULL;
		r = tv->vval.v_string;
		if (r == NULL)
		    r = (char_u *)"";
	    }
	    else
	    {
		*tofree = string_quote(tv->vval.v_string, FALSE);
		r = *tofree;
	    }
	    break;

	case VAR_FUNC:
	    if (echo_style)
	    {
		*tofree = NULL;
		r = tv->vval.v_string;
	    }
	    else
	    {
		*tofree = string_quote(tv->vval.v_string, TRUE);
		r = *tofree;
	    }
	    break;

	case VAR_PARTIAL:
	    {
		partial_T   *pt = tv->vval.v_partial;
		char_u	    *fname = string_quote(pt == NULL ? NULL
						    : partial_name(pt), FALSE);
		garray_T    ga;
		int	    i;
		char_u	    *tf;

		ga_init2(&ga, 1, 100);
		ga_concat(&ga, (char_u *)"function(");
		if (fname != NULL)
		{
		    ga_concat(&ga, fname);
		    vim_free(fname);
		}
		if (pt != NULL && pt->pt_argc > 0)
		{
		    ga_concat(&ga, (char_u *)", [");
		    for (i = 0; i < pt->pt_argc; ++i)
		    {
			if (i > 0)
			    ga_concat(&ga, (char_u *)", ");
			ga_concat(&ga,
			     tv2string(&pt->pt_argv[i], &tf, numbuf, copyID));
			vim_free(tf);
		    }
		    ga_concat(&ga, (char_u *)"]");
		}
		if (pt != NULL && pt->pt_dict != NULL)
		{
		    typval_T dtv;

		    ga_concat(&ga, (char_u *)", ");
		    dtv.v_type = VAR_DICT;
		    dtv.vval.v_dict = pt->pt_dict;
		    ga_concat(&ga, tv2string(&dtv, &tf, numbuf, copyID));
		    vim_free(tf);
		}
		ga_concat(&ga, (char_u *)")");

		*tofree = ga.ga_data;
		r = *tofree;
		break;
	    }

	case VAR_BLOB:
	    r = blob2string(tv->vval.v_blob, tofree, numbuf);
	    break;

	case VAR_LIST:
	    if (tv->vval.v_list == NULL)
	    {
		*tofree = NULL;
		r = NULL;
	    }
	    else if (copyID != 0 && tv->vval.v_list->lv_copyID == copyID
		    && tv->vval.v_list->lv_len > 0)
	    {
		*tofree = NULL;
		r = (char_u *)"[...]";
	    }
	    else
	    {
		int old_copyID = tv->vval.v_list->lv_copyID;

		tv->vval.v_list->lv_copyID = copyID;
		*tofree = list2string(tv, copyID, restore_copyID);
		if (restore_copyID)
		    tv->vval.v_list->lv_copyID = old_copyID;
		r = *tofree;
	    }
	    break;

	case VAR_DICT:
	    if (tv->vval.v_dict == NULL)
	    {
		*tofree = NULL;
		r = NULL;
	    }
	    else if (copyID != 0 && tv->vval.v_dict->dv_copyID == copyID
		    && tv->vval.v_dict->dv_hashtab.ht_used != 0)
	    {
		*tofree = NULL;
		r = (char_u *)"{...}";
	    }
	    else
	    {
		int old_copyID = tv->vval.v_dict->dv_copyID;
		tv->vval.v_dict->dv_copyID = copyID;
		*tofree = dict2string(tv, copyID, restore_copyID);
		if (restore_copyID)
		    tv->vval.v_dict->dv_copyID = old_copyID;
		r = *tofree;
	    }
	    break;

	case VAR_NUMBER:
	case VAR_UNKNOWN:
	    *tofree = NULL;
	    r = tv_get_string_buf(tv, numbuf);
	    break;

	case VAR_JOB:
	case VAR_CHANNEL:
	    *tofree = NULL;
	    r = tv_get_string_buf(tv, numbuf);
	    if (composite_val)
	    {
		*tofree = string_quote(r, FALSE);
		r = *tofree;
	    }
	    break;

	case VAR_FLOAT:
#ifdef FEAT_FLOAT
	    *tofree = NULL;
	    vim_snprintf((char *)numbuf, NUMBUFLEN, "%g", tv->vval.v_float);
	    r = numbuf;
	    break;
#endif

	case VAR_SPECIAL:
	    *tofree = NULL;
	    r = (char_u *)get_var_special_name(tv->vval.v_number);
	    break;
    }

    if (--recurse == 0)
	did_echo_string_emsg = FALSE;
    return r;
}

/*
 * Return a string with the string representation of a variable.
 * If the memory is allocated "tofree" is set to it, otherwise NULL.
 * "numbuf" is used for a number.
 * Does not put quotes around strings, as ":echo" displays values.
 * When "copyID" is not NULL replace recursive lists and dicts with "...".
 * May return NULL.
 */
    char_u *
echo_string(
    typval_T	*tv,
    char_u	**tofree,
    char_u	*numbuf,
    int		copyID)
{
    return echo_string_core(tv, tofree, numbuf, copyID, TRUE, FALSE, FALSE);
}

/*
 * Return a string with the string representation of a variable.
 * If the memory is allocated "tofree" is set to it, otherwise NULL.
 * "numbuf" is used for a number.
 * Puts quotes around strings, so that they can be parsed back by eval().
 * May return NULL.
 */
    char_u *
tv2string(
    typval_T	*tv,
    char_u	**tofree,
    char_u	*numbuf,
    int		copyID)
{
    return echo_string_core(tv, tofree, numbuf, copyID, FALSE, TRUE, FALSE);
}

/*
 * Return string "str" in ' quotes, doubling ' characters.
 * If "str" is NULL an empty string is assumed.
 * If "function" is TRUE make it function('string').
 */
    char_u *
string_quote(char_u *str, int function)
{
    unsigned	len;
    char_u	*p, *r, *s;

    len = (function ? 13 : 3);
    if (str != NULL)
    {
	len += (unsigned)STRLEN(str);
	for (p = str; *p != NUL; MB_PTR_ADV(p))
	    if (*p == '\'')
		++len;
    }
    s = r = alloc(len);
    if (r != NULL)
    {
	if (function)
	{
	    STRCPY(r, "function('");
	    r += 10;
	}
	else
	    *r++ = '\'';
	if (str != NULL)
	    for (p = str; *p != NUL; )
	    {
		if (*p == '\'')
		    *r++ = '\'';
		MB_COPY_CHAR(p, r);
	    }
	*r++ = '\'';
	if (function)
	    *r++ = ')';
	*r++ = NUL;
    }
    return s;
}

#if defined(FEAT_FLOAT) || defined(PROTO)
/*
 * Convert the string "text" to a floating point number.
 * This uses strtod().  setlocale(LC_NUMERIC, "C") has been used to make sure
 * this always uses a decimal point.
 * Returns the length of the text that was consumed.
 */
    int
string2float(
    char_u	*text,
    float_T	*value)	    // result stored here
{
    char	*s = (char *)text;
    float_T	f;

    // MS-Windows does not deal with "inf" and "nan" properly.
    if (STRNICMP(text, "inf", 3) == 0)
    {
	*value = INFINITY;
	return 3;
    }
    if (STRNICMP(text, "-inf", 3) == 0)
    {
	*value = -INFINITY;
	return 4;
    }
    if (STRNICMP(text, "nan", 3) == 0)
    {
	*value = NAN;
	return 3;
    }
    f = strtod(s, &s);
    *value = f;
    return (int)((char_u *)s - text);
}
#endif

/*
 * Get the value of an environment variable.
 * "arg" is pointing to the '$'.  It is advanced to after the name.
 * If the environment variable was not set, silently assume it is empty.
 * Return FAIL if the name is invalid.
 */
    static int
get_env_tv(char_u **arg, typval_T *rettv, int evaluate)
{
    char_u	*string = NULL;
    int		len;
    int		cc;
    char_u	*name;
    int		mustfree = FALSE;

    ++*arg;
    name = *arg;
    len = get_env_len(arg);
    if (evaluate)
    {
	if (len == 0)
	    return FAIL; // invalid empty name

	cc = name[len];
	name[len] = NUL;
	// first try vim_getenv(), fast for normal environment vars
	string = vim_getenv(name, &mustfree);
	if (string != NULL && *string != NUL)
	{
	    if (!mustfree)
		string = vim_strsave(string);
	}
	else
	{
	    if (mustfree)
		vim_free(string);

	    // next try expanding things like $VIM and ${HOME}
	    string = expand_env_save(name - 1);
	    if (string != NULL && *string == '$')
		VIM_CLEAR(string);
	}
	name[len] = cc;

	rettv->v_type = VAR_STRING;
	rettv->vval.v_string = string;
    }

    return OK;
}

/*
 * Translate a String variable into a position.
 * Returns NULL when there is an error.
 */
    pos_T *
var2fpos(
    typval_T	*varp,
    int		dollar_lnum,	// TRUE when $ is last line
    int		*fnum)		// set to fnum for '0, 'A, etc.
{
    char_u		*name;
    static pos_T	pos;
    pos_T		*pp;

    // Argument can be [lnum, col, coladd].
    if (varp->v_type == VAR_LIST)
    {
	list_T		*l;
	int		len;
	int		error = FALSE;
	listitem_T	*li;

	l = varp->vval.v_list;
	if (l == NULL)
	    return NULL;

	// Get the line number
	pos.lnum = list_find_nr(l, 0L, &error);
	if (error || pos.lnum <= 0 || pos.lnum > curbuf->b_ml.ml_line_count)
	    return NULL;	// invalid line number

	// Get the column number
	pos.col = list_find_nr(l, 1L, &error);
	if (error)
	    return NULL;
	len = (long)STRLEN(ml_get(pos.lnum));

	// We accept "$" for the column number: last column.
	li = list_find(l, 1L);
	if (li != NULL && li->li_tv.v_type == VAR_STRING
		&& li->li_tv.vval.v_string != NULL
		&& STRCMP(li->li_tv.vval.v_string, "$") == 0)
	    pos.col = len + 1;

	// Accept a position up to the NUL after the line.
	if (pos.col == 0 || (int)pos.col > len + 1)
	    return NULL;	// invalid column number
	--pos.col;

	// Get the virtual offset.  Defaults to zero.
	pos.coladd = list_find_nr(l, 2L, &error);
	if (error)
	    pos.coladd = 0;

	return &pos;
    }

    name = tv_get_string_chk(varp);
    if (name == NULL)
	return NULL;
    if (name[0] == '.')				// cursor
	return &curwin->w_cursor;
    if (name[0] == 'v' && name[1] == NUL)	// Visual start
    {
	if (VIsual_active)
	    return &VIsual;
	return &curwin->w_cursor;
    }
    if (name[0] == '\'')			// mark
    {
	pp = getmark_buf_fnum(curbuf, name[1], FALSE, fnum);
	if (pp == NULL || pp == (pos_T *)-1 || pp->lnum <= 0)
	    return NULL;
	return pp;
    }

    pos.coladd = 0;

    if (name[0] == 'w' && dollar_lnum)
    {
	pos.col = 0;
	if (name[1] == '0')		// "w0": first visible line
	{
	    update_topline();
	    // In silent Ex mode topline is zero, but that's not a valid line
	    // number; use one instead.
	    pos.lnum = curwin->w_topline > 0 ? curwin->w_topline : 1;
	    return &pos;
	}
	else if (name[1] == '$')	// "w$": last visible line
	{
	    validate_botline();
	    // In silent Ex mode botline is zero, return zero then.
	    pos.lnum = curwin->w_botline > 0 ? curwin->w_botline - 1 : 0;
	    return &pos;
	}
    }
    else if (name[0] == '$')		// last column or line
    {
	if (dollar_lnum)
	{
	    pos.lnum = curbuf->b_ml.ml_line_count;
	    pos.col = 0;
	}
	else
	{
	    pos.lnum = curwin->w_cursor.lnum;
	    pos.col = (colnr_T)STRLEN(ml_get_curline());
	}
	return &pos;
    }
    return NULL;
}

/*
 * Convert list in "arg" into a position and optional file number.
 * When "fnump" is NULL there is no file number, only 3 items.
 * Note that the column is passed on as-is, the caller may want to decrement
 * it to use 1 for the first column.
 * Return FAIL when conversion is not possible, doesn't check the position for
 * validity.
 */
    int
list2fpos(
    typval_T	*arg,
    pos_T	*posp,
    int		*fnump,
    colnr_T	*curswantp)
{
    list_T	*l = arg->vval.v_list;
    long	i = 0;
    long	n;

    // List must be: [fnum, lnum, col, coladd, curswant], where "fnum" is only
    // there when "fnump" isn't NULL; "coladd" and "curswant" are optional.
    if (arg->v_type != VAR_LIST
	    || l == NULL
	    || l->lv_len < (fnump == NULL ? 2 : 3)
	    || l->lv_len > (fnump == NULL ? 4 : 5))
	return FAIL;

    if (fnump != NULL)
    {
	n = list_find_nr(l, i++, NULL);	// fnum
	if (n < 0)
	    return FAIL;
	if (n == 0)
	    n = curbuf->b_fnum;		// current buffer
	*fnump = n;
    }

    n = list_find_nr(l, i++, NULL);	// lnum
    if (n < 0)
	return FAIL;
    posp->lnum = n;

    n = list_find_nr(l, i++, NULL);	// col
    if (n < 0)
	return FAIL;
    posp->col = n;

    n = list_find_nr(l, i, NULL);	// off
    if (n < 0)
	posp->coladd = 0;
    else
	posp->coladd = n;

    if (curswantp != NULL)
	*curswantp = list_find_nr(l, i + 1, NULL);  // curswant

    return OK;
}

/*
 * Get the length of an environment variable name.
 * Advance "arg" to the first character after the name.
 * Return 0 for error.
 */
    int
get_env_len(char_u **arg)
{
    char_u	*p;
    int		len;

    for (p = *arg; vim_isIDc(*p); ++p)
	;
    if (p == *arg)	    // no name found
	return 0;

    len = (int)(p - *arg);
    *arg = p;
    return len;
}

/*
 * Get the length of the name of a function or internal variable.
 * "arg" is advanced to the first non-white character after the name.
 * Return 0 if something is wrong.
 */
    int
get_id_len(char_u **arg)
{
    char_u	*p;
    int		len;

    // Find the end of the name.
    for (p = *arg; eval_isnamec(*p); ++p)
    {
	if (*p == ':')
	{
	    // "s:" is start of "s:var", but "n:" is not and can be used in
	    // slice "[n:]".  Also "xx:" is not a namespace.
	    len = (int)(p - *arg);
	    if ((len == 1 && vim_strchr(NAMESPACE_CHAR, **arg) == NULL)
		    || len > 1)
		break;
	}
    }
    if (p == *arg)	    // no name found
	return 0;

    len = (int)(p - *arg);
    *arg = skipwhite(p);

    return len;
}

/*
 * Get the length of the name of a variable or function.
 * Only the name is recognized, does not handle ".key" or "[idx]".
 * "arg" is advanced to the first non-white character after the name.
 * Return -1 if curly braces expansion failed.
 * Return 0 if something else is wrong.
 * If the name contains 'magic' {}'s, expand them and return the
 * expanded name in an allocated string via 'alias' - caller must free.
 */
    int
get_name_len(
    char_u	**arg,
    char_u	**alias,
    int		evaluate,
    int		verbose)
{
    int		len;
    char_u	*p;
    char_u	*expr_start;
    char_u	*expr_end;

    *alias = NULL;  // default to no alias

    if ((*arg)[0] == K_SPECIAL && (*arg)[1] == KS_EXTRA
						  && (*arg)[2] == (int)KE_SNR)
    {
	// hard coded <SNR>, already translated
	*arg += 3;
	return get_id_len(arg) + 3;
    }
    len = eval_fname_script(*arg);
    if (len > 0)
    {
	// literal "<SID>", "s:" or "<SNR>"
	*arg += len;
    }

    /*
     * Find the end of the name; check for {} construction.
     */
    p = find_name_end(*arg, &expr_start, &expr_end,
					       len > 0 ? 0 : FNE_CHECK_START);
    if (expr_start != NULL)
    {
	char_u	*temp_string;

	if (!evaluate)
	{
	    len += (int)(p - *arg);
	    *arg = skipwhite(p);
	    return len;
	}

	/*
	 * Include any <SID> etc in the expanded string:
	 * Thus the -len here.
	 */
	temp_string = make_expanded_name(*arg - len, expr_start, expr_end, p);
	if (temp_string == NULL)
	    return -1;
	*alias = temp_string;
	*arg = skipwhite(p);
	return (int)STRLEN(temp_string);
    }

    len += get_id_len(arg);
    // Only give an error when there is something, otherwise it will be
    // reported at a higher level.
    if (len == 0 && verbose && **arg != NUL)
	semsg(_(e_invexpr2), *arg);

    return len;
}

/*
 * Find the end of a variable or function name, taking care of magic braces.
 * If "expr_start" is not NULL then "expr_start" and "expr_end" are set to the
 * start and end of the first magic braces item.
 * "flags" can have FNE_INCL_BR and FNE_CHECK_START.
 * Return a pointer to just after the name.  Equal to "arg" if there is no
 * valid name.
 */
    char_u *
find_name_end(
    char_u	*arg,
    char_u	**expr_start,
    char_u	**expr_end,
    int		flags)
{
    int		mb_nest = 0;
    int		br_nest = 0;
    char_u	*p;
    int		len;

    if (expr_start != NULL)
    {
	*expr_start = NULL;
	*expr_end = NULL;
    }

    // Quick check for valid starting character.
    if ((flags & FNE_CHECK_START) && !eval_isnamec1(*arg) && *arg != '{')
	return arg;

    for (p = arg; *p != NUL
		    && (eval_isnamec(*p)
			|| *p == '{'
			|| ((flags & FNE_INCL_BR) && (*p == '[' || *p == '.'))
			|| mb_nest != 0
			|| br_nest != 0); MB_PTR_ADV(p))
    {
	if (*p == '\'')
	{
	    // skip over 'string' to avoid counting [ and ] inside it.
	    for (p = p + 1; *p != NUL && *p != '\''; MB_PTR_ADV(p))
		;
	    if (*p == NUL)
		break;
	}
	else if (*p == '"')
	{
	    // skip over "str\"ing" to avoid counting [ and ] inside it.
	    for (p = p + 1; *p != NUL && *p != '"'; MB_PTR_ADV(p))
		if (*p == '\\' && p[1] != NUL)
		    ++p;
	    if (*p == NUL)
		break;
	}
	else if (br_nest == 0 && mb_nest == 0 && *p == ':')
	{
	    // "s:" is start of "s:var", but "n:" is not and can be used in
	    // slice "[n:]".  Also "xx:" is not a namespace. But {ns}: is.
	    len = (int)(p - arg);
	    if ((len == 1 && vim_strchr(NAMESPACE_CHAR, *arg) == NULL)
		    || (len > 1 && p[-1] != '}'))
		break;
	}

	if (mb_nest == 0)
	{
	    if (*p == '[')
		++br_nest;
	    else if (*p == ']')
		--br_nest;
	}

	if (br_nest == 0)
	{
	    if (*p == '{')
	    {
		mb_nest++;
		if (expr_start != NULL && *expr_start == NULL)
		    *expr_start = p;
	    }
	    else if (*p == '}')
	    {
		mb_nest--;
		if (expr_start != NULL && mb_nest == 0 && *expr_end == NULL)
		    *expr_end = p;
	    }
	}
    }

    return p;
}

/*
 * Expands out the 'magic' {}'s in a variable/function name.
 * Note that this can call itself recursively, to deal with
 * constructs like foo{bar}{baz}{bam}
 * The four pointer arguments point to "foo{expre}ss{ion}bar"
 *			"in_start"      ^
 *			"expr_start"	   ^
 *			"expr_end"		 ^
 *			"in_end"			    ^
 *
 * Returns a new allocated string, which the caller must free.
 * Returns NULL for failure.
 */
    static char_u *
make_expanded_name(
    char_u	*in_start,
    char_u	*expr_start,
    char_u	*expr_end,
    char_u	*in_end)
{
    char_u	c1;
    char_u	*retval = NULL;
    char_u	*temp_result;
    char_u	*nextcmd = NULL;

    if (expr_end == NULL || in_end == NULL)
	return NULL;
    *expr_start	= NUL;
    *expr_end = NUL;
    c1 = *in_end;
    *in_end = NUL;

    temp_result = eval_to_string(expr_start + 1, &nextcmd, FALSE);
    if (temp_result != NULL && nextcmd == NULL)
    {
	retval = alloc(STRLEN(temp_result) + (expr_start - in_start)
						   + (in_end - expr_end) + 1);
	if (retval != NULL)
	{
	    STRCPY(retval, in_start);
	    STRCAT(retval, temp_result);
	    STRCAT(retval, expr_end + 1);
	}
    }
    vim_free(temp_result);

    *in_end = c1;		// put char back for error messages
    *expr_start = '{';
    *expr_end = '}';

    if (retval != NULL)
    {
	temp_result = find_name_end(retval, &expr_start, &expr_end, 0);
	if (expr_start != NULL)
	{
	    // Further expansion!
	    temp_result = make_expanded_name(retval, expr_start,
						       expr_end, temp_result);
	    vim_free(retval);
	    retval = temp_result;
	}
    }

    return retval;
}

/*
 * Return TRUE if character "c" can be used in a variable or function name.
 * Does not include '{' or '}' for magic braces.
 */
    int
eval_isnamec(int c)
{
    return (ASCII_ISALNUM(c) || c == '_' || c == ':' || c == AUTOLOAD_CHAR);
}

/*
 * Return TRUE if character "c" can be used as the first character in a
 * variable or function name (excluding '{' and '}').
 */
    int
eval_isnamec1(int c)
{
    return (ASCII_ISALPHA(c) || c == '_');
}

/*
 * Handle:
 * - expr[expr], expr[expr:expr] subscript
 * - ".name" lookup
 * - function call with Funcref variable: func(expr)
 * - method call: var->method()
 *
 * Can all be combined in any order: dict.func(expr)[idx]['func'](expr)->len()
 */
    int
handle_subscript(
    char_u	**arg,
    typval_T	*rettv,
    int		evaluate,	// do more than finding the end
    int		verbose,	// give error messages
    char_u	*start_leader,	// start of '!' and '-' prefixes
    char_u	**end_leaderp)  // end of '!' and '-' prefixes
{
    int		ret = OK;
    dict_T	*selfdict = NULL;

    // "." is ".name" lookup when we found a dict or when evaluating and
    // scriptversion is at least 2, where string concatenation is "..".
    while (ret == OK
	    && (((**arg == '['
		    || (**arg == '.' && (rettv->v_type == VAR_DICT
			|| (!evaluate
			    && (*arg)[1] != '.'
			    && current_sctx.sc_version >= 2)))
		    || (**arg == '(' && (!evaluate || rettv->v_type == VAR_FUNC
					    || rettv->v_type == VAR_PARTIAL)))
		&& !VIM_ISWHITE(*(*arg - 1)))
	    || (**arg == '-' && (*arg)[1] == '>')))
    {
	if (**arg == '(')
	{
	    ret = call_func_rettv(arg, rettv, evaluate, selfdict, NULL);

	    // Stop the expression evaluation when immediately aborting on
	    // error, or when an interrupt occurred or an exception was thrown
	    // but not caught.
	    if (aborting())
	    {
		if (ret == OK)
		    clear_tv(rettv);
		ret = FAIL;
	    }
	    dict_unref(selfdict);
	    selfdict = NULL;
	}
	else if (**arg == '-')
	{
	    // Expression "-1.0->method()" applies the leader "-" before
	    // applying ->.
	    if (evaluate && *end_leaderp > start_leader)
		ret = eval7_leader(rettv, start_leader, end_leaderp);
	    if (ret == OK)
	    {
		if ((*arg)[2] == '{')
		    // expr->{lambda}()
		    ret = eval_lambda(arg, rettv, evaluate, verbose);
		else
		    // expr->name()
		    ret = eval_method(arg, rettv, evaluate, verbose);
	    }
	}
	else // **arg == '[' || **arg == '.'
	{
	    dict_unref(selfdict);
	    if (rettv->v_type == VAR_DICT)
	    {
		selfdict = rettv->vval.v_dict;
		if (selfdict != NULL)
		    ++selfdict->dv_refcount;
	    }
	    else
		selfdict = NULL;
	    if (eval_index(arg, rettv, evaluate, verbose) == FAIL)
	    {
		clear_tv(rettv);
		ret = FAIL;
	    }
	}
    }

    // Turn "dict.Func" into a partial for "Func" bound to "dict".
    // Don't do this when "Func" is already a partial that was bound
    // explicitly (pt_auto is FALSE).
    if (selfdict != NULL
	    && (rettv->v_type == VAR_FUNC
		|| (rettv->v_type == VAR_PARTIAL
		    && (rettv->vval.v_partial->pt_auto
			|| rettv->vval.v_partial->pt_dict == NULL))))
	selfdict = make_partial(selfdict, rettv);

    dict_unref(selfdict);
    return ret;
}

/*
 * Allocate memory for a variable type-value, and make it empty (0 or NULL
 * value).
 */
    typval_T *
alloc_tv(void)
{
    return ALLOC_CLEAR_ONE(typval_T);
}

/*
 * Allocate memory for a variable type-value, and assign a string to it.
 * The string "s" must have been allocated, it is consumed.
 * Return NULL for out of memory, the variable otherwise.
 */
    typval_T *
alloc_string_tv(char_u *s)
{
    typval_T	*rettv;

    rettv = alloc_tv();
    if (rettv != NULL)
    {
	rettv->v_type = VAR_STRING;
	rettv->vval.v_string = s;
    }
    else
	vim_free(s);
    return rettv;
}

/*
 * Free the memory for a variable type-value.
 */
    void
free_tv(typval_T *varp)
{
    if (varp != NULL)
    {
	switch (varp->v_type)
	{
	    case VAR_FUNC:
		func_unref(varp->vval.v_string);
		// FALLTHROUGH
	    case VAR_STRING:
		vim_free(varp->vval.v_string);
		break;
	    case VAR_PARTIAL:
		partial_unref(varp->vval.v_partial);
		break;
	    case VAR_BLOB:
		blob_unref(varp->vval.v_blob);
		break;
	    case VAR_LIST:
		list_unref(varp->vval.v_list);
		break;
	    case VAR_DICT:
		dict_unref(varp->vval.v_dict);
		break;
	    case VAR_JOB:
#ifdef FEAT_JOB_CHANNEL
		job_unref(varp->vval.v_job);
		break;
#endif
	    case VAR_CHANNEL:
#ifdef FEAT_JOB_CHANNEL
		channel_unref(varp->vval.v_channel);
		break;
#endif
	    case VAR_NUMBER:
	    case VAR_FLOAT:
	    case VAR_UNKNOWN:
	    case VAR_SPECIAL:
		break;
	}
	vim_free(varp);
    }
}

/*
 * Free the memory for a variable value and set the value to NULL or 0.
 */
    void
clear_tv(typval_T *varp)
{
    if (varp != NULL)
    {
	switch (varp->v_type)
	{
	    case VAR_FUNC:
		func_unref(varp->vval.v_string);
		// FALLTHROUGH
	    case VAR_STRING:
		VIM_CLEAR(varp->vval.v_string);
		break;
	    case VAR_PARTIAL:
		partial_unref(varp->vval.v_partial);
		varp->vval.v_partial = NULL;
		break;
	    case VAR_BLOB:
		blob_unref(varp->vval.v_blob);
		varp->vval.v_blob = NULL;
		break;
	    case VAR_LIST:
		list_unref(varp->vval.v_list);
		varp->vval.v_list = NULL;
		break;
	    case VAR_DICT:
		dict_unref(varp->vval.v_dict);
		varp->vval.v_dict = NULL;
		break;
	    case VAR_NUMBER:
	    case VAR_SPECIAL:
		varp->vval.v_number = 0;
		break;
	    case VAR_FLOAT:
#ifdef FEAT_FLOAT
		varp->vval.v_float = 0.0;
		break;
#endif
	    case VAR_JOB:
#ifdef FEAT_JOB_CHANNEL
		job_unref(varp->vval.v_job);
		varp->vval.v_job = NULL;
#endif
		break;
	    case VAR_CHANNEL:
#ifdef FEAT_JOB_CHANNEL
		channel_unref(varp->vval.v_channel);
		varp->vval.v_channel = NULL;
#endif
	    case VAR_UNKNOWN:
		break;
	}
	varp->v_lock = 0;
    }
}

/*
 * Set the value of a variable to NULL without freeing items.
 */
    void
init_tv(typval_T *varp)
{
    if (varp != NULL)
	vim_memset(varp, 0, sizeof(typval_T));
}

/*
 * Get the number value of a variable.
 * If it is a String variable, uses vim_str2nr().
 * For incompatible types, return 0.
 * tv_get_number_chk() is similar to tv_get_number(), but informs the
 * caller of incompatible types: it sets *denote to TRUE if "denote"
 * is not NULL or returns -1 otherwise.
 */
    varnumber_T
tv_get_number(typval_T *varp)
{
    int		error = FALSE;

    return tv_get_number_chk(varp, &error);	// return 0L on error
}

    varnumber_T
tv_get_number_chk(typval_T *varp, int *denote)
{
    varnumber_T	n = 0L;

    switch (varp->v_type)
    {
	case VAR_NUMBER:
	    return varp->vval.v_number;
	case VAR_FLOAT:
#ifdef FEAT_FLOAT
	    emsg(_("E805: Using a Float as a Number"));
	    break;
#endif
	case VAR_FUNC:
	case VAR_PARTIAL:
	    emsg(_("E703: Using a Funcref as a Number"));
	    break;
	case VAR_STRING:
	    if (varp->vval.v_string != NULL)
		vim_str2nr(varp->vval.v_string, NULL, NULL,
					    STR2NR_ALL, &n, NULL, 0, FALSE);
	    return n;
	case VAR_LIST:
	    emsg(_("E745: Using a List as a Number"));
	    break;
	case VAR_DICT:
	    emsg(_("E728: Using a Dictionary as a Number"));
	    break;
	case VAR_SPECIAL:
	    return varp->vval.v_number == VVAL_TRUE ? 1 : 0;
	    break;
	case VAR_JOB:
#ifdef FEAT_JOB_CHANNEL
	    emsg(_("E910: Using a Job as a Number"));
	    break;
#endif
	case VAR_CHANNEL:
#ifdef FEAT_JOB_CHANNEL
	    emsg(_("E913: Using a Channel as a Number"));
	    break;
#endif
	case VAR_BLOB:
	    emsg(_("E974: Using a Blob as a Number"));
	    break;
	case VAR_UNKNOWN:
	    internal_error("tv_get_number(UNKNOWN)");
	    break;
    }
    if (denote == NULL)		// useful for values that must be unsigned
	n = -1;
    else
	*denote = TRUE;
    return n;
}

#ifdef FEAT_FLOAT
    float_T
tv_get_float(typval_T *varp)
{
    switch (varp->v_type)
    {
	case VAR_NUMBER:
	    return (float_T)(varp->vval.v_number);
	case VAR_FLOAT:
	    return varp->vval.v_float;
	case VAR_FUNC:
	case VAR_PARTIAL:
	    emsg(_("E891: Using a Funcref as a Float"));
	    break;
	case VAR_STRING:
	    emsg(_("E892: Using a String as a Float"));
	    break;
	case VAR_LIST:
	    emsg(_("E893: Using a List as a Float"));
	    break;
	case VAR_DICT:
	    emsg(_("E894: Using a Dictionary as a Float"));
	    break;
	case VAR_SPECIAL:
	    emsg(_("E907: Using a special value as a Float"));
	    break;
	case VAR_JOB:
# ifdef FEAT_JOB_CHANNEL
	    emsg(_("E911: Using a Job as a Float"));
	    break;
# endif
	case VAR_CHANNEL:
# ifdef FEAT_JOB_CHANNEL
	    emsg(_("E914: Using a Channel as a Float"));
	    break;
# endif
	case VAR_BLOB:
	    emsg(_("E975: Using a Blob as a Float"));
	    break;
	case VAR_UNKNOWN:
	    internal_error("tv_get_float(UNKNOWN)");
	    break;
    }
    return 0;
}
#endif

/*
 * Get the string value of a variable.
 * If it is a Number variable, the number is converted into a string.
 * tv_get_string() uses a single, static buffer.  YOU CAN ONLY USE IT ONCE!
 * tv_get_string_buf() uses a given buffer.
 * If the String variable has never been set, return an empty string.
 * Never returns NULL;
 * tv_get_string_chk() and tv_get_string_buf_chk() are similar, but return
 * NULL on error.
 */
    char_u *
tv_get_string(typval_T *varp)
{
    static char_u   mybuf[NUMBUFLEN];

    return tv_get_string_buf(varp, mybuf);
}

    char_u *
tv_get_string_buf(typval_T *varp, char_u *buf)
{
    char_u	*res =  tv_get_string_buf_chk(varp, buf);

    return res != NULL ? res : (char_u *)"";
}

/*
 * Careful: This uses a single, static buffer.  YOU CAN ONLY USE IT ONCE!
 */
    char_u *
tv_get_string_chk(typval_T *varp)
{
    static char_u   mybuf[NUMBUFLEN];

    return tv_get_string_buf_chk(varp, mybuf);
}

    char_u *
tv_get_string_buf_chk(typval_T *varp, char_u *buf)
{
    switch (varp->v_type)
    {
	case VAR_NUMBER:
	    vim_snprintf((char *)buf, NUMBUFLEN, "%lld",
					    (long_long_T)varp->vval.v_number);
	    return buf;
	case VAR_FUNC:
	case VAR_PARTIAL:
	    emsg(_("E729: using Funcref as a String"));
	    break;
	case VAR_LIST:
	    emsg(_("E730: using List as a String"));
	    break;
	case VAR_DICT:
	    emsg(_("E731: using Dictionary as a String"));
	    break;
	case VAR_FLOAT:
#ifdef FEAT_FLOAT
	    emsg(_(e_float_as_string));
	    break;
#endif
	case VAR_STRING:
	    if (varp->vval.v_string != NULL)
		return varp->vval.v_string;
	    return (char_u *)"";
	case VAR_SPECIAL:
	    STRCPY(buf, get_var_special_name(varp->vval.v_number));
	    return buf;
        case VAR_BLOB:
	    emsg(_("E976: using Blob as a String"));
	    break;
	case VAR_JOB:
#ifdef FEAT_JOB_CHANNEL
	    {
		job_T *job = varp->vval.v_job;
		char  *status;

		if (job == NULL)
		    return (char_u *)"no process";
		status = job->jv_status == JOB_FAILED ? "fail"
				: job->jv_status >= JOB_ENDED ? "dead"
				: "run";
# ifdef UNIX
		vim_snprintf((char *)buf, NUMBUFLEN,
			    "process %ld %s", (long)job->jv_pid, status);
# elif defined(MSWIN)
		vim_snprintf((char *)buf, NUMBUFLEN,
			    "process %ld %s",
			    (long)job->jv_proc_info.dwProcessId,
			    status);
# else
		// fall-back
		vim_snprintf((char *)buf, NUMBUFLEN, "process ? %s", status);
# endif
		return buf;
	    }
#endif
	    break;
	case VAR_CHANNEL:
#ifdef FEAT_JOB_CHANNEL
	    {
		channel_T *channel = varp->vval.v_channel;
		char      *status = channel_status(channel, -1);

		if (channel == NULL)
		    vim_snprintf((char *)buf, NUMBUFLEN, "channel %s", status);
		else
		    vim_snprintf((char *)buf, NUMBUFLEN,
				     "channel %d %s", channel->ch_id, status);
		return buf;
	    }
#endif
	    break;
	case VAR_UNKNOWN:
	    emsg(_("E908: using an invalid value as a String"));
	    break;
    }
    return NULL;
}

/*
 * Turn a typeval into a string.  Similar to tv_get_string_buf() but uses
 * string() on Dict, List, etc.
 */
    static char_u *
tv_stringify(typval_T *varp, char_u *buf)
{
    if (varp->v_type == VAR_LIST
	    || varp->v_type == VAR_DICT
	    || varp->v_type == VAR_FUNC
	    || varp->v_type == VAR_PARTIAL
	    || varp->v_type == VAR_FLOAT)
    {
	typval_T tmp;

	f_string(varp, &tmp);
	tv_get_string_buf(&tmp, buf);
	clear_tv(varp);
	*varp = tmp;
	return tmp.vval.v_string;
    }
    return tv_get_string_buf(varp, buf);
}

/*
 * Return TRUE if typeval "tv" and its value are set to be locked (immutable).
 * Also give an error message, using "name" or _("name") when use_gettext is
 * TRUE.
 */
    static int
tv_check_lock(typval_T *tv, char_u *name, int use_gettext)
{
    int	lock = 0;

    switch (tv->v_type)
    {
	case VAR_BLOB:
	    if (tv->vval.v_blob != NULL)
		lock = tv->vval.v_blob->bv_lock;
	    break;
	case VAR_LIST:
	    if (tv->vval.v_list != NULL)
		lock = tv->vval.v_list->lv_lock;
	    break;
	case VAR_DICT:
	    if (tv->vval.v_dict != NULL)
		lock = tv->vval.v_dict->dv_lock;
	    break;
	default:
	    break;
    }
    return var_check_lock(tv->v_lock, name, use_gettext)
		    || (lock != 0 && var_check_lock(lock, name, use_gettext));
}

/*
 * Copy the values from typval_T "from" to typval_T "to".
 * When needed allocates string or increases reference count.
 * Does not make a copy of a list, blob or dict but copies the reference!
 * It is OK for "from" and "to" to point to the same item.  This is used to
 * make a copy later.
 */
    void
copy_tv(typval_T *from, typval_T *to)
{
    to->v_type = from->v_type;
    to->v_lock = 0;
    switch (from->v_type)
    {
	case VAR_NUMBER:
	case VAR_SPECIAL:
	    to->vval.v_number = from->vval.v_number;
	    break;
	case VAR_FLOAT:
#ifdef FEAT_FLOAT
	    to->vval.v_float = from->vval.v_float;
	    break;
#endif
	case VAR_JOB:
#ifdef FEAT_JOB_CHANNEL
	    to->vval.v_job = from->vval.v_job;
	    if (to->vval.v_job != NULL)
		++to->vval.v_job->jv_refcount;
	    break;
#endif
	case VAR_CHANNEL:
#ifdef FEAT_JOB_CHANNEL
	    to->vval.v_channel = from->vval.v_channel;
	    if (to->vval.v_channel != NULL)
		++to->vval.v_channel->ch_refcount;
	    break;
#endif
	case VAR_STRING:
	case VAR_FUNC:
	    if (from->vval.v_string == NULL)
		to->vval.v_string = NULL;
	    else
	    {
		to->vval.v_string = vim_strsave(from->vval.v_string);
		if (from->v_type == VAR_FUNC)
		    func_ref(to->vval.v_string);
	    }
	    break;
	case VAR_PARTIAL:
	    if (from->vval.v_partial == NULL)
		to->vval.v_partial = NULL;
	    else
	    {
		to->vval.v_partial = from->vval.v_partial;
		++to->vval.v_partial->pt_refcount;
	    }
	    break;
	case VAR_BLOB:
	    if (from->vval.v_blob == NULL)
		to->vval.v_blob = NULL;
	    else
	    {
		to->vval.v_blob = from->vval.v_blob;
		++to->vval.v_blob->bv_refcount;
	    }
	    break;
	case VAR_LIST:
	    if (from->vval.v_list == NULL)
		to->vval.v_list = NULL;
	    else
	    {
		to->vval.v_list = from->vval.v_list;
		++to->vval.v_list->lv_refcount;
	    }
	    break;
	case VAR_DICT:
	    if (from->vval.v_dict == NULL)
		to->vval.v_dict = NULL;
	    else
	    {
		to->vval.v_dict = from->vval.v_dict;
		++to->vval.v_dict->dv_refcount;
	    }
	    break;
	case VAR_UNKNOWN:
	    internal_error("copy_tv(UNKNOWN)");
	    break;
    }
}

/*
 * Make a copy of an item.
 * Lists and Dictionaries are also copied.  A deep copy if "deep" is set.
 * For deepcopy() "copyID" is zero for a full copy or the ID for when a
 * reference to an already copied list/dict can be used.
 * Returns FAIL or OK.
 */
    int
item_copy(
    typval_T	*from,
    typval_T	*to,
    int		deep,
    int		copyID)
{
    static int	recurse = 0;
    int		ret = OK;

    if (recurse >= DICT_MAXNEST)
    {
	emsg(_("E698: variable nested too deep for making a copy"));
	return FAIL;
    }
    ++recurse;

    switch (from->v_type)
    {
	case VAR_NUMBER:
	case VAR_FLOAT:
	case VAR_STRING:
	case VAR_FUNC:
	case VAR_PARTIAL:
	case VAR_SPECIAL:
	case VAR_JOB:
	case VAR_CHANNEL:
	    copy_tv(from, to);
	    break;
	case VAR_LIST:
	    to->v_type = VAR_LIST;
	    to->v_lock = 0;
	    if (from->vval.v_list == NULL)
		to->vval.v_list = NULL;
	    else if (copyID != 0 && from->vval.v_list->lv_copyID == copyID)
	    {
		// use the copy made earlier
		to->vval.v_list = from->vval.v_list->lv_copylist;
		++to->vval.v_list->lv_refcount;
	    }
	    else
		to->vval.v_list = list_copy(from->vval.v_list, deep, copyID);
	    if (to->vval.v_list == NULL)
		ret = FAIL;
	    break;
	case VAR_BLOB:
	    ret = blob_copy(from, to);
	    break;
	case VAR_DICT:
	    to->v_type = VAR_DICT;
	    to->v_lock = 0;
	    if (from->vval.v_dict == NULL)
		to->vval.v_dict = NULL;
	    else if (copyID != 0 && from->vval.v_dict->dv_copyID == copyID)
	    {
		// use the copy made earlier
		to->vval.v_dict = from->vval.v_dict->dv_copydict;
		++to->vval.v_dict->dv_refcount;
	    }
	    else
		to->vval.v_dict = dict_copy(from->vval.v_dict, deep, copyID);
	    if (to->vval.v_dict == NULL)
		ret = FAIL;
	    break;
	case VAR_UNKNOWN:
	    internal_error("item_copy(UNKNOWN)");
	    ret = FAIL;
    }
    --recurse;
    return ret;
}

/*
 * ":echo expr1 ..."	print each argument separated with a space, add a
 *			newline at the end.
 * ":echon expr1 ..."	print each argument plain.
 */
    void
ex_echo(exarg_T *eap)
{
    char_u	*arg = eap->arg;
    typval_T	rettv;
    char_u	*tofree;
    char_u	*p;
    int		needclr = TRUE;
    int		atstart = TRUE;
    char_u	numbuf[NUMBUFLEN];
    int		did_emsg_before = did_emsg;
    int		called_emsg_before = called_emsg;

    if (eap->skip)
	++emsg_skip;
    while (*arg != NUL && *arg != '|' && *arg != '\n' && !got_int)
    {
	// If eval1() causes an error message the text from the command may
	// still need to be cleared. E.g., "echo 22,44".
	need_clr_eos = needclr;

	p = arg;
	if (eval1(&arg, &rettv, !eap->skip) == FAIL)
	{
	    /*
	     * Report the invalid expression unless the expression evaluation
	     * has been cancelled due to an aborting error, an interrupt, or an
	     * exception.
	     */
	    if (!aborting() && did_emsg == did_emsg_before
					  && called_emsg == called_emsg_before)
		semsg(_(e_invexpr2), p);
	    need_clr_eos = FALSE;
	    break;
	}
	need_clr_eos = FALSE;

	if (!eap->skip)
	{
	    if (atstart)
	    {
		atstart = FALSE;
		// Call msg_start() after eval1(), evaluating the expression
		// may cause a message to appear.
		if (eap->cmdidx == CMD_echo)
		{
		    // Mark the saved text as finishing the line, so that what
		    // follows is displayed on a new line when scrolling back
		    // at the more prompt.
		    msg_sb_eol();
		    msg_start();
		}
	    }
	    else if (eap->cmdidx == CMD_echo)
		msg_puts_attr(" ", echo_attr);
	    p = echo_string(&rettv, &tofree, numbuf, get_copyID());
	    if (p != NULL)
		for ( ; *p != NUL && !got_int; ++p)
		{
		    if (*p == '\n' || *p == '\r' || *p == TAB)
		    {
			if (*p != TAB && needclr)
			{
			    // remove any text still there from the command
			    msg_clr_eos();
			    needclr = FALSE;
			}
			msg_putchar_attr(*p, echo_attr);
		    }
		    else
		    {
			if (has_mbyte)
			{
			    int i = (*mb_ptr2len)(p);

			    (void)msg_outtrans_len_attr(p, i, echo_attr);
			    p += i - 1;
			}
			else
			    (void)msg_outtrans_len_attr(p, 1, echo_attr);
		    }
		}
	    vim_free(tofree);
	}
	clear_tv(&rettv);
	arg = skipwhite(arg);
    }
    eap->nextcmd = check_nextcmd(arg);

    if (eap->skip)
	--emsg_skip;
    else
    {
	// remove text that may still be there from the command
	if (needclr)
	    msg_clr_eos();
	if (eap->cmdidx == CMD_echo)
	    msg_end();
    }
}

/*
 * ":echohl {name}".
 */
    void
ex_echohl(exarg_T *eap)
{
    echo_attr = syn_name2attr(eap->arg);
}

/*
 * Returns the :echo attribute
 */
    int
get_echo_attr(void)
{
    return echo_attr;
}

/*
 * ":execute expr1 ..."	execute the result of an expression.
 * ":echomsg expr1 ..."	Print a message
 * ":echoerr expr1 ..."	Print an error
 * Each gets spaces around each argument and a newline at the end for
 * echo commands
 */
    void
ex_execute(exarg_T *eap)
{
    char_u	*arg = eap->arg;
    typval_T	rettv;
    int		ret = OK;
    char_u	*p;
    garray_T	ga;
    int		len;
    int		save_did_emsg;

    ga_init2(&ga, 1, 80);

    if (eap->skip)
	++emsg_skip;
    while (*arg != NUL && *arg != '|' && *arg != '\n')
    {
	ret = eval1_emsg(&arg, &rettv, !eap->skip);
	if (ret == FAIL)
	    break;

	if (!eap->skip)
	{
	    char_u   buf[NUMBUFLEN];

	    if (eap->cmdidx == CMD_execute)
		p = tv_get_string_buf(&rettv, buf);
	    else
		p = tv_stringify(&rettv, buf);
	    len = (int)STRLEN(p);
	    if (ga_grow(&ga, len + 2) == FAIL)
	    {
		clear_tv(&rettv);
		ret = FAIL;
		break;
	    }
	    if (ga.ga_len)
		((char_u *)(ga.ga_data))[ga.ga_len++] = ' ';
	    STRCPY((char_u *)(ga.ga_data) + ga.ga_len, p);
	    ga.ga_len += len;
	}

	clear_tv(&rettv);
	arg = skipwhite(arg);
    }

    if (ret != FAIL && ga.ga_data != NULL)
    {
	if (eap->cmdidx == CMD_echomsg || eap->cmdidx == CMD_echoerr)
	{
	    // Mark the already saved text as finishing the line, so that what
	    // follows is displayed on a new line when scrolling back at the
	    // more prompt.
	    msg_sb_eol();
	}

	if (eap->cmdidx == CMD_echomsg)
	{
	    msg_attr(ga.ga_data, echo_attr);
	    out_flush();
	}
	else if (eap->cmdidx == CMD_echoerr)
	{
	    // We don't want to abort following commands, restore did_emsg.
	    save_did_emsg = did_emsg;
	    emsg(ga.ga_data);
	    if (!force_abort)
		did_emsg = save_did_emsg;
	}
	else if (eap->cmdidx == CMD_execute)
	    do_cmdline((char_u *)ga.ga_data,
		       eap->getline, eap->cookie, DOCMD_NOWAIT|DOCMD_VERBOSE);
    }

    ga_clear(&ga);

    if (eap->skip)
	--emsg_skip;

    eap->nextcmd = check_nextcmd(arg);
}

/*
 * Skip over the name of an option: "&option", "&g:option" or "&l:option".
 * "arg" points to the "&" or '+' when called, to "option" when returning.
 * Returns NULL when no option name found.  Otherwise pointer to the char
 * after the option name.
 */
    char_u *
find_option_end(char_u **arg, int *opt_flags)
{
    char_u	*p = *arg;

    ++p;
    if (*p == 'g' && p[1] == ':')
    {
	*opt_flags = OPT_GLOBAL;
	p += 2;
    }
    else if (*p == 'l' && p[1] == ':')
    {
	*opt_flags = OPT_LOCAL;
	p += 2;
    }
    else
	*opt_flags = 0;

    if (!ASCII_ISALPHA(*p))
	return NULL;
    *arg = p;

    if (p[0] == 't' && p[1] == '_' && p[2] != NUL && p[3] != NUL)
	p += 4;	    // termcap option
    else
	while (ASCII_ISALPHA(*p))
	    ++p;
    return p;
}

/*
 * Display script name where an item was last set.
 * Should only be invoked when 'verbose' is non-zero.
 */
    void
last_set_msg(sctx_T script_ctx)
{
    char_u *p;

    if (script_ctx.sc_sid != 0)
    {
	p = home_replace_save(NULL, get_scriptname(script_ctx.sc_sid));
	if (p != NULL)
	{
	    verbose_enter();
	    msg_puts(_("\n\tLast set from "));
	    msg_puts((char *)p);
	    if (script_ctx.sc_lnum > 0)
	    {
		msg_puts(_(" line "));
		msg_outnum((long)script_ctx.sc_lnum);
	    }
	    verbose_leave();
	    vim_free(p);
	}
    }
}

/*
 * Compare "typ1" and "typ2".  Put the result in "typ1".
 */
    int
typval_compare(
    typval_T	*typ1,   // first operand
    typval_T	*typ2,   // second operand
    exptype_T	type,    // operator
    int		type_is, // TRUE for "is" and "isnot"
    int		ic)      // ignore case
{
    int		i;
    varnumber_T	n1, n2;
    char_u	*s1, *s2;
    char_u	buf1[NUMBUFLEN], buf2[NUMBUFLEN];

    if (type_is && typ1->v_type != typ2->v_type)
    {
	// For "is" a different type always means FALSE, for "notis"
	// it means TRUE.
	n1 = (type == TYPE_NEQUAL);
    }
    else if (typ1->v_type == VAR_BLOB || typ2->v_type == VAR_BLOB)
    {
	if (type_is)
	{
	    n1 = (typ1->v_type == typ2->v_type
			    && typ1->vval.v_blob == typ2->vval.v_blob);
	    if (type == TYPE_NEQUAL)
		n1 = !n1;
	}
	else if (typ1->v_type != typ2->v_type
		|| (type != TYPE_EQUAL && type != TYPE_NEQUAL))
	{
	    if (typ1->v_type != typ2->v_type)
		emsg(_("E977: Can only compare Blob with Blob"));
	    else
		emsg(_(e_invalblob));
	    clear_tv(typ1);
	    return FAIL;
	}
	else
	{
	    // Compare two Blobs for being equal or unequal.
	    n1 = blob_equal(typ1->vval.v_blob, typ2->vval.v_blob);
	    if (type == TYPE_NEQUAL)
		n1 = !n1;
	}
    }
    else if (typ1->v_type == VAR_LIST || typ2->v_type == VAR_LIST)
    {
	if (type_is)
	{
	    n1 = (typ1->v_type == typ2->v_type
			    && typ1->vval.v_list == typ2->vval.v_list);
	    if (type == TYPE_NEQUAL)
		n1 = !n1;
	}
	else if (typ1->v_type != typ2->v_type
		|| (type != TYPE_EQUAL && type != TYPE_NEQUAL))
	{
	    if (typ1->v_type != typ2->v_type)
		emsg(_("E691: Can only compare List with List"));
	    else
		emsg(_("E692: Invalid operation for List"));
	    clear_tv(typ1);
	    return FAIL;
	}
	else
	{
	    // Compare two Lists for being equal or unequal.
	    n1 = list_equal(typ1->vval.v_list, typ2->vval.v_list,
							    ic, FALSE);
	    if (type == TYPE_NEQUAL)
		n1 = !n1;
	}
    }

    else if (typ1->v_type == VAR_DICT || typ2->v_type == VAR_DICT)
    {
	if (type_is)
	{
	    n1 = (typ1->v_type == typ2->v_type
			    && typ1->vval.v_dict == typ2->vval.v_dict);
	    if (type == TYPE_NEQUAL)
		n1 = !n1;
	}
	else if (typ1->v_type != typ2->v_type
		|| (type != TYPE_EQUAL && type != TYPE_NEQUAL))
	{
	    if (typ1->v_type != typ2->v_type)
		emsg(_("E735: Can only compare Dictionary with Dictionary"));
	    else
		emsg(_("E736: Invalid operation for Dictionary"));
	    clear_tv(typ1);
	    return FAIL;
	}
	else
	{
	    // Compare two Dictionaries for being equal or unequal.
	    n1 = dict_equal(typ1->vval.v_dict, typ2->vval.v_dict,
							    ic, FALSE);
	    if (type == TYPE_NEQUAL)
		n1 = !n1;
	}
    }

    else if (typ1->v_type == VAR_FUNC || typ2->v_type == VAR_FUNC
	|| typ1->v_type == VAR_PARTIAL || typ2->v_type == VAR_PARTIAL)
    {
	if (type != TYPE_EQUAL && type != TYPE_NEQUAL)
	{
	    emsg(_("E694: Invalid operation for Funcrefs"));
	    clear_tv(typ1);
	    return FAIL;
	}
	if ((typ1->v_type == VAR_PARTIAL
					&& typ1->vval.v_partial == NULL)
		|| (typ2->v_type == VAR_PARTIAL
					&& typ2->vval.v_partial == NULL))
	    // when a partial is NULL assume not equal
	    n1 = FALSE;
	else if (type_is)
	{
	    if (typ1->v_type == VAR_FUNC && typ2->v_type == VAR_FUNC)
		// strings are considered the same if their value is
		// the same
		n1 = tv_equal(typ1, typ2, ic, FALSE);
	    else if (typ1->v_type == VAR_PARTIAL
					&& typ2->v_type == VAR_PARTIAL)
		n1 = (typ1->vval.v_partial == typ2->vval.v_partial);
	    else
		n1 = FALSE;
	}
	else
	    n1 = tv_equal(typ1, typ2, ic, FALSE);
	if (type == TYPE_NEQUAL)
	    n1 = !n1;
    }

#ifdef FEAT_FLOAT
    /*
	* If one of the two variables is a float, compare as a float.
	* When using "=~" or "!~", always compare as string.
	*/
    else if ((typ1->v_type == VAR_FLOAT || typ2->v_type == VAR_FLOAT)
	    && type != TYPE_MATCH && type != TYPE_NOMATCH)
    {
	float_T f1, f2;

	f1 = tv_get_float(typ1);
	f2 = tv_get_float(typ2);
	n1 = FALSE;
	switch (type)
	{
	    case TYPE_EQUAL:    n1 = (f1 == f2); break;
	    case TYPE_NEQUAL:   n1 = (f1 != f2); break;
	    case TYPE_GREATER:  n1 = (f1 > f2); break;
	    case TYPE_GEQUAL:   n1 = (f1 >= f2); break;
	    case TYPE_SMALLER:  n1 = (f1 < f2); break;
	    case TYPE_SEQUAL:   n1 = (f1 <= f2); break;
	    case TYPE_UNKNOWN:
	    case TYPE_MATCH:
	    case TYPE_NOMATCH:  break;  // avoid gcc warning
	}
    }
#endif

    /*
	* If one of the two variables is a number, compare as a number.
	* When using "=~" or "!~", always compare as string.
	*/
    else if ((typ1->v_type == VAR_NUMBER || typ2->v_type == VAR_NUMBER)
	    && type != TYPE_MATCH && type != TYPE_NOMATCH)
    {
	n1 = tv_get_number(typ1);
	n2 = tv_get_number(typ2);
	switch (type)
	{
	    case TYPE_EQUAL:    n1 = (n1 == n2); break;
	    case TYPE_NEQUAL:   n1 = (n1 != n2); break;
	    case TYPE_GREATER:  n1 = (n1 > n2); break;
	    case TYPE_GEQUAL:   n1 = (n1 >= n2); break;
	    case TYPE_SMALLER:  n1 = (n1 < n2); break;
	    case TYPE_SEQUAL:   n1 = (n1 <= n2); break;
	    case TYPE_UNKNOWN:
	    case TYPE_MATCH:
	    case TYPE_NOMATCH:  break;  // avoid gcc warning
	}
    }
    else
    {
	s1 = tv_get_string_buf(typ1, buf1);
	s2 = tv_get_string_buf(typ2, buf2);
	if (type != TYPE_MATCH && type != TYPE_NOMATCH)
	    i = ic ? MB_STRICMP(s1, s2) : STRCMP(s1, s2);
	else
	    i = 0;
	n1 = FALSE;
	switch (type)
	{
	    case TYPE_EQUAL:    n1 = (i == 0); break;
	    case TYPE_NEQUAL:   n1 = (i != 0); break;
	    case TYPE_GREATER:  n1 = (i > 0); break;
	    case TYPE_GEQUAL:   n1 = (i >= 0); break;
	    case TYPE_SMALLER:  n1 = (i < 0); break;
	    case TYPE_SEQUAL:   n1 = (i <= 0); break;

	    case TYPE_MATCH:
	    case TYPE_NOMATCH:
		    n1 = pattern_match(s2, s1, ic);
		    if (type == TYPE_NOMATCH)
			n1 = !n1;
		    break;

	    case TYPE_UNKNOWN:  break;  // avoid gcc warning
	}
    }
    clear_tv(typ1);
    typ1->v_type = VAR_NUMBER;
    typ1->vval.v_number = n1;

    return OK;
}

    char_u *
typval_tostring(typval_T *arg)
{
    char_u	*tofree;
    char_u	numbuf[NUMBUFLEN];
    char_u	*ret = NULL;

    if (arg == NULL)
	return vim_strsave((char_u *)"(does not exist)");
    ret = tv2string(arg, &tofree, numbuf, 0);
    // Make a copy if we have a value but it's not in allocated memory.
    if (ret != NULL && tofree == NULL)
	ret = vim_strsave(ret);
    return ret;
}

#endif // FEAT_EVAL

/*
 * Perform a substitution on "str" with pattern "pat" and substitute "sub".
 * When "sub" is NULL "expr" is used, must be a VAR_FUNC or VAR_PARTIAL.
 * "flags" can be "g" to do a global substitute.
 * Returns an allocated string, NULL for error.
 */
    char_u *
do_string_sub(
    char_u	*str,
    char_u	*pat,
    char_u	*sub,
    typval_T	*expr,
    char_u	*flags)
{
    int		sublen;
    regmatch_T	regmatch;
    int		i;
    int		do_all;
    char_u	*tail;
    char_u	*end;
    garray_T	ga;
    char_u	*ret;
    char_u	*save_cpo;
    char_u	*zero_width = NULL;

    // Make 'cpoptions' empty, so that the 'l' flag doesn't work here
    save_cpo = p_cpo;
    p_cpo = empty_option;

    ga_init2(&ga, 1, 200);

    do_all = (flags[0] == 'g');

    regmatch.rm_ic = p_ic;
    regmatch.regprog = vim_regcomp(pat, RE_MAGIC + RE_STRING);
    if (regmatch.regprog != NULL)
    {
	tail = str;
	end = str + STRLEN(str);
	while (vim_regexec_nl(&regmatch, str, (colnr_T)(tail - str)))
	{
	    // Skip empty match except for first match.
	    if (regmatch.startp[0] == regmatch.endp[0])
	    {
		if (zero_width == regmatch.startp[0])
		{
		    // avoid getting stuck on a match with an empty string
		    i = mb_ptr2len(tail);
		    mch_memmove((char_u *)ga.ga_data + ga.ga_len, tail,
								   (size_t)i);
		    ga.ga_len += i;
		    tail += i;
		    continue;
		}
		zero_width = regmatch.startp[0];
	    }

	    /*
	     * Get some space for a temporary buffer to do the substitution
	     * into.  It will contain:
	     * - The text up to where the match is.
	     * - The substituted text.
	     * - The text after the match.
	     */
	    sublen = vim_regsub(&regmatch, sub, expr, tail, FALSE, TRUE, FALSE);
	    if (ga_grow(&ga, (int)((end - tail) + sublen -
			    (regmatch.endp[0] - regmatch.startp[0]))) == FAIL)
	    {
		ga_clear(&ga);
		break;
	    }

	    // copy the text up to where the match is
	    i = (int)(regmatch.startp[0] - tail);
	    mch_memmove((char_u *)ga.ga_data + ga.ga_len, tail, (size_t)i);
	    // add the substituted text
	    (void)vim_regsub(&regmatch, sub, expr, (char_u *)ga.ga_data
					  + ga.ga_len + i, TRUE, TRUE, FALSE);
	    ga.ga_len += i + sublen - 1;
	    tail = regmatch.endp[0];
	    if (*tail == NUL)
		break;
	    if (!do_all)
		break;
	}

	if (ga.ga_data != NULL)
	    STRCPY((char *)ga.ga_data + ga.ga_len, tail);

	vim_regfree(regmatch.regprog);
    }

    ret = vim_strsave(ga.ga_data == NULL ? str : (char_u *)ga.ga_data);
    ga_clear(&ga);
    if (p_cpo == empty_option)
	p_cpo = save_cpo;
    else
	// Darn, evaluating {sub} expression or {expr} changed the value.
	free_string_option(save_cpo);

    return ret;
}
