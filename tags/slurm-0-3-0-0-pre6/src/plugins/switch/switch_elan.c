/*****************************************************************************\
 *  switch_elan.c - Library routines for initiating jobs on QsNet. 
 *****************************************************************************
 *  Copyright (C) 2003 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Kevin Tew <tew1@llnl.gov>, et. al.
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#if     HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif /* WITH_PTHREADS */

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <slurm/slurm_errno.h>

#include "src/common/bitstring.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/setenvpf.h"
#include "src/common/switch.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/plugins/switch/qsw.h"

#define BUF_SIZE 1024

#ifdef HAVE_LIBELAN3
#include <elan3/elan3.h>
/*
 * Static prototypes for network error resolver creation:
 */
static int   _set_elan_ids(void);
static void *_neterr_thr(void *arg);

static int             neterr_retval = 0;
static pthread_t       neterr_tid;
static pthread_mutex_t neterr_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  neterr_cond  = PTHREAD_COND_INITIALIZER;

#endif /* HAVE_LIBELAN3 */

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobcomp" for SLURM job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  SLURM will
 * only load job completion logging plugins if the plugin_type string has a 
 * prefix of "jobcomp/".
 *
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum versions for their plugins as the job completion logging API 
 * matures.
 */
const char plugin_name[]        = "switch Quadrics Elan3 or Elan4 plugin";
const char plugin_type[]        = "switch/elan";
const uint32_t plugin_version   = 90;

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
int init ( void )
{
	return SLURM_SUCCESS;
}

int fini ( void )
{
	return SLURM_SUCCESS;
}

/*
 * switch functions for global state save/restore
 */
int switch_p_libstate_save (char *dir_name)
{
	int error_code = SLURM_SUCCESS;
	qsw_libstate_t old_state = NULL;
	Buf buffer = NULL;
	int state_fd;
	char *file_name;

	if (qsw_alloc_libstate(&old_state))
		return errno;
	qsw_fini(old_state);
	buffer = init_buf(1024);
	(void) qsw_pack_libstate(old_state, buffer);
	file_name = xstrdup(dir_name);
	xstrcat(file_name, "/qsw_state");
	(void) unlink(file_name);
	state_fd = creat (file_name, 0600);
	if (state_fd == 0) {
		error ("Can't save state, error creating file %s %m",
			file_name);
		error_code = errno;
	} else {
		char  *buf = get_buf_data(buffer);
		size_t len =get_buf_offset(buffer);
		while(1) {
			int wrote = write (state_fd, buf, len);
			if ((wrote < 0) && (errno == EINTR))
				continue;
			if (wrote == 0)
				break;
			if (wrote < 0) {
				error ("Can't save switch state: %m");
				error_code = errno;
				break;
			}
			buf += wrote;
			len -= wrote;
		}
		close (state_fd);
	}
	xfree(file_name);

	if (buffer)
		free_buf(buffer);
	if (old_state)
		qsw_free_libstate(old_state);

	return error_code;
}

int switch_p_libstate_restore (char *dir_name)
{
	char *data = NULL, *file_name;
	qsw_libstate_t old_state = NULL;
	Buf buffer = NULL;
	int error_code = SLURM_SUCCESS;
	int state_fd, data_allocated = 0, data_read = 0, data_size = 0;

	if (dir_name == NULL)	/* clean start, no recovery */
		return qsw_init(NULL);
	
	file_name = xstrdup(dir_name);
	xstrcat(file_name, "/qsw_state");
	state_fd = open (file_name, O_RDONLY);
	if (state_fd >= 0) {
		data_allocated = BUF_SIZE;
		data = xmalloc(data_allocated);
		while (1) {
			data_read = read (state_fd, &data[data_size],
					BUF_SIZE);
			if ((data_read < 0) && (errno == EINTR))
				continue;
			if (data_read < 0) {
				error ("Read error on %s, %m", file_name);
				error_code = SLURM_ERROR;
				break;
			} else if (data_read == 0)
				break;
			data_size      += data_read;
			data_allocated += data_read;
			xrealloc(data, data_allocated);
		}
		close (state_fd);
	} else {
		error("No %s file for QSW state recovery", file_name);
		error("Starting QSW with clean state");
		return qsw_init(NULL);
	}
	xfree(file_name);

	if (error_code == SLURM_SUCCESS) {
		if (qsw_alloc_libstate(&old_state)) {
			error_code = SLURM_ERROR;
		} else {
			buffer = create_buf (data, data_size);
			if (qsw_unpack_libstate(old_state, buffer) < 0)
				error_code = errno;
		}
	}

	if (buffer)
		free_buf(buffer);
	else if (data)
		xfree(data);

	if (error_code == SLURM_SUCCESS)
		error_code = qsw_init(old_state);
	if (old_state)
		qsw_free_libstate(old_state);

	return error_code;
}

bool switch_p_no_frag ( void )
{
	return true;
}

/*
 * switch functions for job step specific credential
 */
int switch_p_alloc_jobinfo(switch_jobinfo_t *jp)
{
	return qsw_alloc_jobinfo((qsw_jobinfo_t *)jp);
}

int switch_p_build_jobinfo ( switch_jobinfo_t switch_job, char *nodelist,
		int nprocs, int cyclic_alloc)
{
	int node_set_size = QSW_MAX_TASKS; /* overkill but safe */
	hostlist_t host_list;
	char *this_node_name;
	bitstr_t *nodeset;
	int node_id, error_code = SLURM_SUCCESS;

	if (nprocs > node_set_size)
		return ESLURM_BAD_TASK_COUNT;
	if ((nodeset = bit_alloc (node_set_size)) == NULL)
		fatal("bit_alloc: %m");


	if ((host_list = hostlist_create(nodelist)) == NULL)
		fatal("hostlist_create(%s): %m", nodelist);
	while ((this_node_name = hostlist_shift(host_list))) {
		node_id = qsw_getnodeid_byhost(this_node_name);
		if (node_id >= 0)
			bit_set(nodeset, node_id);
		else {
			error("qsw_getnodeid_byhost(%s) failure", 
					this_node_name);
			error_code = ESLURM_INTERCONNECT_FAILURE;
		}
		free(this_node_name);
	}
	hostlist_destroy(host_list);

	if (error_code == SLURM_SUCCESS) {
		qsw_jobinfo_t j = (qsw_jobinfo_t) switch_job;
		error_code = qsw_setup_jobinfo(j, nprocs, nodeset, 
				cyclic_alloc);
	}

	bit_free(nodeset);
	return error_code;
}

switch_jobinfo_t switch_p_copy_jobinfo(switch_jobinfo_t j)
{
	return (switch_jobinfo_t) qsw_copy_jobinfo((qsw_jobinfo_t) j);
}

void switch_p_free_jobinfo(switch_jobinfo_t k)
{
	qsw_free_jobinfo((qsw_jobinfo_t) k);
}

int switch_p_pack_jobinfo(switch_jobinfo_t k, Buf buffer)
{
	return qsw_pack_jobinfo((qsw_jobinfo_t) k, buffer);
}

int switch_p_unpack_jobinfo(switch_jobinfo_t k, Buf buffer)
{
	return qsw_unpack_jobinfo((qsw_jobinfo_t) k, buffer);
}

void switch_p_print_jobinfo(FILE *fp, switch_jobinfo_t jobinfo)
{
	qsw_print_jobinfo(fp, (qsw_jobinfo_t) jobinfo);
}

char *switch_p_sprint_jobinfo(switch_jobinfo_t switch_jobinfo, char *buf,
		size_t size)
{
	return qsw_capability_string((struct qsw_jobinfo *) switch_jobinfo,
		       buf, size);
}

/*
 * switch functions for job initiation
 */

/*  Initialize node for use of the Elan interconnect by loading 
 *   elanid/hostname pairs then spawning the Elan network error
 *   resover thread.
 *
 *  Main thread waits for neterr thread to successfully start before
 *   continuing.
 */
int switch_p_node_init ( void )
{
#if HAVE_LIBELAN3
	int err = 0;
	pthread_attr_t attr;

        /* 
         *  We only know how to do this for Elan3 right now
         */

	/*
	 *  Load neterr elanid/hostname values into kernel 
	 */
	if (_set_elan_ids() < 0)
		return SLURM_FAILURE;

	if ((err = pthread_attr_init(&attr)))
		error("pthread_attr_init: %s", slurm_strerror(err));

	err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (err)
		error("pthread_attr_setdetachstate: %s", slurm_strerror(err));

	slurm_mutex_lock(&neterr_mutex);

	if ((err = pthread_create(&neterr_tid, &attr, _neterr_thr, NULL)))
		return SLURM_FAILURE;

	/*
	 *  Wait for successful startup of neterr thread before
	 *   returning control to slurmd.
	 */
	pthread_cond_wait(&neterr_cond, &neterr_mutex);
	pthread_mutex_unlock(&neterr_mutex);

	return neterr_retval;

#else  /* !HAVE_LIBELAN3 */

        return SLURM_SUCCESS;
#endif /*  HAVE_LIBELAN3 */
}

#if HAVE_LIBELAN3
static void *_neterr_thr(void *arg)
{	
	debug3("Starting Elan network error resolver thread");

	if (!elan3_init_neterr_svc(0)) {
		error("elan3_init_neterr_svc: %m");
		goto fail;
	}

	/* 
	 *  Attempt to register the neterr svc thread. If the address 
	 *   cannot be bound, then there is already a thread running, and
	 *   we should just exit with success.
	 */
	if (!elan3_register_neterr_svc()) {
		if (errno != EADDRINUSE) {
			error("elan3_register_neterr_svc: %m");
			goto fail;
		}
		info("Warning: Elan error resolver thread already running");
	}

	/* 
	 *  Signal main thread that we've successfully initialized
	 */
	slurm_mutex_lock(&neterr_mutex);
	neterr_retval = 0;
	pthread_cond_signal(&neterr_cond);
	slurm_mutex_unlock(&neterr_mutex);

	/*
	 *  Run the network error resolver thread. This should
	 *   never return. If it does, there's not much we can do
	 *   about it.
	 */
	elan3_run_neterr_svc();

	return NULL;

   fail:
	slurm_mutex_lock(&neterr_mutex);
	neterr_retval = SLURM_FAILURE;
	pthread_cond_signal(&neterr_cond);
	slurm_mutex_unlock(&neterr_mutex);

	return NULL;
}
#endif /* HAVE_LIBELAN3 */

/*
 *  Called from slurmd just before termination.
 *   We don't really need to do anything special for Elan, but
 *   we'll call pthread_cancel() on the neterr resolver thread anyhow.
 */
int switch_p_node_fini ( void )
{
#if HAVE_LIBELAN3
	int err = pthread_cancel(neterr_tid);
	if (err == 0) 
		return SLURM_SUCCESS;

	error("Unable to cancel neterr thread: %s", slurm_strerror(err));
	return SLURM_FAILURE;
#else  /* !HAVE_LIBELAN3 */

        return SLURM_SUCCESS;
#endif /*  HAVE_LIBELAN3 */
}

static int 
_wait_and_destroy_prg(qsw_jobinfo_t qsw_job)
{
	int i = 0;
	int sleeptime = 1;

	debug("going to destroy program description...");

	while((qsw_prgdestroy(qsw_job) < 0) && (errno == ECHILD_PRGDESTROY)) {
		debug("qsw_prgdestroy: %m");
		i++;
		if (i == 1) {
			debug("sending SIGTERM to remaining tasks");
			qsw_prgsignal(qsw_job, SIGTERM);
		} else {
			debug("sending SIGKILL to remaining tasks");
			qsw_prgsignal(qsw_job, SIGKILL);
		}

		debug("sleeping for %d sec ...", sleeptime);
		sleep(sleeptime*=2);
	}

	debug("destroyed program description");
	return SLURM_SUCCESS;
}

int switch_p_job_preinit ( switch_jobinfo_t jobinfo )
{
	return SLURM_SUCCESS;
}

/* 
 * prepare node for interconnect use
 */
int switch_p_job_init ( switch_jobinfo_t jobinfo, uid_t uid )
{
	char buf[4096];

	debug2("calling interconnect_init from process %lu", 
		(unsigned long) getpid());
	verbose("ELAN: %s", qsw_capability_string(
		(qsw_jobinfo_t)jobinfo, buf, 4096));

	if (qsw_prog_init((qsw_jobinfo_t)jobinfo, uid) < 0) {
		/*
		 * Check for EBADF, which probably means the rms
		 *  kernel module is not loaded.
		 */
		if (errno == EBADF)
			error("Initializing interconnect: "
			      "is the rms kernel module loaded?");
		else
			error ("elan_interconnect_init: %m");

		qsw_print_jobinfo(log_fp(), (qsw_jobinfo_t)jobinfo);
		return SLURM_ERROR;
	}
	
	return SLURM_SUCCESS; 
}

int switch_p_job_fini ( switch_jobinfo_t jobinfo )
{
	qsw_prog_fini((qsw_jobinfo_t)jobinfo); 
	return SLURM_SUCCESS;
}

int switch_p_job_postfini ( switch_jobinfo_t jobinfo, uid_t pgid, 
				uint32_t job_id, uint32_t step_id )
{
	_wait_and_destroy_prg((qsw_jobinfo_t)jobinfo);
	return SLURM_SUCCESS;
}

int switch_p_job_attach ( switch_jobinfo_t jobinfo, char ***env, 
			uint32_t nodeid, uint32_t procid, uint32_t nnodes, 
			uint32_t nprocs, uint32_t rank )
{
	debug3("nodeid=%lu nnodes=%lu procid=%lu nprocs=%lu rank=%lu", 
		(unsigned long) nodeid, (unsigned long) nnodes, 
		(unsigned long) procid, (unsigned long) nprocs, 
		(unsigned long) rank);
	debug3("setting capability in process %lu", 
		(unsigned long) getpid());
	if (qsw_setcap((qsw_jobinfo_t) jobinfo, (int) procid) < 0) {
		error("qsw_setcap: %m");
		return SLURM_ERROR;
	}

	if (setenvpf(env, "RMS_RANK",   "%lu", (unsigned long) rank  ) < 0)
		return SLURM_ERROR;
	if (setenvpf(env, "RMS_NODEID", "%lu", (unsigned long) nodeid) < 0)
		return SLURM_ERROR;
	if (setenvpf(env, "RMS_PROCID", "%lu", (unsigned long) rank  ) < 0)
		return SLURM_ERROR;
	if (setenvpf(env, "RMS_NNODES", "%lu", (unsigned long) nnodes) < 0)
		return SLURM_ERROR;
	if (setenvpf(env, "RMS_NPROCS", "%lu", (unsigned long) nprocs) < 0)
		return SLURM_ERROR;

	return SLURM_SUCCESS;
}

#if HAVE_LIBELAN3

static int 
_set_elan_ids(void)
{
	int i;

	for (i = 0; i <= qsw_maxnodeid(); i++) {
		char host[256]; 
		if (qsw_gethost_bynodeid(host, 256, i) < 0)
			continue;
			
		if (elan3_load_neterr_svc(i, host) < 0)
			error("elan3_load_neterr_svc(%d, %s): %m", i, host);
	}

	return 0;
}

#endif
