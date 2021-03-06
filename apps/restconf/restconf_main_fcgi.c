/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

/*
 * This program should be run as user www-data 
 *
 * See draft-ietf-netconf-restconf-13.txt [draft]

 * sudo apt-get install libfcgi-dev
 * gcc -o fastcgi fastcgi.c -lfcgi

 * sudo su -c "/www-data/clixon_restconf -D 1 -f /usr/local/etc/example.xml " -s /bin/sh www-data

 * This is the interface:
 * api/data/profile=<name>/metric=<name> PUT data:enable=<flag>
 * api/test
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <libgen.h>
#include <sys/stat.h> /* chmod */

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include <fcgiapp.h> /* Need to be after clixon_xml.h due to attribute format */

/* restconf */
#include "restconf_lib.h"      /* generic shared with plugins */
#include "restconf_handle.h"
#include "restconf_api.h"      /* generic not shared with plugins */
#include "restconf_err.h"
#include "restconf_root.h"     /* generic not shared with plugins */
#include "restconf_methods.h"  /* fcgi specific */
#include "restconf_methods_get.h"
#include "restconf_methods_post.h"
#include "restconf_stream.h"

/* Command line options to be passed to getopt(3) */
#define RESTCONF_OPTS "hD:f:l:p:d:y:a:u:o:"

/*! Convert FCGI parameters to clixon runtime data
 * @param[in]  h     Clixon handle
 * @param[in]  envp  Fastcgi request handle parameter array on the format "<param>=<value>"
 * @see https://nginx.org/en/docs/http/ngx_http_core_module.html#var_https
 */
static int
fcgi_params_set(clicon_handle h,
			   char        **envp)
{
    int   retval = -1;
    int   i;
    char *param = NULL;
    char *val = NULL;

    clicon_debug(1, "%s", __FUNCTION__);
    for (i = 0; envp[i] != NULL; i++){ /* on the form <param>=<value> */
	if (clixon_strsplit(envp[i], '=', &param, &val) < 0)
	    goto done;
	if (restconf_param_set(h, param, val) < 0)
	    goto done;
	if (param){
	    free(param);
	    param = NULL;
	}
	if (val){
	    free(val);
	    val = NULL;
	}
    }
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}

/* Need global variable to for signal handler XXX */
static clicon_handle _CLICON_HANDLE = NULL;

/*! Signall terminates process
 */
static void
restconf_sig_term(int arg)
{
    static int i=0;

    if (i++ == 0)
	clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
		   __PROGRAM__, __FUNCTION__, getpid(), arg);
    else
	exit(-1);
    if (_CLICON_HANDLE){
	stream_child_freeall(_CLICON_HANDLE);
	restconf_terminate(_CLICON_HANDLE);
    }
    clicon_exit_set(); /* checked in clixon_event_loop() */
    exit(-1);
}

static void
restconf_sig_child(int arg)
{
    int status;
    int pid;

    if ((pid = waitpid(-1, &status, 0)) != -1 && WIFEXITED(status))
	stream_child_free(_CLICON_HANDLE, pid);
}

/*! Usage help routine
 * @param[in]  argv0  command line
 * @param[in]  h      Clicon handle
 */
static void
usage(clicon_handle h,
      char         *argv0)

{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\t  Help\n"
	    "\t-D <level>\t  Debug level\n"
    	    "\t-f <file>\t  Configuration file (mandatory)\n"
	    "\t-l <s|f<file>> \t  Log on (s)yslog, (f)ile (syslog is default)\n"
	    "\t-p <dir>\t  Yang directory path (see CLICON_YANG_DIR)\n"
	    "\t-d <dir>\t  Specify restconf plugin directory dir (default: %s)\n"
	    "\t-y <file>\t  Load yang spec file (override yang main module)\n"
    	    "\t-a UNIX|IPv4|IPv6 Internal backend socket family\n"
    	    "\t-u <path|addr>\t  Internal socket domain path or IP addr (see -a)\n"
	    "\t-o \"<option>=<value>\" Give configuration option overriding config file (see clixon-config.yang)\n",
	    argv0,
	    clicon_restconf_dir(h)
	    );
    exit(0);
}

/*! Main routine for fastcgi restconf
 */
int 
main(int    argc, 
     char **argv) 
{
    int            retval = -1;
    int            sock;
    char	  *argv0 = argv[0];
    FCGX_Request   request;
    FCGX_Request  *req = &request;
    int            c;
    char          *sockpath;
    char          *path;
    clicon_handle  h;
    char          *dir;
    int            logdst = CLICON_LOG_SYSLOG;
    yang_stmt     *yspec = NULL;
    char          *stream_path;
    int            finish = 0;
    int            start = 1;
    char          *str;
    clixon_plugin *cp = NULL;
    uint32_t       id = 0;
    cvec          *nsctx_global = NULL; /* Global namespace context */
    size_t         cligen_buflen;
    size_t         cligen_bufthreshold;
    int            dbg = 0;
    
    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Create handle */
    if ((h = restconf_handle_init()) == NULL)
	goto done;

    _CLICON_HANDLE = h; /* for termination handling */

    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h':
	    usage(h, argv[0]);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(h, argv[0]);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	     if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	   break;
	} /* switch getopt */
    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst); 

    clicon_debug_init(dbg, NULL); 
    clicon_log(LOG_NOTICE, "%s: %u Started", __PROGRAM__, getpid());
    if (set_signal(SIGTERM, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGCHLD, restconf_sig_child, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }

    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
	goto done;

    stream_path = clicon_option_str(h, "CLICON_STREAM_PATH");
    /* Now rest of options, some overwrite option file */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'l':  /* log  */
	    break; /* see above */
	case 'p' : /* yang dir path */
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_RESTCONF_DIR", optarg);
	    break;
	case 'y' : /* Load yang spec file (override yang main module) */
	    clicon_option_str_set(h, "CLICON_YANG_MAIN_FILE", optarg);
	    break;
	case 'a': /* internal backend socket address family */
	    clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
	    break;
	case 'u': /* internal backend socket unix domain path or ip host */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_SOCK", optarg);
	    break;
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
        default:
            usage(h, argv[0]);
            break;
	}
    argc -= optind;
    argv += optind;

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);
    
    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    
    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;

    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);
    /* Treat unknown XML as anydata */
    if (clicon_option_bool(h, "CLICON_YANG_UNKNOWN_ANYDATA") == 1)
	xml_bind_yang_unknown_anydata(1);
    
    /* Load restconf plugins before yangs are loaded (eg extension callbacks) */
    if ((dir = clicon_restconf_dir(h)) != NULL)
	if (clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
	    return -1;
    /* Create a pseudo-plugin to create extension callback to set the ietf-routing
     * yang-data extension for api-root top-level restconf function.
     */
    if (clixon_pseudo_plugin(h, "pseudo restconf", &cp) < 0)
	goto done;
    cp->cp_api.ca_extension = restconf_main_extension_cb;

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;

    /* Load yang restconf module */
    if (yang_spec_parse_module(h, "ietf-restconf", NULL, yspec)< 0)
	goto done;
    
    /* Add netconf yang spec, used as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    
    /* Add system modules */
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040") &&
	 yang_spec_parse_module(h, "ietf-restconf-monitoring", NULL, yspec)< 0)
	 goto done;
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277") &&
	 yang_spec_parse_module(h, "clixon-rfc5277", NULL, yspec)< 0)
	 goto done;

     /* Here all modules are loaded 
      * Compute and set canonical namespace context
      */
     if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	 goto done;
     if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	 goto done;

     /* Dump configuration options on debug */
    if (dbg)      
	clicon_option_dump(h, dbg);

    /* Call start function in all plugins before we go interactive 
     */
     if (clixon_plugin_start_all(h) < 0)
	 goto done;

    if ((sockpath = clicon_option_str(h, "CLICON_RESTCONF_PATH")) == NULL){
	clicon_err(OE_CFG, errno, "No CLICON_RESTCONF_PATH in clixon configure file");
	goto done;
    }
    if (FCGX_Init() != 0){ /* How to cleanup memory after this? */
	clicon_err(OE_CFG, errno, "FCGX_Init");
	goto done;
    }
    clicon_debug(1, "restconf_main: Opening FCGX socket: %s", sockpath);
    if ((sock = FCGX_OpenSocket(sockpath, 10)) < 0){
	clicon_err(OE_CFG, errno, "FCGX_OpenSocket");
	goto done;
    }
#if 1
    {
    /* Change group of fcgi sock fronting reverse proxy to WWWUSER, the effective group is clicon
     * which is backend. */
	gid_t wgid = -1;
	if (group_name2gid(WWWUSER, &wgid) < 0){
	    clicon_log(LOG_ERR, "'%s' does not seem to be a valid user group.", WWWUSER);
	    goto done;
	}
	if (chown(sockpath, -1, wgid) < 0){
	    clicon_err(OE_CFG, errno, "chown");
	    goto done;
	}
    }
#endif
    if (clicon_socket_set(h, sock) < 0)
	goto done;
    /* umask settings may interfer: we want group to write: this is 774 */
    if (chmod(sockpath, S_IRWXU|S_IRWXG|S_IROTH) < 0){
	clicon_err(OE_UNIX, errno, "chmod");
	goto done;
    }
#if 1
    if (restconf_drop_privileges(h, WWWUSER) < 0)
	goto done;
#endif
    if (FCGX_InitRequest(req, sock, 0) != 0){
	clicon_err(OE_CFG, errno, "FCGX_InitRequest");
	goto done;
    }
    while (1) {
	finish = 1; /* If zero, dont finish request, initiate new */

	if (FCGX_Accept_r(req) < 0) {
	    clicon_err(OE_CFG, errno, "FCGX_Accept_r");
	    goto done;
	}
	clicon_debug(1, "------------");

	if (start == 0){
	    /* Send hello request to backend to get session-id back
	     * This is done once at the beginning of the session and then this is
	     * used by the client, even though new TCP sessions are created for
	     * each message sent to the backend.
	     */
	    if (clicon_hello_req(h, &id) < 0)
		goto done;
	    clicon_session_id_set(h, id);
	    start++;
	}
	/* Translate from FCGI parameter form to Clixon runtime data 
	 * XXX: potential name collision?
	 */
	if (fcgi_params_set(h, req->envp) < 0)
	    goto done;
	if ((path = restconf_param_get(h, "REQUEST_URI")) != NULL){
	    clicon_debug(1, "path: %s", path);
	    if (strncmp(path, "/" RESTCONF_API, strlen("/" RESTCONF_API)) == 0){
		char  *query = NULL;
		cvec  *qvec = NULL;
		query = restconf_param_get(h, "QUERY_STRING");
		if (query != NULL && strlen(query))
		    if (str2cvec(query, '&', '=', &qvec) < 0)
			goto done;
		api_root_restconf(h, req, qvec); /* This is the function */
		if (qvec){
		    cvec_free(qvec);
		    qvec = NULL;
		}
	    }
	    else if (strncmp(path+1, stream_path, strlen(stream_path)) == 0) {
		char  *query = NULL;
		cvec  *qvec = NULL;
		query = restconf_param_get(h, "QUERY_STRING");
		if (query != NULL && strlen(query))
		    if (str2cvec(query, '&', '=', &qvec) < 0)
			goto done;
		api_stream(h, req, qvec, stream_path, &finish); 
		if (qvec){
		    cvec_free(qvec);
		    qvec = NULL;
		}
	    }
	    else if (strncmp(path, RESTCONF_WELL_KNOWN, strlen(RESTCONF_WELL_KNOWN)) == 0) {
		api_well_known(h, req); /*  */
	    }
	    else{
		clicon_debug(1, "top-level %s not found", path);
		restconf_notfound(h, req);
	    }
	}
	else
	    clicon_debug(1, "NULL URI");
	if (restconf_param_del_all(h) < 0)
	    goto done;
	if (finish)
	    FCGX_Finish_r(req);
	else{ /* A handler is forked so we initiate a new request after instead 
		 of finishing the old */
	    if (FCGX_InitRequest(req, sock, 0) != 0){
		clicon_err(OE_CFG, errno, "FCGX_InitRequest");
		goto done;
	    }
	}
	
    } /* while */
    retval = 0;
 done:
    stream_child_freeall(h);
    restconf_terminate(h);
    return retval;
}
