/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * Yang module and feature handling
 * @see https://tools.ietf.org/html/rfc7895
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <regex.h>
#include <dirent.h>
#include <sys/types.h>
#include <fcntl.h>
#include <syslog.h>
#include <assert.h>
#include <sys/stat.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_log.h"
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_file.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_plugin.h"
#include "clixon_netconf_lib.h"
#include "clixon_yang_module.h"
#include "clixon_yang_internal.h" /* internal */

modstate_diff_t *
modstate_diff_new(void)
{
    modstate_diff_t *md;
    if ((md = malloc(sizeof(modstate_diff_t))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	return NULL;
    }
    memset(md, 0, sizeof(modstate_diff_t));
    return md;
}

int
modstate_diff_free(modstate_diff_t *md)
{
    if (md == NULL)
	return 0;
    if (md->md_del)
	xml_free(md->md_del);
    if (md->md_mod)
	xml_free(md->md_mod);
    free(md);
    return 0;
}

/*! Init the Yang module library
 *
 * Load RFC7895 yang spec, module-set-id, etc.
 * @param[in]     h       Clicon handle
 */
int
yang_modules_init(clicon_handle h)
{
    int        retval = -1;
    yang_stmt *yspec;

    yspec = clicon_dbspec_yang(h);	
    if (!clicon_option_bool(h, "CLICON_MODULE_LIBRARY_RFC7895"))
	goto ok;
    /* Ensure module-set-id is set */
    if (!clicon_option_exists(h, "CLICON_MODULE_SET_ID")){
	clicon_err(OE_CFG, ENOENT, "CLICON_MODULE_SET_ID must be defined when CLICON_MODULE_LIBRARY_RFC7895 is enabled");
	goto done;
    }
    /* Ensure revision exists is set */
    if (yang_spec_parse_module(h, "ietf-yang-library", NULL, yspec)< 0)
	goto done;
    /* Find revision */
    if (yang_modules_revision(h) == NULL){
	clicon_err(OE_CFG, ENOENT, "Yang client library yang spec has no revision");
	goto done;
    }
 ok:
     retval = 0;
 done:
     return retval;
}

/*! Return RFC7895 revision (if parsed)
 * @param[in]    h        Clicon handle
 * @retval       revision String (dont free)
 * @retval       NULL     Error: RFC7895 not loaded or revision not found
 */
char *
yang_modules_revision(clicon_handle h)
{
    yang_stmt *yspec;
    yang_stmt *ymod;
    yang_stmt *yrev;
    char      *revision = NULL;

    yspec = clicon_dbspec_yang(h);
    if ((ymod = yang_find(yspec, Y_MODULE, "ietf-yang-library")) != NULL ||
	(ymod = yang_find(yspec, Y_SUBMODULE, "ietf-yang-library")) != NULL){
	if ((yrev = yang_find(ymod, Y_REVISION, NULL)) != NULL){
	    revision = yrev->ys_argument;
	}
    }
    return revision;
}

/*! Actually build the yang modules state XML tree
 * @see RFC7895
 */
static int
yms_build(clicon_handle    h,
	  yang_stmt       *yspec,
	  char            *msid,
	  int              brief,
	  cbuf            *cb)
{
    int         retval = -1;
    yang_stmt  *ylib = NULL; /* ietf-yang-library */
    char       *module = "ietf-yang-library";
    yang_stmt  *ys;
    yang_stmt  *yc;
    yang_stmt  *ymod;        /* generic module */
    yang_stmt  *yns = NULL;  /* namespace */

    if ((ylib = yang_find(yspec, Y_MODULE, module)) == NULL &&
	(ylib = yang_find(yspec, Y_SUBMODULE, module)) == NULL){
            clicon_err(OE_YANG, 0, "%s not found", module);
            goto done;
        }
    if ((yns = yang_find(ylib, Y_NAMESPACE, NULL)) == NULL){
	clicon_err(OE_YANG, 0, "%s yang namespace not found", module);
	goto done;
    }

    cprintf(cb,"<modules-state xmlns=\"%s\">", yns->ys_argument);
    cprintf(cb,"<module-set-id>%s</module-set-id>", msid);

    ymod = NULL;
    while ((ymod = yn_each(yspec, ymod)) != NULL) {
	if (ymod->ys_keyword != Y_MODULE &&
	    ymod->ys_keyword != Y_SUBMODULE)
	    continue;
	cprintf(cb,"<module>");
	cprintf(cb,"<name>%s</name>", ymod->ys_argument);
	if ((ys = yang_find(ymod, Y_REVISION, NULL)) != NULL)
	    cprintf(cb,"<revision>%s</revision>", ys->ys_argument);
	else{
	    /* RFC7895 1 If no (such) revision statement exists, the module's or 
	       submodule's revision is the zero-length string. */
	    cprintf(cb,"<revision></revision>");
	}
	if ((ys = yang_find(ymod, Y_NAMESPACE, NULL)) != NULL)
	    cprintf(cb,"<namespace>%s</namespace>", ys->ys_argument);
	else
	    cprintf(cb,"<namespace></namespace>");
	/* This follows order in rfc 7895: feature, conformance-type, 
	   submodules */
	if (!brief){
	    yc = NULL;
	    while ((yc = yn_each(ymod, yc)) != NULL) {
		switch(yc->ys_keyword){
		case Y_FEATURE:
		    if (yc->ys_cv && cv_bool_get(yc->ys_cv))
			cprintf(cb,"<feature>%s</feature>", yc->ys_argument);
		    break;
		default:
		    break;
		}
	    }
	    cprintf(cb, "<conformance-type>implement</conformance-type>");
	}
	yc = NULL;
	while ((yc = yn_each(ymod, yc)) != NULL) {
	    switch(yc->ys_keyword){
	    case Y_SUBMODULE:
		cprintf(cb,"<submodule>");
		cprintf(cb,"<name>%s</name>", yc->ys_argument);
		if ((ys = yang_find(yc, Y_REVISION, NULL)) != NULL)
		    cprintf(cb,"<revision>%s</revision>", ys->ys_argument);
		else
		    cprintf(cb,"<revision></revision>");
		cprintf(cb,"</submodule>");
		break;
	    default:
		break;
	    }
	}
	cprintf(cb,"</module>");
    }
    cprintf(cb,"</modules-state>");
    retval = 0;
 done:
    return retval;
}

/*! Get modules state according to RFC 7895
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   XML Xpath
 * @param[in]     brief   Just name, revision and uri (no cache)
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @retval       -1       Error (fatal)
 * @retval        0       OK
 * @retval        1       Statedata callback failed
 * @notes NYI: schema, deviation
x      +--ro modules-state
x         +--ro module-set-id    string
x         +--ro module* [name revision]
x            +--ro name                yang:yang-identifier
x            +--ro revision            union
            +--ro schema?             inet:uri
x            +--ro namespace           inet:uri
            +--ro feature*            yang:yang-identifier
            +--ro deviation* [name revision]
            |  +--ro name        yang:yang-identifier
            |  +--ro revision    union
            +--ro conformance-type    enumeration
            +--ro submodule* [name revision]
               +--ro name        yang:yang-identifier
               +--ro revision    union
               +--ro schema?     inet:uri
 * @see netconf_create_hello
 */
int
yang_modules_state_get(clicon_handle    h,
                       yang_stmt       *yspec,
                       char            *xpath,
		       int              brief,
                       cxobj          **xret)
{
    int         retval = -1;
    cxobj      *x = NULL; /* Top tree, some juggling w top symbol */
    char       *msid; /* modules-set-id */
    cxobj      *x1;
    cbuf       *cb = NULL;

    msid = clicon_option_str(h, "CLICON_MODULE_SET_ID");
    if ((x = clicon_modst_cache_get(h, brief)) != NULL){
	/* x is here: <modules-state>... 
	 * and x is original tree, need to copy */
        if (xpath_first(x, "%s", xpath)){
            if ((x1 = xml_dup(x)) == NULL)
                goto done;
            x = x1;
        }
        else
            x = NULL;
    }
    else { /* No cache -> build the tree */
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, 0, "clicon buffer");
	    goto done;
	}
	/* Build a cb string: <modules-state>... */
	if (yms_build(h, yspec, msid, brief, cb) < 0)
	    goto done;
	/* Parse cb, x is on the form: <top><modules-state>... */
	if (xml_parse_string(cbuf_get(cb), yspec, &x) < 0){
	    if (netconf_operation_failed_xml(xret, "protocol", clicon_err_reason)< 0)
		goto done;
	    retval = 1;
	    goto done;
	}
	if (xml_rootchild(x, 0, &x) < 0)
	    goto done;
	/* x is now: <modules-state>... */
	if (clicon_modst_cache_set(h, brief, x) < 0) /* move to fn above? */
	    goto done;
    }
    if (x){
	/* Wrap x (again) with new top-level node "top" which merge wants */
	if ((x = xml_wrap(x, "top")) < 0)
	    goto done;
	if (netconf_trymerge(x, yspec, xret) < 0)
	    goto done;
    }
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (cb)
        cbuf_free(cb);
    if (x)
        xml_free(x);
    return retval;
}

/*! For single module state with namespace, get revisions and send upgrade callbacks
 * @param[in]  h        Clicon handle
 * @param[in]  xt      Top-level XML tree to be updated (includes other ns as well)
 * @param[in]  xs       XML module state (for one yang module)
 * @param[in]  xvec     Help vector where to store XML child nodes (??)
 * @param[in]  xlen     Length of xvec
 * @param[in]  ns0      Namespace of module state we are looking for
 * @param[in]  deleted  If set, dont look for system yang module and "to" rev
 * @param[out] cbret Netconf error message if invalid
 * @retval     1        OK
 * @retval     0        Validation failed (cbret set)
 * @retval    -1        Error
 */
static int
mod_ns_upgrade(clicon_handle h,
	       cxobj        *xt,
	       cxobj        *xs,
	       char         *ns,
	       int           deleted,
	       cbuf         *cbret)
{
    int        retval = -1;
    char      *b; /* string body */
    yang_stmt *ymod;
    yang_stmt *yrev;
    uint32_t   from = 0;
    uint32_t   to = 0;
    int        ret;
    yang_stmt *yspec;

    /* Make upgrade callback for this XML, specifying the module 
     * namespace, from and to revision.
     */
    if ((b = xml_find_body(xs, "revision")) != NULL) /* Module revision */
	if (ys_parse_date_arg(b, &from) < 0)
	    goto done;
    if (deleted)
	to = 0;
    else {	    /* Look up system module (alt send it via argument) */
	yspec = clicon_dbspec_yang(h);
	if ((ymod = yang_find_module_by_namespace(yspec, ns)) == NULL)
	    goto fail;
	if ((yrev = yang_find(ymod, Y_REVISION, NULL)) == NULL)
	    goto fail;
	if (ys_parse_date_arg(yrev->ys_argument, &to) < 0)
	    goto done;
    }
    if ((ret = upgrade_callback_call(h, xt, ns, from, to, cbret)) < 0)
	goto done;
    if (ret == 0) /* XXX ignore and continue? */
	goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Upgrade XML
 * @param[in]  h    Clicon handle
 * @param[in]  xt   XML tree (to upgrade)
 * @param[in]  msd  Modules-state differences of xt
 * @param[out] cbret Netconf error message if invalid
 * @retval     1    OK
 * @retval     0    Validation failed (cbret set)
 * @retval    -1    Error
 */
int
clixon_module_upgrade(clicon_handle    h,
		      cxobj           *xt,
		      modstate_diff_t *msd,
   		      cbuf            *cbret)
{
    int        retval = -1;
    char      *ns;           /* Namespace */
    cxobj     *xs;            /* XML module state */
    int        ret;

    if (msd == NULL)
	goto ok;
    /* Iterate through xml modified module state */
    xs = NULL;
    while ((xs = xml_child_each(msd->md_mod, xs, CX_ELMNT)) != NULL) {
	/* Extract namespace */
	if ((ns = xml_find_body(xs, "namespace")) == NULL)
	    goto done;
	/* Extract revisions and make callbacks */
	if ((ret = mod_ns_upgrade(h, xt, xs, ns, 0, cbret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    /* Iterate through xml deleted module state */
    xs = NULL;
    while ((xs = xml_child_each(msd->md_del, xs, CX_ELMNT)) != NULL) {
	/* Extract namespace */
	if ((ns = xml_find_body(xs, "namespace")) == NULL)
	    continue;
	/* Extract revisions and make callbacks (now w deleted=1)  */
	if ((ret = mod_ns_upgrade(h, xt, xs, ns, 1, cbret)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}
