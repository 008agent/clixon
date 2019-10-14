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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>

/* clicon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_sig.h"

/*! Set a signal handler.
 * @param[in]  signo      Signal number 
 * @param[in]  handler    Function to call when signal occurs
 * @param[out] oldhandler Pointer to old handler
 */
int
set_signal(int     signo, 
	   void  (*handler)(int), 
	   void (**oldhandler)(int))
{
#if defined(HAVE_SIGACTION)
    struct sigaction sold, snew;

    snew.sa_handler = handler;
    sigemptyset(&snew.sa_mask);
     snew.sa_flags = 0;
     if (sigaction (signo, &snew, &sold) < 0){
	clicon_err(OE_UNIX, errno, "sigaction");
	return -1;
     }
     if (oldhandler)
	 *oldhandler = sold.sa_handler;
    return 0;
#elif defined(HAVE_SIGVEC)
    assert(0);
    return 0;
#endif
}


/*! Block signal. 
 * @param[in] sig   Signal number to block, If 0, block all signals
 */
void
clicon_signal_block (int sig)
{
	sigset_t
		set;

	sigemptyset (&set);
	if (sig)
		sigaddset (&set, sig);
	else 
		sigfillset (&set);

	sigprocmask (SIG_BLOCK, &set, NULL);
}

/*! Unblock signal. 
 * @param[in] sig   Signal number to unblock. If 0, unblock all signals
 */
void
clicon_signal_unblock (int sig)
{
	sigset_t
		set;

	sigemptyset (&set);
	if (sig)
		sigaddset (&set, sig);
	else 
		sigfillset (&set);

	sigprocmask (SIG_UNBLOCK, &set, NULL);
}

/*! Read pidfile and return pid, if any
 *
 * @param[in]  pidfile  Name of pidfile
 * @param[out] pid0     Process id of (eventual) existing daemon process
 * @retval    0         OK. if pid > 0 old process exists w that pid
 * @retval    -1        Error, and clicon_err() called
 */
int
pidfile_get(char  *pidfile, 
	    pid_t *pid0)
{
    FILE   *f;
    char   *ptr;
    char    buf[32];
    pid_t   pid;

    if ((f = fopen (pidfile, "r")) != NULL){
	ptr = fgets(buf, sizeof(buf), f);
	fclose (f);
	if (ptr != NULL && (pid = atoi (ptr)) > 1) {
	    if (kill (pid, 0) == 0 || errno != ESRCH) {
		/* Yes there is a process */
		*pid0 = pid;
		return 0;
	    }
	}
    }
    *pid0 = 0;
    return 0;
}

/*! Given a pid, kill that process
 *
 * @param[in] pid   Process id
 * @retval    0     Killed OK
 * @retval    -1    Could not kill. 
 * Maybe shouldk not belong to pidfile code,..
 */
int
pidfile_zapold(pid_t pid)
{
    clicon_log(LOG_NOTICE, "Killing old daemon with pid: %d", pid);
    killpg(pid, SIGTERM);
    kill(pid, SIGTERM);
    sleep(1); /* check again */
    if ((kill (pid, 0)) != 0 && errno == ESRCH) /* Nothing there */
	;
    else{ /* problem: couldnt kill it */
	clicon_err(OE_DAEMON, errno, "Killing old demon");
	return -1;
    }
    return 0;
}

/*! Write a pid-file
 *
 * @param[in] pidfile   Name of pidfile
 */
int
pidfile_write(char *pidfile)
{
    FILE *f = NULL;
    int   retval = -1;

    /* Here, there should be no old agent and no pidfile */
    if ((f = fopen(pidfile, "w")) == NULL){
	if (errno == EACCES)
	    clicon_err(OE_DAEMON, errno, "Creating pid-file %s (Try run as root?)", pidfile);
	else
	    clicon_err(OE_DAEMON, errno, "Creating pid-file %s", pidfile);
	goto done;
    } 
    if ((retval = fprintf(f, "%ld\n", (long) getpid())) < 1){
	clicon_err(OE_DAEMON, errno, "Could not write pid to %s", pidfile);
	goto done;
    }
    clicon_debug(1, "Opened pidfile %s with pid %d", pidfile, getpid());
    retval = 0;
 done:
    if (f != NULL)
	fclose(f);
    return retval;
}
