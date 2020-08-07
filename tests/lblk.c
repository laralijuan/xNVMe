// Copyright (C) Simon A. F. Lund <simon.lund@samsung.com>
// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <errno.h>
#include <liblblk.h>
#include <libxnvmec.h>

/**
 * Constructs an LBA range if --slba and --elba are not provided by CLI
 *  - Stored in [rng_slba, rng_elba]
 *
 * Allocates buffers to support transfers of mdts_naddr
 *  - wbuf -- to be used for writes
 *  - rbuf -- to be used for read
 */
static int
boilerplate(struct xnvmec *cli, uint8_t **wbuf, uint8_t **rbuf,
	    size_t *buf_nbytes, uint64_t *mdts_naddr, uint32_t *nsid,
	    uint64_t *rng_slba, uint64_t *rng_elba)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_geo *geo = cli->args.geo;
	uint64_t rng_naddr;
	int err;

	*nsid = xnvme_dev_get_nsid(cli->args.dev);

	*rng_slba = cli->args.slba;
	*rng_elba = cli->args.elba;

	*mdts_naddr = XNVME_MIN(geo->mdts_nbytes / geo->lba_nbytes, 256);
	*buf_nbytes = (*mdts_naddr) * geo->lba_nbytes;

	// Construct a range if none is given
	if (!(cli->given[XNVMEC_OPT_SLBA] && cli->given[XNVMEC_OPT_ELBA])) {
		*rng_slba = 0;
		*rng_elba = (1 << 28) / geo->lba_nbytes;	// About 256MB
	}
	rng_naddr = *rng_elba - *rng_slba + 1;

	// Verify range
	if (*rng_elba <= *rng_slba) {
		err = -EINVAL;
		xnvmec_perr("Invalid range: [rng_slba,rng_elba]", err);
		return err;
	}
	// TODO: verify that the range is sufficiently large

	*wbuf = xnvme_buf_alloc(dev, *buf_nbytes, NULL);
	if (!*wbuf) {
		err = -errno;
		xnvmec_perr("xnvme_buf_alloc()", err);
		return err;
	}

	*rbuf = xnvme_buf_alloc(dev, *buf_nbytes, NULL);
	if (!*rbuf) {
		err = -errno;
		xnvmec_perr("xnvme_buf_alloc()", err);
		xnvme_buf_free(dev, *rbuf);
		return err;
	}

	xnvmec_pinf("nsid: 0x%x", *nsid);
	xnvmec_pinf("range: { slba: 0x%016x, elba: 0x%016x, naddr: %zu }",
		    *rng_slba, *rng_elba, rng_naddr);
	xnvmec_pinf("buf_nbytes: %zu", *buf_nbytes);
	xnvmec_pinf("mdts_naddr: %zu", *mdts_naddr);

	return 0;
}

/**
 * 0) Fill wbuf with '!'
 * 1) Write the entire LBA range [slba, elba] using wbuf
 * 2) Fill wbuf with a repeating sequence of letters A to Z
 * 3) Scatter the content of wbuf within [slba,elba]
 * 4) Read, with exponential stride, within [slba,elba] using rbuf
 * 5) Verify that the content of rbuf is the same as wbuf
 */
static int
sub_io(struct xnvmec *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_geo *geo = cli->args.geo;
	uint32_t nsid;
	uint64_t rng_slba, rng_elba, mdts_naddr;
	size_t buf_nbytes;
	uint8_t *wbuf = NULL, *rbuf = NULL;
	int err;

	err = boilerplate(cli, &wbuf, &rbuf, &buf_nbytes, &mdts_naddr, &nsid,
			  &rng_slba, &rng_elba);
	if (err) {
		xnvmec_perr("boilerplate()", err);
		goto exit;
	}

	xnvmec_pinf("Writing '!' to LBA range [slba,elba]");
	memset(wbuf, '!', buf_nbytes);
	for (uint64_t slba = rng_slba; slba < rng_elba; slba += mdts_naddr) {
		uint64_t nlb = XNVME_MIN(rng_elba - slba, mdts_naddr) - 1;
		struct xnvme_req req = { 0 };

		err = xnvme_cmd_write(dev, nsid, slba, nlb, wbuf, NULL,
				      XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			xnvmec_pinf("xnvme_cmd_write(): "
				    "{err: 0x%x, slba: 0x%016x}",
				    err, slba);
			xnvme_req_pr(&req, XNVME_PR_DEF);
			goto exit;
		}
	}

	xnvmec_pinf("Writing payload scattered within LBA range [slba,elba]");
	err = xnvmec_buf_fill(wbuf, buf_nbytes, "anum");
	if (err) {
		xnvmec_perr("xnvmec_buf_fill()", err);
		goto exit;
	}

	for (uint64_t count = 0; count < mdts_naddr; ++count) {
		size_t wbuf_ofz = count * geo->lba_nbytes;
		uint64_t slba = rng_slba + count * 4;
		struct xnvme_req req = { 0 };
		int err = 0;

		err = xnvme_cmd_write(dev, nsid, slba, 0, wbuf + wbuf_ofz, NULL,
				      XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			xnvmec_pinf("xnvme_cmd_write(): "
				    "{err: 0x%x, slba: 0x%016x}",
				    err, slba);
			xnvme_req_pr(&req, XNVME_PR_DEF);
			goto exit;
		}
	}

	xnvmec_pinf("Read scattered payload within LBA range [slba,elba]");

	xnvmec_buf_clear(rbuf, buf_nbytes);
	for (uint64_t count = 0; count < mdts_naddr; ++count) {
		size_t rbuf_ofz = count * geo->lba_nbytes;
		uint64_t slba = rng_slba + count * 4;
		struct xnvme_req req = { 0 };

		err = xnvme_cmd_read(dev, nsid, slba, 0, rbuf + rbuf_ofz, NULL,
				     XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			xnvmec_pinf("xnvme_cmd_read(): "
				    "{err: 0x%x, slba: 0x%016x}",
				    err, slba);
			xnvme_req_pr(&req, XNVME_PR_DEF);
			goto exit;
		}
	}

	xnvmec_pinf("Comparing wbuf and rbuf");
	if (xnvmec_buf_diff(wbuf, rbuf, buf_nbytes)) {
		xnvmec_buf_diff_pr(wbuf, rbuf, buf_nbytes, XNVME_PR_DEF);
		goto exit;
	}

exit:
	xnvme_buf_free(dev, wbuf);
	xnvme_buf_free(dev, rbuf);

	return err;
}

/**
 * 0) Fill wbuf with '!'
 *
 * 1) Write the entire LBA range [slba, elba] using wbuf
 *
 * 2) Fill wbuf with a repeating sequence of letters A to Z
 *
 * 3) Scatter the content of wbuf within [slba,elba]
 *
 * 4) Read, with exponential stride, within [slba,elba] using rbuf
 *
 * 5) Verify that the content of rbuf is the same as wbuf
 */
static int
test_scopy(struct xnvmec *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_geo *geo = cli->args.geo;
	uint32_t nsid;
	uint64_t rng_slba, rng_elba, mdts_naddr;
	size_t buf_nbytes;
	uint8_t *wbuf = NULL, *rbuf = NULL;

	struct lblk_source_range *sranges = NULL;	// For the copy-payload
	uint64_t sdlba = 0;
	enum lblk_scopy_fmt copy_fmt;
	int err;

	err = boilerplate(cli, &wbuf, &rbuf, &buf_nbytes, &mdts_naddr, &nsid,
			  &rng_slba, &rng_elba);
	if (err) {
		xnvmec_perr("boilerplate()", err);
		goto exit;
	}

	sranges = xnvme_buf_alloc(dev, sizeof(*sranges), NULL);
	if (!wbuf) {
		err = -errno;
		xnvmec_perr("xnvme_buf_alloc()", err);
		goto exit;
	}
	memset(sranges, 0, sizeof(*sranges));

	// Copy to the end of [slba,elba]
	sdlba = rng_elba - mdts_naddr;

	// NVMe-struct copy format
	copy_fmt = LBLK_SCOPY_FMT_ZERO;

	lblk_idfy_ctrlr_pr((struct lblk_idfy_ctrlr *)xnvme_dev_get_ctrlr(dev),
			   XNVME_PR_DEF);
	lblk_idfy_ns_pr((struct lblk_idfy_ns *)xnvme_dev_get_ns(dev),
			XNVME_PR_DEF);

	xnvmec_pinf("Writing '!' to LBA range [slba,elba]");
	memset(wbuf, '!', buf_nbytes);
	for (uint64_t slba = rng_slba; slba < rng_elba; slba += mdts_naddr) {
		uint64_t nlb = XNVME_MIN(rng_elba - slba, mdts_naddr) - 1;
		struct xnvme_req req = { 0 };

		err = xnvme_cmd_write(dev, nsid, slba, nlb, wbuf, NULL,
				      XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			xnvmec_perr("xnvme_cmd_write()", err);
			xnvme_req_pr(&req, XNVME_PR_DEF);
			err = err ? err : -EIO;
			goto exit;
		}
	}

	xnvmec_pinf("Writing payload scattered within LBA range [slba,elba]");
	xnvmec_buf_fill(wbuf, buf_nbytes, "anum");
	for (uint64_t count = 0; count < mdts_naddr; ++count) {
		size_t wbuf_ofz = count * geo->lba_nbytes;
		uint64_t slba = rng_slba + count * 4;
		struct xnvme_req req = { 0 };

		sranges->entry[count].slba = slba;
		sranges->entry[count].nlb = 0;

		err = xnvme_cmd_write(dev, nsid, slba, 0, wbuf + wbuf_ofz, NULL,
				      XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			xnvmec_perr("xnvme_cmd_write()", err);
			xnvme_req_pr(&req, XNVME_PR_DEF);
			err = err ? err : -EIO;
			goto exit;
		}
	}

	{
		struct xnvme_req req = { 0 };
		uint8_t nr = mdts_naddr - 1;

		xnvmec_pinf("scopy sranges to sdlba: 0x%016x", sdlba);
		lblk_source_range_pr(sranges, nr, XNVME_PR_DEF);

		err = lblk_cmd_scopy(dev, nsid, sdlba, sranges->entry, nr,
				     copy_fmt, XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			xnvmec_perr("xnvme_cmd_scopy()", err);
			xnvme_req_pr(&req, XNVME_PR_DEF);
			err = err ? err : -EIO;
			goto exit;
		}

		xnvmec_pinf("read sdlba: 0x%016x", sdlba);
		memset(rbuf, 0, buf_nbytes);
		err = xnvme_cmd_read(dev, nsid, sdlba, nr, rbuf, NULL,
				     XNVME_CMD_SYNC, &req);
		if (err || xnvme_req_cpl_status(&req)) {
			xnvmec_perr("xnvme_cmd_read()", err);
			xnvme_req_pr(&req, XNVME_PR_DEF);
			err = err ? err : -EIO;
			goto exit;
		}

		xnvmec_pinf("Comparing wbuf and rbuf");
		if (xnvmec_buf_diff(wbuf, rbuf, buf_nbytes)) {
			xnvmec_buf_diff_pr(wbuf, rbuf, buf_nbytes, XNVME_PR_DEF);
			err = -EIO;
			goto exit;
		}
	}

exit:
	xnvme_buf_free(dev, wbuf);
	xnvme_buf_free(dev, rbuf);
	xnvme_buf_free(dev, sranges);

	return err;
}


//
// Command-Line Interface (CLI) definition
//
static struct xnvmec_sub subs[] = {
	{
		"io",
		"Basic Verification of being able to read, what was written",
		"Basic Verification of being able to read, what was written",
		sub_io,
		{
			{XNVMEC_OPT_URI, XNVMEC_POSA},
			{XNVMEC_OPT_SLBA, XNVMEC_LOPT},
			{XNVMEC_OPT_ELBA, XNVMEC_LOPT},
		}
	},
	{
		"scopy",
		"Basic Verification of the Simple-Copy Command",
		"Basic Verification of the Simple-Copy Command",
		test_scopy, {
			{XNVMEC_OPT_URI, XNVMEC_POSA},
			{XNVMEC_OPT_SLBA, XNVMEC_LOPT},
		}
	},
};

static struct xnvmec cli = {
	.title = "Basic LBLK Verification",
	.descr_short = "Basic LBLK Verification",
	.subs = subs,
	.nsubs = sizeof subs / sizeof(*subs),
};

int
main(int argc, char **argv)
{
	return xnvmec(&cli, argc, argv, XNVMEC_INIT_DEV_OPEN);
}
