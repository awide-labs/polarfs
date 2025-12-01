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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "pfsd_option.h"
#include "pfsd_common.h"

unsigned int server_id = 0; /* db ins id */

pfsd_option_t g_option;

#define PFSD_TRIM_VALUE(v, min_v, max_v) do {\
	if (v > max_v) \
		v = max_v; \
	else if (v < min_v) \
		v = min_v; \
} while(0)

static bool
sanity_check()
{
	PFSD_TRIM_VALUE(g_option.o_workers, 1, PFSD_WORKER_MAX);
	PFSD_TRIM_VALUE(g_option.o_queues, -1, g_option.o_workers);

	if (strlen(g_option.o_pbdname) == 0) {
		fprintf(stderr, "pbdname is empty\n");
		return false;
	}

	fprintf(stderr, "option workers %d\n",g_option.o_workers);
	fprintf(stderr, "option queues %d\n",g_option.o_queues);
	fprintf(stderr, "option pbdname %s\n",g_option.o_pbdname);
	fprintf(stderr, "option server id %u\n", server_id);
	fprintf(stderr, "option logconf %s\n",g_option.o_log_cfg);

    return true;
}

static void __attribute__((constructor))
init_default_value()
{
	g_option.o_workers = 32;
	strncpy(g_option.o_log_cfg, "pfsd_logger.conf", sizeof g_option.o_log_cfg);
	g_option.o_daemon = 1;
	server_id = 0;
}

int
pfsd_parse_option(int ac, char *av[])
{
	int ch = 0;
	while ((ch = getopt(ac, av, "w:q:i:c:p:l:e:fd")) != -1) {
		switch (ch) {
			case 'f':
				g_option.o_daemon = 0;
				break;

			case 'd':
				g_option.o_daemon = 1;
				break;
			case 'w':
				{
					errno = 0;
					long w = strtol(optarg, NULL, 10);
					if (errno == 0)
						g_option.o_workers = int(w);
				}
				break;
			case 'q':
				{
					errno = 0;
					long q = strtol(optarg, NULL, 10);
					if (errno == 0)
						g_option.o_queues = int(q);
				}
				break;
			case 'i':
				break;
			case 'e':
				{
					errno = 0;
					long w = strtol(optarg, NULL, 10);
					if (errno == 0)
						server_id = (unsigned int)(w);
				}
				break;
			case 'c':
				strncpy(g_option.o_log_cfg, optarg, sizeof g_option.o_log_cfg);
				break;
			case 'p':
				strncpy(g_option.o_pbdname, optarg, sizeof g_option.o_pbdname);
				break;
			default:
				return -1;
		}
	}

	if (!sanity_check())
		return -1;

	if (optind != ac)
		return -1;

	return 0;
}

void
pfsd_usage(const char *prog)
{
	fprintf(stderr, "Usage: %s \n"
					" -f (not daemon mode)\n"
					" -w #nworkers\n"
					" -c log_config_file\n"
					" -p pbdname\n"
					" -e db ins id\n"
					" -i #inode_list_size\n", prog);
}
