/*****************************************************************************\
 *  burst_buffer.h - driver for burst buffer infrastructure and plugin
 *****************************************************************************
 *  Copyright (C) 2014 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifndef _SLURM_BURST_BUFFER_H
#define _SLURM_BURST_BUFFER_H

#include "slurm/slurm.h"
#include "src/slurmctld/slurmctld.h"

/*
 * Initialize the burst buffer infrastructure.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_init(void);

/*
 * Terminate the burst buffer infrastructure. Free memory.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_fini(void);

/*
 **************************************************************************
 *                          P L U G I N   C A L L S                       *
 **************************************************************************
 */

/*
 * Load the current burst buffer state (e.g. how much space is available now).
 * Run at the beginning of each scheduling cycle in order to recognize external
 * changes to the burst buffer state (e.g. capacity is added, removed, fails,
 * etc.)
 *
 * Returns a SLURM errno.
 */
extern int bb_g_load_state(void);

/*
 * Note configuration may have changed. Handle changes in BurstBufferParameters.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_reconfig(void);

/*
 * Validate a job submit request with respect to burst buffer options.
 *
 * Returns a SLURM errno.
 */
extern int bb_g_job_validate(struct job_descriptor *job_desc,
			     uid_t submit_uid);

/*
 * Determine if a job's burst buffer stage in is complete
 *
 * RET: 0 - stage in is underway
 *      1 - stage in complete
 *     -1 - fatal error
 */
extern int bb_g_job_test_stage_in(struct job_record *job_ptr);

#endif /* !_SLURM_BURST_BUFFER_H */
