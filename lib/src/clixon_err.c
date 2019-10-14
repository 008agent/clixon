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

 *
 * Errors may be syslogged using LOG_ERR, and printed to stderr, as controlled
 * by clicon_log_init
 * global error variables are set:
 *  clicon_errno, clicon_suberrno, clicon_err_reason.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>

#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_err.h"

/*
 * Types
 */
struct errvec{
    char *ev_str;
    int   ev_err;
};

struct err_state{
    int  es_errno;
    int  es_suberrno;
    char es_reason[ERR_STRLEN];
};

/*
 * Variables
 */
int clicon_errno  = 0;    /* See enum clicon_err */
int clicon_suberrno  = 0; /* Corresponds to errno.h */
char clicon_err_reason[ERR_STRLEN] = {0, };

/*
 * Error descriptions. Must stop with NULL element.
 */
static struct errvec EV[] = {
    {"Database error",         OE_DB},
    {"Daemon error",           OE_DAEMON},
    {"Event error",            OE_EVENTS},
    {"Config error",           OE_CFG},
    {"Protocol error",         OE_PROTO},
    {"Regexp error",           OE_REGEX},
    {"UNIX error",             OE_UNIX},
    {"Syslog error",           OE_SYSLOG},
    {"Routing demon error",    OE_ROUTING},
    {"XML error",              OE_XML},
    {"Plugins",                OE_PLUGIN},
    {"Yang error",             OE_YANG},
    {"FATAL",                  OE_FATAL},
    {"Undefined",              OE_UNDEF},
    {NULL,                     -1}
};

static char *
clicon_strerror1(int           err,
		 struct errvec vec[])
{
    struct errvec *ev;

    for (ev=vec; ev->ev_err != -1; ev++)
	if (ev->ev_err == err)
	    break;
    return ev?(ev->ev_str?ev->ev_str:"unknown"):"CLICON unknown error";
}

/*! Clear error state and continue.
 *
 * Clear error state and get on with it, typically non-fatal error and you wish to continue.
 */
int
clicon_err_reset(void)
{
    clicon_errno = 0;
    clicon_suberrno = 0;
    memset(clicon_err_reason, 0, ERR_STRLEN);
    return 0;
}

/*! Report an error.
 *
 * Library routines should call this function when an error occurs.
 * The function does he following:
 * - Logs to syslog with LOG_ERR
 * - Set global error variable name clicon_errno
 * - Set global reason string clicon_err_reason
 * @note: err direction (syslog and/or stderr) controlled by clicon_log_init()
 *
 * @param[in]    fn       Inline function name (when called from clicon_err() macro)
 * @param[in]    line     Inline file line number (when called from clicon_err() macro)
 * @param[in]    err      Error number, typically errno
 * @param[in]    suberr   Sub-error number   
 * @param[in]    reason   Error string, format with argv
 * @see clicon_err_reser  Resetting the global error variables.
 */
int
clicon_err_fn(const char *fn, 
	      const int   line, 
	      int         category, 
	      int         suberr, 
	      char       *reason, ...)
{
    va_list args;
    int     len;
    char   *msg    = NULL;
    int     retval = -1;

    /* Set the global variables */
    clicon_errno    = category;
    clicon_suberrno = suberr;

    /* first round: compute length of error message */
    va_start(args, reason);
    len = vsnprintf(NULL, 0, reason, args);
    va_end(args);

    /* allocate a message string exactly fitting the message length */
    if ((msg = malloc(len+1)) == NULL){
	fprintf(stderr, "malloc: %s\n", strerror(errno)); /* dont use clicon_err here due to recursion */
	goto done;
    }

    /* second round: compute write message from reason and args */
    va_start(args, reason);
    if (vsnprintf(msg, len+1, reason, args) < 0){
	va_end(args);
	fprintf(stderr, "vsnprintf: %s\n", strerror(errno)); /* dont use clicon_err here due to recursion */
	goto done;
    }
    va_end(args);
    strncpy(clicon_err_reason, msg, ERR_STRLEN-1);

    /* Actually log it */
    if (suberr){
	/* Here we could take care of specific suberr, like application-defined errors */
	clicon_log(LOG_ERR, "%s: %d: %s: %s: %s", 
		   fn,
		   line,
		   clicon_strerror(category),
		   msg,
		   suberr==XMLPARSE_ERRNO?"XML parse error":strerror(suberr));
    }
    else
	clicon_log(LOG_ERR, "%s: %d: %s: %s", 
		   fn,
		   line,
		   clicon_strerror(category),
		   msg);

    retval = 0;
  done:
    if (msg)
	free(msg);
    return retval;
}

/*! Translate from numeric error to string representation
 */
char *
clicon_strerror(int err)
{
    return clicon_strerror1(err, EV);
}

/*! Push an error state, if recursive error handling
 */
void*
clicon_err_save(void)
{
    struct err_state *es;

    if ((es = malloc(sizeof(*es))) == NULL)
	return NULL;
    es->es_errno = clicon_errno;
    es->es_suberrno = clicon_suberrno;
    strncpy(es->es_reason, clicon_err_reason, ERR_STRLEN-1);
    return (void*)es;
}

/*! Pop an error state, if recursive error handling
 */
int
clicon_err_restore(void* handle)
{
    struct err_state *es;

    if ((es = (struct err_state *)handle) != NULL){
	clicon_errno = es->es_errno;
	clicon_suberrno = es->es_suberrno;
	strncpy(clicon_err_reason, es->es_reason, ERR_STRLEN-1);
	free(es);
    }
    return 0;
}
