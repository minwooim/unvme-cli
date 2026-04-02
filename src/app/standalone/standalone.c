// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * standalone.c - libunvmed standalone application
 *
 * Demonstrates direct use of libunvmed without the unvmed daemon:
 *   1. Initialize NVMe controller
 *   2. Create admin queues
 *   3. Enable controller
 *   4. Create per-CPU I/O SQ/CQ pairs
 *   5. Issue Identify Controller admin command and print results
 *
 * Usage: unvme-standalone <bdf>
 *   bdf: PCI bus-device-function address (e.g., 0000:01:00.0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>

#include <nvme/types.h>
#include <libunvmed.h>

#define ADMINQ_SIZE	256
#define IOQ_SIZE	256

static void print_id_ctrl(struct nvme_id_ctrl *id)
{
	printf("  Model Number (MN)  : %.40s\n", id->mn);
	printf("  Serial Number (SN) : %.20s\n", id->sn);
	printf("  Firmware (FR)      : %.8s\n", id->fr);
	printf("  Max Queue Entries  : %u\n", le16_to_cpu(id->mqes) + 1);
	printf("  Num Namespaces(NN) : %u\n", le32_to_cpu(id->nn));
}

int main(int argc, char *argv[])
{
	const char *bdf;
	struct unvme *u;
	int nr_cpus;
	int mps;
	int ret = 0;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <bdf>\n", argv[0]);
		fprintf(stderr, "  bdf: PCI bus-device-function (e.g., 0000:01:00.0)\n");
		return 1;
	}

	bdf = argv[1];
	nr_cpus = get_nprocs();
	mps = __builtin_ctz(getpagesize()) - 12;

	printf("=== libunvmed Standalone App ===\n");
	printf("Device : %s\n", bdf);
	printf("CPUs   : %d\n", nr_cpus);
	printf("\n");

	/* Step 1: Initialize controller */
	printf("[1] Initializing controller ...\n");
	u = unvmed_init_ctrl(bdf, nr_cpus);
	if (!u) {
		fprintf(stderr, "    FAIL: %s\n", strerror(errno));
		return 1;
	}
	printf("    OK\n");

	/* Step 2: Create admin queues */
	printf("[2] Creating admin queues (size=%d) ...\n", ADMINQ_SIZE);
	if (unvmed_create_adminq(u, ADMINQ_SIZE, ADMINQ_SIZE, true)) {
		fprintf(stderr, "    FAIL: %s\n", strerror(errno));
		ret = 1;
		goto free_ctrl;
	}
	printf("    OK (ASQ/ACQ size=%d)\n", ADMINQ_SIZE);

	/* Step 3: Enable controller */
	printf("[3] Enabling controller (iosqes=6, iocqes=4, mps=%d) ...\n", mps);
	if (unvmed_enable_ctrl(u, 6, 4, (uint8_t)mps, 0, 0, 0)) {
		fprintf(stderr, "    FAIL: %s\n", strerror(errno));
		ret = 1;
		goto free_ctrl;
	}
	printf("    OK (CSTS.RDY=1)\n");

	/* Step 4: Create per-CPU I/O CQ/SQ pairs (qid 1 .. nr_cpus) */
	printf("[4] Creating %d I/O queue pairs (size=%d) ...\n", nr_cpus, IOQ_SIZE);
	for (int i = 1; i <= nr_cpus; i++) {
		/* Create I/O CQ (polling mode: vector=-1) */
		if (unvmed_create_cq(u, i, IOQ_SIZE, -1, 1)) {
			fprintf(stderr, "    FAIL: create CQ qid=%d: %s\n",
				i, strerror(errno));
			ret = 1;
			goto free_ctrl;
		}

		/* Create I/O SQ paired with the CQ above */
		if (unvmed_create_sq(u, i, IOQ_SIZE, i, 0, 1, 0)) {
			fprintf(stderr, "    FAIL: create SQ qid=%d: %s\n",
				i, strerror(errno));
			ret = 1;
			goto free_ctrl;
		}

		printf("    qpair[%d]: SQ/CQ created\n", i);
	}
	printf("    OK\n");

	/* Step 5: Issue Identify Controller */
	printf("[5] Issuing Identify Controller ...\n");
	{
		const size_t size = NVME_IDENTIFY_DATA_SIZE;
		struct unvme_sq *asq;
		struct unvme_cmd *cmd;
		struct iovec iov;
		void *buf = NULL;
		ssize_t len;

		len = unvmed_pgmap(u, &buf, size);
		if (len < 0) {
			fprintf(stderr, "    FAIL: pgmap: %s\n", strerror(errno));
			ret = 1;
			goto free_ctrl;
		}

		iov.iov_base = buf;
		iov.iov_len = size;

		asq = unvmed_sq_get(u, 0);
		if (!asq) {
			fprintf(stderr, "    FAIL: get admin SQ\n");
			unvmed_pgunmap(buf);
			ret = 1;
			goto free_ctrl;
		}

		unvmed_sq_enter(asq);

		if (!unvmed_sq_ready(asq)) {
			fprintf(stderr, "    FAIL: admin SQ not ready\n");
			unvmed_sq_exit(asq);
			unvmed_sq_put(u, asq);
			unvmed_pgunmap(buf);
			ret = 1;
			goto free_ctrl;
		}

		cmd = unvmed_alloc_cmd(u, asq, NULL, buf, len);
		if (!cmd) {
			fprintf(stderr, "    FAIL: alloc cmd: %s\n", strerror(errno));
			unvmed_sq_exit(asq);
			unvmed_sq_put(u, asq);
			unvmed_pgunmap(buf);
			ret = 1;
			goto free_ctrl;
		}

		if (unvmed_cmd_prep_id_ctrl(cmd, &iov, 1) < 0) {
			fprintf(stderr, "    FAIL: prep id-ctrl: %s\n", strerror(errno));
			unvmed_sq_exit(asq);
			unvmed_cmd_put(cmd);
			unvmed_sq_put(u, asq);
			unvmed_pgunmap(buf);
			ret = 1;
			goto free_ctrl;
		}

		cmd->flags |= UNVMED_CMD_F_WAKEUP_ON_CQE;
		unvmed_cmd_post(cmd, &cmd->sqe, cmd->flags);
		unvmed_sq_exit(asq);

		unvmed_cmd_wait(cmd);
		ret = unvmed_cqe_status(&cmd->cqe);

		if (ret == 0 && nvme_cqe_ok(&cmd->cqe)) {
			printf("    OK\n\n");
			printf("--- Identify Controller ---\n");
			print_id_ctrl((struct nvme_id_ctrl *)buf);
		} else {
			fprintf(stderr, "    FAIL: CQE status=0x%x\n", ret);
			ret = 1;
		}

		unvmed_cmd_put(cmd);
		unvmed_sq_put(u, asq);
		unvmed_pgunmap(buf);
	}

free_ctrl:
	unvmed_free_ctrl(u);
	return ret;
}
