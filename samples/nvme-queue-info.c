// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvme-queue-info.c - Create NVMe Admin/IO queues and print queue addresses
 *
 * Usage: nvme-queue-info --bdf=<BDF>
 *
 * Example: nvme-queue-info --bdf=0000:01:00.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

#include <libunvmed.h>

#define ADMINQ_SIZE	256
#define IOQ_SIZE	256

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s --bdf=<BDF>\n", prog);
	fprintf(stderr, "  --bdf=<BDF>  PCI BDF address (e.g. 0000:01:00.0)\n");
}

int main(int argc, char *argv[])
{
	static struct option opts[] = {
		{ "bdf", required_argument, NULL, 'b' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	char bdf[32] = { 0 };
	struct unvme *u;
	struct unvme_sq *usq;
	struct unvme_cq *ucq;
	int opt, ret;

	while ((opt = getopt_long(argc, argv, "b:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'b':
			if (unvmed_parse_bdf(optarg, bdf) < 0) {
				fprintf(stderr, "error: invalid BDF '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!bdf[0]) {
		fprintf(stderr, "error: --bdf is required\n");
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	/*
	 * Initialize the NVMe controller.  max_nr_ioqs=2 is enough for qid=1.
	 */
	u = unvmed_init_ctrl(bdf, 2);
	if (!u) {
		fprintf(stderr, "error: failed to initialize controller '%s': %s\n",
			bdf, strerror(errno));
		return EXIT_FAILURE;
	}
	printf("Controller initialized: %s\n", bdf);

	/*
	 * Configure Admin SQ and CQ.  Must be done before enabling the
	 * controller.  No IRQ (irq=false) for simplicity.
	 */
	ret = unvmed_create_adminq(u, ADMINQ_SIZE, ADMINQ_SIZE, false);
	if (ret < 0) {
		fprintf(stderr, "error: failed to create admin queue: %s\n",
			strerror(errno));
		goto free_ctrl;
	}
	printf("Admin queue created (SQ/CQ size=%d)\n", ADMINQ_SIZE);

	/*
	 * Enable the controller.
	 *   iosqes=6  -> SQ entry size = 2^6 = 64 bytes
	 *   iocqes=4  -> CQ entry size = 2^4 = 16 bytes
	 *   mps=0     -> memory page size = 4KB
	 *   ams=0     -> round robin arbitration
	 *   css=0     -> NVM command set
	 *   timeout=60 seconds
	 */
	ret = unvmed_enable_ctrl(u, 6, 4, 0, 0, 0, 60);
	if (ret < 0) {
		fprintf(stderr, "error: failed to enable controller: %s\n",
			strerror(errno));
		goto free_ctrl;
	}
	printf("Controller enabled\n");

	/*
	 * Create I/O Completion Queue with qid=1.
	 *   vector=0 -> no MSI-X interrupt
	 *   pc=1     -> physically contiguous
	 */
	ret = unvmed_create_cq(u, 1, IOQ_SIZE, 0, 1);
	if (ret < 0) {
		fprintf(stderr, "error: failed to create I/O CQ (qid=1): %s\n",
			strerror(errno));
		goto free_ctrl;
	}
	printf("I/O CQ created (qid=1, size=%d)\n", IOQ_SIZE);

	/*
	 * Create I/O Submission Queue with qid=1, associated to CQ qid=1.
	 *   qprio=0    -> urgent (round robin)
	 *   pc=1       -> physically contiguous
	 *   nvmsetid=0 -> default NVM set
	 */
	ret = unvmed_create_sq(u, 1, IOQ_SIZE, 1, 0, 1, 0);
	if (ret < 0) {
		fprintf(stderr, "error: failed to create I/O SQ (qid=1): %s\n",
			strerror(errno));
		goto free_ctrl;
	}
	printf("I/O SQ created (qid=1, size=%d, cqid=1)\n", IOQ_SIZE);

	/*
	 * Retrieve queue instances and print their memory addresses.
	 */
	usq = unvmed_sq_get(u, 1);
	if (!usq) {
		fprintf(stderr, "error: failed to get SQ (qid=1)\n");
		goto free_ctrl;
	}

	ucq = unvmed_cq_get(u, 1);
	if (!ucq) {
		fprintf(stderr, "error: failed to get CQ (qid=1)\n");
		unvmed_sq_put(u, usq);
		goto free_ctrl;
	}

	printf("\n--- I/O Queue Addresses (qid=1) ---\n");
	printf("SQ vaddr : %p\n",  usq->q->mem.vaddr);
	printf("SQ iova  : 0x%016" PRIx64 "\n", (uint64_t)usq->q->mem.iova);
	printf("CQ vaddr : %p\n",  ucq->q->mem.vaddr);
	printf("CQ iova  : 0x%016" PRIx64 "\n", (uint64_t)ucq->q->mem.iova);

	unvmed_sq_put(u, usq);
	unvmed_cq_put(u, ucq);

	unvmed_free_ctrl(u);
	return EXIT_SUCCESS;

free_ctrl:
	unvmed_free_ctrl(u);
	return EXIT_FAILURE;
}
