/*
 * Copyright (c) 2017-2021, Alibaba Group Holding Limited
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ipc/shm.h"
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <vector>

#include "pfsd_common.h"
#include "pfsd_worker.h"
#include "pfsd_option.h"

#include "pfs_trace.h"
#include "pfsd_zlog.h"

#include "ipc/server.h"

static void
signal_handler(int num)
{
	g_stop = true;
}

static void
reload_handler(int num)
{
}

/* used for libpfs logger */
zlog_category_t *original_zlog_cat = NULL;

static void
handle_request(ipc::RequestPtr rp, ipc::Server *server)
{
	auto [r, lk] = server->getPtrWithLock<ipc::Request>(
		rp.connectionId, rp.memBufId, rp.offset);
	if (r == nullptr) {
		/* request not found means that the request issuer already
		disconnected (or crashed) and it's mmap'ed buffers cleaned up. */
		return;
	}
	pfsd_worker_handle_request(server, rp.connectionId, r);
	sem_post(&r->sem);
}

static void
queue_worker(ipc::Queue *q, std::vector<ipc::Queue *> other_queues,
	     ipc::Server *server, int worker_id)
{
	int idx = 0;
	while (!g_stop) {
		ipc::RequestPtr rp;
		if (q->try_pop(rp)) {
			handle_request(rp, server);
			continue;
		}
		int count = other_queues.size();
		while (count > 0) {
			if (other_queues[idx]->try_pop(rp)) {
				handle_request(rp, server);
				break;
			}
			count--;
			idx = (idx + 1) % other_queues.size();
		}
		q->pop(rp);
		handle_request(rp, server);
	}
}

int main(int ac, char *av[])
{
	const char *pbdname;
	int err;
	if (pfsd_parse_option(ac, av) != 0) {
		pfsd_usage(av[0]);
		return -1;
	}

	if (ac == 1)
		pfsd_usage(av[0]);

	pbdname = g_option.o_pbdname;
	err = pfsd_write_pid(pbdname);
	if (err != 0) {
		fprintf(stderr, "pfsd %s may already running, err %d.\n",
		    pbdname, err);
		return -1;
	}

	/* init signal */
	struct sigaction sig;
	memset(&sig, 0, sizeof(sig));
	sig.sa_handler = signal_handler;
	sigaction(SIGINT, &sig, NULL);
	sig.sa_handler = reload_handler;
	sigaction(SIGHUP, &sig, NULL);
	sig.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sig, NULL);

	if (g_option.o_daemon)
		daemon(1, 1);

	/* init logger: use env for pass logdir to zlog */
	if (setenv("PFSD_PBDNAME", pbdname, 1) != 0) {
		fprintf(stderr, "set env [%s] failed: %s\n", pbdname, strerror(errno));
		return -1;
	}
	char logdir[PFS_MAX_PATHLEN] = "";
	snprintf(logdir, PFS_MAX_PATHLEN-1, "/var/log/pfsd-%s", pbdname);
	mkdir(logdir, 0777);
	int rv = LogInit(g_option.o_log_cfg, (char *)"pfsd_cat");
	if (rv != 0) {
		fprintf(stderr, "Error: init log failed, ret:%d\n", rv);
		return rv;
	}

	/* init libpfs logger */
	original_zlog_cat = zlog_get_category("original_cat");
	if (original_zlog_cat == NULL) {
		pfsd_error("why no original category");
		original_zlog_cat = zlog_get_category("pfsd_cat");
	}

	pfs_log_functor = wrapper_zlog;

	fprintf(stderr, "starting pfsd[%d] %s\n", pfs_getpid(), pbdname);
	pfsd_info("starting pfsd[%d] %s", pfs_getpid(), pbdname);

	std::vector<std::unique_ptr<ipc::Queue> > queues;
	std::vector<ipc::Queue *> queue_pointers;
	pfsutil::EventLoop evb;
	auto server = std::make_unique<ipc::Server>(evb, pbdname);

	g_nworkers = g_option.o_workers;

	int n_queues = g_option.o_queues;
	if (n_queues <= 0) {
		n_queues = std::thread::hardware_concurrency();
	}

	for (int i = 0; i < n_queues; i++) {
		const size_t capacity = 1024;
		size_t memSize = ipc::Queue::memorySizeForCapacity(capacity);
		auto memfd = std::make_unique<ipc::MemFd>(
			"queue-" + std::to_string(i), memSize);
		auto queue =
			ipc::makeQueue(memfd->buf(), memfd->size(), capacity);

		server->addQueue(std::move(memfd));
		queue_pointers.push_back(queue.get());
		queues.push_back(std::move(queue));
	}

  std::vector<std::thread> workers;
  int i = 0, nq = 0;
  for (i = 0; i < g_nworkers; i++) {
	  workers.emplace_back(queue_worker, queue_pointers[nq % n_queues],
			       queue_pointers, server.get(), i);
	  nq++;
  }

  server->start();

  for (auto &worker : workers) {
	  worker.join();
  }

  return 0;

	pfsd_info("[pfsd]bye bye");
	return 0;
}

