/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <inttypes.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_timer.h>

#include "spdk/nvmf_spec.h"
#include "conn.h"
#include "rdma.h"
#include "request.h"
#include "session.h"
#include "subsystem.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/trace.h"


/** \file

*/

static void spdk_nvmf_conn_do_work(void *arg);

int
spdk_nvmf_startup_conn(struct spdk_nvmf_conn *conn)
{
	conn->state = CONN_STATE_RUNNING;
	conn->poller.fn = spdk_nvmf_conn_do_work;
	conn->poller.arg = conn;

	spdk_poller_register(&conn->poller, conn->sess->subsys->lcore, NULL);

	return 0;
}

void
spdk_nvmf_conn_destruct(struct spdk_nvmf_conn *conn)
{
	spdk_poller_unregister(&conn->poller, NULL);

	nvmf_disconnect(conn->sess, conn);
	nvmf_rdma_conn_cleanup(conn);
}

static void
spdk_nvmf_conn_do_work(void *arg)
{
	struct spdk_nvmf_conn *conn = arg;
	struct nvmf_session *session = conn->sess;

	/* process pending NVMe device completions */
	if (session) {
		if (conn->type == CONN_TYPE_AQ) {
			nvmf_check_admin_completions(session);
		} else {
			nvmf_check_io_completions(session);
		}
	}

	/* process pending RDMA completions */
	if (nvmf_check_rdma_completions(conn) < 0) {
		SPDK_ERRLOG("Transport poll failed for conn %p; closing connection\n", conn);
		conn->state = CONN_STATE_EXITING;
	}

	if (conn->state == CONN_STATE_EXITING ||
	    conn->state == CONN_STATE_FABRIC_DISCONNECT) {
		spdk_nvmf_conn_destruct(conn);
		if (session && (session->num_connections == 0)) {
			spdk_nvmf_session_destruct(session);
		}
	}
}
