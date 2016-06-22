/*-
 * Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2016 Broadcom, All Rights Reserved.
 * The term Broadcom refers to Broadcom Limited and/or its subsidiaries
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/endian.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/iflib.h>

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_rss.h"

#include "ifdi_if.h"

#include "bnxt.h"
#include "bnxt_hwrm.h"
#include "hsi_struct_def.h"

/*
 * PCI Device ID Table
 */

static pci_vendor_info_t bnxt_vendor_info_array[] =
{
    PVID(BROADCOM_VENDOR_ID, BCM57301,
	"Broadcom BCM57301 NetXtreme-C Single-port 10Gb Ethernet"),
    PVID(BROADCOM_VENDOR_ID, BCM57302,
	"Broadcom BCM57302 NetXtreme-C Dual-port 10Gb/25Gb Ethernet"),
    PVID(BROADCOM_VENDOR_ID, BCM57304,
	"Broadcom BCM57304 NetXtreme-C Dual-port 10Gb/25Gb/40Gb/50Gb Ethernet"),
    PVID(BROADCOM_VENDOR_ID, BCM57402,
	"Broadcom BCM57402 NetXtreme-E Dual-port 10Gb Ethernet"),
    PVID(BROADCOM_VENDOR_ID, BCM57404,
	"Broadcom BCM57404 NetXtreme-E Dual-port 10Gb/25Gb Ethernet"),
    PVID(BROADCOM_VENDOR_ID, BCM57406,
	"Broadcom BCM57406 NetXtreme-E Dual-port 10GBase-T Ethernet"),

    /* required last entry */
    PVID_END
};

/*
 * Function prototypes
 */

static void *bnxt_register(device_t dev);

/* Soft queue setup and teardown */
static int bnxt_tx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs,
    uint64_t *paddrs, int ntxqs, int ntxqsets);
static int bnxt_rx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs,
    uint64_t *paddrs, int nrxqs, int nrxqsets);
static void bnxt_queues_free(if_ctx_t ctx);

/* Device setup and teardown */
static int bnxt_attach_pre(if_ctx_t ctx);
static int bnxt_attach_post(if_ctx_t ctx);
static int bnxt_detach(if_ctx_t ctx);

/* Device configuration */
static void bnxt_init(if_ctx_t ctx);
static void bnxt_stop(if_ctx_t ctx);
static void bnxt_multi_set(if_ctx_t ctx);
static int bnxt_mtu_set(if_ctx_t ctx, uint32_t mtu);
static void bnxt_media_status(if_ctx_t ctx, struct ifmediareq * ifmr);
static int bnxt_media_change(if_ctx_t ctx);
static int bnxt_promisc_set(if_ctx_t ctx, int flags);
static uint64_t	bnxt_get_counter(if_ctx_t, ift_counter);
static void bnxt_update_admin_status(if_ctx_t ctx);

/* Interrupt enable / disable */
static void bnxt_enable_intr(if_ctx_t ctx);
static void bnxt_queue_intr_enable(if_ctx_t ctx, uint16_t qid);
static void bnxt_disable_intr(if_ctx_t ctx);
static int bnxt_msix_intr_assign(if_ctx_t ctx, int msix);

/* Internal support functions */
static int bnxt_probe_phy(struct bnxt_softc *softc);
static void bnxt_add_media_types(struct bnxt_softc *softc);
static int bnxt_pci_mapping(struct bnxt_softc *softc);
static void bnxt_pci_mapping_free(struct bnxt_softc *softc);
static int bnxt_update_link(struct bnxt_softc *softc, bool chng_link_state);
static int bnxt_handle_def_cp(void *arg);
static int bnxt_handle_rx_cp(void *arg);
static void bnxt_clear_ids(struct bnxt_softc *softc);

/*
 * Device Interface Declaration
 */

static device_method_t bnxt_methods[] = {
	/* Device interface */
	DEVMETHOD(device_register, bnxt_register),
	DEVMETHOD(device_probe, iflib_device_probe),
	DEVMETHOD(device_attach, iflib_device_attach),
	DEVMETHOD(device_detach, iflib_device_detach),
	DEVMETHOD(device_shutdown, iflib_device_shutdown),
	DEVMETHOD(device_suspend, iflib_device_suspend),
	DEVMETHOD(device_resume, iflib_device_resume),
	DEVMETHOD_END
};

static driver_t bnxt_driver = {
	"bnxt", bnxt_methods, sizeof(struct bnxt_softc),
};

devclass_t bnxt_devclass;
DRIVER_MODULE(bnxt, pci, bnxt_driver, bnxt_devclass, 0, 0);

MODULE_DEPEND(bnxt, pci, 1, 1, 1);
MODULE_DEPEND(bnxt, ether, 1, 1, 1);
MODULE_DEPEND(bnxt, iflib, 1, 1, 1);

static device_method_t bnxt_iflib_methods[] = {
	DEVMETHOD(ifdi_tx_queues_alloc, bnxt_tx_queues_alloc),
	DEVMETHOD(ifdi_rx_queues_alloc, bnxt_rx_queues_alloc),
	DEVMETHOD(ifdi_queues_free, bnxt_queues_free),

	DEVMETHOD(ifdi_attach_pre, bnxt_attach_pre),
	DEVMETHOD(ifdi_attach_post, bnxt_attach_post),
	DEVMETHOD(ifdi_detach, bnxt_detach),

	DEVMETHOD(ifdi_init, bnxt_init),
	DEVMETHOD(ifdi_stop, bnxt_stop),
	DEVMETHOD(ifdi_multi_set, bnxt_multi_set),
	DEVMETHOD(ifdi_mtu_set, bnxt_mtu_set),
	DEVMETHOD(ifdi_media_status, bnxt_media_status),
	DEVMETHOD(ifdi_media_change, bnxt_media_change),
	DEVMETHOD(ifdi_promisc_set, bnxt_promisc_set),
	DEVMETHOD(ifdi_get_counter, bnxt_get_counter),
	DEVMETHOD(ifdi_update_admin_status, bnxt_update_admin_status),

	DEVMETHOD(ifdi_intr_enable, bnxt_enable_intr),
	DEVMETHOD(ifdi_queue_intr_enable, bnxt_queue_intr_enable),
	DEVMETHOD(ifdi_intr_disable, bnxt_disable_intr),
	DEVMETHOD(ifdi_msix_intr_assign, bnxt_msix_intr_assign),

	DEVMETHOD_END
};

static driver_t bnxt_iflib_driver = {
	"bnxt", bnxt_iflib_methods, sizeof(struct bnxt_softc)
};

/*
 * iflib shared context
 */

extern struct if_txrx bnxt_txrx;
static struct if_shared_ctx bnxt_sctx_init = {
	.isc_magic = IFLIB_MAGIC,
	.isc_txrx = &bnxt_txrx,
	.isc_driver = &bnxt_iflib_driver,
	.isc_nfl = 2,				// Number of Free Lists
	.isc_flags = 0 /*IFLIB_HAS_CQ*/,
	.isc_q_align = PAGE_SIZE,

	/* We really don't have a maximum here */
#if 0
	// But specifying that messes stuff up.
	.isc_tx_maxsize = UINT32_MAX * sizeof(struct tx_bd_short),
	.isc_tx_maxsegsize = UINT32_MAX * sizeof(struct tx_bd_short),
	.isc_rx_maxsize = UINT32_MAX * sizeof(struct rx_pkt_cmpl),
	.isc_rx_maxsegsize = UINT32_MAX * sizeof(struct rx_pkt_cmpl),
#else
	// TODO: Play around with these...
	.isc_tx_maxsize = BNXT_TSO_SIZE,
	.isc_tx_maxsegsize = BNXT_TSO_SIZE,
	.isc_rx_maxsize = BNXT_TSO_SIZE,
	.isc_rx_maxsegsize = BNXT_TSO_SIZE,
#endif

	// Only use a single segment to avoid page size constraints
	.isc_rx_nsegments = 1,
	.isc_ntxqs = 2,
	.isc_nrxqs = 3,
	.isc_nrxd = PAGE_SIZE / sizeof(struct rx_pkt_cmpl),
	.isc_ntxd = PAGE_SIZE / sizeof(struct tx_bd_short),
	.isc_admin_intrcnt = 1,
	.isc_vendor_info = bnxt_vendor_info_array,
	/*
	 * TODO: Previously completion rings were twice the size of
	 * the other rings.  This is because overrunning the completion
	 * ring hands the hardware, and we can get more completions than
	 * what we produce.  Unfortunately, using the pidx method makes
	 * this a bit tricky, so they're the same size for now.
	 */
	.isc_txqsizes = {PAGE_SIZE * 2, PAGE_SIZE},
	.isc_rxqsizes = {PAGE_SIZE * 2, PAGE_SIZE, PAGE_SIZE},
};

if_shared_ctx_t bnxt_sctx = &bnxt_sctx_init;

/*
 * Device Methods
 */

static void *
bnxt_register(device_t dev)
{
	return bnxt_sctx;
}

/*
 * Device Dependent Configuration Functions
*/

/* Soft queue setup and teardown */
static int
bnxt_tx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs,
    uint64_t *paddrs, int ntxqs, int ntxqsets)
{
	struct bnxt_softc *softc;
	int i;
	int rc;

	softc = iflib_get_softc(ctx);

	softc->tx_cp_rings = malloc(sizeof(struct bnxt_cp_ring) * ntxqsets,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!softc->tx_cp_rings) {
		device_printf(iflib_get_dev(ctx),
		    "unable to allocate TX completion rings");
		rc = ENOMEM;
		goto cp_alloc_fail;
	}
	softc->tx_rings = malloc(sizeof(struct bnxt_tx_ring) * ntxqsets,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!softc->tx_rings) {
		device_printf(iflib_get_dev(ctx),
		    "unable to allocate TX rings");
		rc = ENOMEM;
		goto ring_alloc_fail;
	}
	rc = iflib_dma_alloc(ctx, sizeof(struct ctx_hw_stats) * ntxqsets,
	    &softc->tx_stats, 0);
	if (rc)
		goto dma_alloc_fail;

	for (i = 0; i < ntxqsets; i++) {
		/* Set up the completion ring */
		softc->tx_cp_rings[i].stats_ctx_id = HWRM_NA_SIGNATURE;
		softc->tx_cp_rings[i].ring.phys_id =
		    (uint16_t)HWRM_NA_SIGNATURE;
		softc->tx_cp_rings[i].ring.softc = softc;
		softc->tx_cp_rings[i].ring.id =
		    (softc->scctx->isc_nrxqsets * 2) + 1 + i;
		softc->tx_cp_rings[i].ring.doorbell = softc->tx_cp_rings[i].ring.id * 0x80;
		softc->tx_cp_rings[i].ring.ring_size = softc->sctx->isc_ntxd * 2;
		softc->tx_cp_rings[i].ring.ring_mask =
		    softc->tx_cp_rings[i].ring.ring_size - 1;
		softc->tx_cp_rings[i].ring.vaddr = vaddrs[i * ntxqs];
		softc->tx_cp_rings[i].ring.paddr = paddrs[i * ntxqs];

		/* Set up the TX ring */
		softc->tx_rings[i].ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->tx_rings[i].ring.softc = softc;
		softc->tx_rings[i].ring.id =
		    (softc->scctx->isc_nrxqsets * 2) + 1 + i;
		softc->tx_rings[i].ring.doorbell = softc->tx_rings[i].ring.id * 0x80;
		softc->tx_rings[i].ring.ring_size = softc->sctx->isc_ntxd;
		softc->tx_rings[i].ring.ring_mask =
		    softc->tx_rings[i].ring.ring_size - 1;
		softc->tx_rings[i].prod = 0;
		softc->tx_rings[i].ring.vaddr = vaddrs[i * ntxqs + 1];
		softc->tx_rings[i].ring.paddr = paddrs[i * ntxqs + 1];
	}

	softc->ntxqsets = ntxqsets;
	return rc;

dma_alloc_fail:
	free(softc->tx_rings, M_DEVBUF);
ring_alloc_fail:
	free(softc->tx_cp_rings, M_DEVBUF);
cp_alloc_fail:
	return rc;
}

static void
bnxt_queues_free(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);

	// Free TX queues
	iflib_dma_free(&softc->tx_stats);
	free(softc->tx_rings, M_DEVBUF);
	softc->tx_rings = NULL;
	free(softc->tx_cp_rings, M_DEVBUF);
	softc->tx_cp_rings = NULL;
	softc->ntxqsets = 0;

	// Free RX queues
	iflib_dma_free(&softc->rx_stats);
	free(softc->vnic_info, M_DEVBUF);
	free(softc->grp_info, M_DEVBUF);
	free(softc->ag_rings, M_DEVBUF);
	free(softc->rx_rings, M_DEVBUF);
	free(softc->rx_cp_rings, M_DEVBUF);

	/*
	 * Moved here from _detach() since this is called later and
	 * we need HWRM access
	 */
	bnxt_hwrm_func_drv_unrgtr(softc, false);
	bnxt_free_hwrm_dma_mem(softc);
	BNXT_HWRM_LOCK_DESTROY(softc);
}

static int
bnxt_rx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs,
    uint64_t *paddrs, int nrxqs, int nrxqsets)
{
	struct bnxt_softc *softc;
	int i;
	int rc;

	softc = iflib_get_softc(ctx);

	softc->rx_cp_rings = malloc(sizeof(struct bnxt_cp_ring) * nrxqsets,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!softc->rx_cp_rings) {
		device_printf(iflib_get_dev(ctx),
		    "unable to allocate RX completion rings");
		rc = ENOMEM;
		goto cp_alloc_fail;
	}
	softc->rx_rings = malloc(sizeof(struct bnxt_rx_ring) * nrxqsets,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!softc->rx_rings) {
		device_printf(iflib_get_dev(ctx),
		    "unable to allocate RX rings");
		rc = ENOMEM;
		goto ring_alloc_fail;
	}
	softc->ag_rings = malloc(sizeof(struct bnxt_rx_ring) * nrxqsets,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!softc->ag_rings) {
		device_printf(iflib_get_dev(ctx),
		    "unable to allocate aggregation rings");
		rc = ENOMEM;
		goto ag_alloc_fail;
	}
	softc->grp_info = malloc(sizeof(struct bnxt_grp_info) * nrxqsets,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!softc->grp_info) {
		device_printf(iflib_get_dev(ctx),
		    "unable to allocate ring groups");
		rc = ENOMEM;
		goto grp_alloc_fail;
	}
	softc->vnic_info = malloc(sizeof(struct bnxt_vnic_info) * nrxqsets,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (!softc->vnic_info) {
		device_printf(iflib_get_dev(ctx),
		    "unable to allocate ring groups");
		rc = ENOMEM;
		goto vnic_alloc_fail;
	}

	rc = iflib_dma_alloc(ctx, sizeof(struct ctx_hw_stats) * nrxqsets,
	    &softc->rx_stats, 0);
	if (rc)
		goto dma_alloc_fail;

	for (i = 0; i < nrxqsets; i++) {
		/* Allocation the completion ring */
		softc->rx_cp_rings[i].stats_ctx_id = HWRM_NA_SIGNATURE;
		softc->rx_cp_rings[i].ring.phys_id =
		    (uint16_t)HWRM_NA_SIGNATURE;
		softc->rx_cp_rings[i].ring.softc = softc;
		softc->rx_cp_rings[i].ring.id = i + 1;
		softc->rx_cp_rings[i].ring.doorbell = softc->rx_cp_rings[i].ring.id * 0x80;
		softc->rx_cp_rings[i].ring.ring_size = softc->sctx->isc_nrxd * 2;
		softc->rx_cp_rings[i].ring.ring_mask =
		    softc->rx_cp_rings[i].ring.ring_size - 1;
		softc->rx_cp_rings[i].ring.vaddr = vaddrs[i * nrxqs];
		softc->rx_cp_rings[i].ring.paddr = paddrs[i * nrxqs];

		/* Allocate the RX ring */
		softc->rx_rings[i].ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->rx_rings[i].ring.softc = softc;
		softc->rx_rings[i].ring.id = i + 1;
		softc->rx_rings[i].ring.doorbell = softc->rx_rings[i].ring.id * 0x80;
		softc->rx_rings[i].ring.ring_size = softc->sctx->isc_nrxd;
		softc->rx_rings[i].ring.ring_mask =
		    softc->rx_rings[i].ring.ring_size - 1;
		softc->rx_rings[i].prod = 0;
		softc->rx_rings[i].ring.vaddr = vaddrs[i * nrxqs + 1];
		softc->rx_rings[i].ring.paddr = paddrs[i * nrxqs + 1];

		/* Allocate the AG ring */
		softc->ag_rings[i].ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->ag_rings[i].ring.softc = softc;
		softc->ag_rings[i].ring.id = nrxqsets + i + 1;
		softc->ag_rings[i].ring.doorbell = softc->ag_rings[i].ring.id * 0x80;
		softc->ag_rings[i].ring.ring_size = softc->sctx->isc_nrxd;
		softc->ag_rings[i].ring.ring_mask =
		    softc->ag_rings[i].ring.ring_size - 1;
		softc->ag_rings[i].prod = 0;
		softc->ag_rings[i].ring.vaddr = vaddrs[i * nrxqs + 2];
		softc->ag_rings[i].ring.paddr = paddrs[i * nrxqs + 2];

		/* Allocate the ring group */
		softc->grp_info[i].grp_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->grp_info[i].stats_ctx =
		    softc->rx_cp_rings[i].stats_ctx_id;
		softc->grp_info[i].rx_ring_id = softc->rx_rings[i].ring.phys_id;
		softc->grp_info[i].ag_ring_id = softc->ag_rings[i].ring.phys_id;
		softc->grp_info[i].cp_ring_id = softc->rx_cp_rings[i].ring.phys_id;

		/* And finally, the VNIC */
		softc->vnic_info[i].id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].flow_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].filter_id = -1;
		softc->vnic_info[i].ring_grp = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].rss_rule = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].cos_rule = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].lb_rule = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].ctx_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].rx_mask =
		    HWRM_CFA_L2_SET_RX_MASK_INPUT_MASK_BCAST;
		softc->vnic_info[i].mc_list_count = 0;
		softc->vnic_info[i].mc_list_size =
		    BNXT_MAX_MC_ADDRS * ETHER_ADDR_LEN;
		if (i == 0)
			softc->vnic_info[i].flags = BNXT_VNIC_FLAG_DEFAULT;
		rc = iflib_dma_alloc(ctx, softc->vnic_info[i].mc_list_size,
		    &softc->vnic_info[i].mc_list, 0);
		if (rc) {
			i--;
			goto mc_alloc_fail;
		}
	}

	softc->nrxqsets = nrxqsets;
	return rc;

mc_alloc_fail:
	for (; i >= 0; i--)
		iflib_dma_free(&softc->vnic_info[i].mc_list);
dma_alloc_fail:
	free(softc->vnic_info, M_DEVBUF);
vnic_alloc_fail:
	free(softc->grp_info, M_DEVBUF);
grp_alloc_fail:
	free(softc->ag_rings, M_DEVBUF);
ag_alloc_fail:
	free(softc->rx_rings, M_DEVBUF);
ring_alloc_fail:
	free(softc->rx_cp_rings, M_DEVBUF);
cp_alloc_fail:
	return rc;
}

/* Device setup and teardown */
static int
bnxt_attach_pre(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	if_softc_ctx_t scctx;
	int rc = 0;

	softc->ctx = ctx;
	softc->dev = iflib_get_dev(ctx);
	softc->media = iflib_get_media(ctx);
	softc->scctx = iflib_get_softc_ctx(ctx);
	softc->sctx = iflib_get_sctx(ctx);
	scctx = softc->scctx;

	pci_enable_busmaster(softc->dev);

	if (bnxt_pci_mapping(softc)) 
                return (ENXIO);

	/* HWRM setup/init */
	BNXT_HWRM_LOCK_INIT(softc, device_get_nameunit(softc->dev));
	rc = bnxt_alloc_hwrm_dma_mem(softc);
	if (rc)
		goto dma_fail;

	/* Get firmware version and compare with driver */
	rc = bnxt_hwrm_ver_get(softc);
	if (rc) {
		printf("attach: hwrm ver get failed\n");
		goto ver_fail;
	}

	/* Register the driver with the FW */
	rc = bnxt_hwrm_func_drv_rgtr(softc);
	if (rc) {
		printf("attach: hwrm drv rgtr failed\n");
		goto ver_fail;
	}

	/* Get the HW capabilities */
	rc = bnxt_hwrm_func_qcaps(softc);
	if (rc)
		goto failed;
	iflib_set_mac(ctx, softc->pf.mac_addr);

	/* Get the queue config */
	rc = bnxt_hwrm_queue_qportcfg(softc);
	if (rc) {
		printf("attach: hwrm qportcfg failed\n");
		goto failed;
	}

	/* Now perform a function reset */
	rc = bnxt_hwrm_func_reset(softc);
	bnxt_clear_ids(softc);
	if (rc)
		goto failed;

	/* Now set up iflib sc */
	scctx->isc_tx_nsegments = 32,
	scctx->isc_tx_tso_segments_max = 32;
	scctx->isc_tx_tso_size_max = BNXT_TSO_SIZE;
	scctx->isc_tx_tso_segsize_max = BNXT_TSO_SIZE;
	scctx->isc_vectors = softc->pf.max_cp_rings;

	/* iflib will map and release this bar */
	scctx->isc_msix_bar = pci_msix_table_bar(softc->dev);

	/* Allocate the default completion ring */
	softc->def_cp_ring.stats_ctx_id = HWRM_NA_SIGNATURE;
	softc->def_cp_ring.ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
	softc->def_cp_ring.ring.softc = softc;
	softc->def_cp_ring.ring.id = 0;
	softc->def_cp_ring.ring.doorbell = softc->def_cp_ring.ring.id * 0x80;
	softc->def_cp_ring.ring.ring_size = PAGE_SIZE / sizeof(struct cmpl_base);
	softc->def_cp_ring.ring.ring_mask = softc->def_cp_ring.ring.ring_size - 1;
	rc = iflib_dma_alloc(ctx, sizeof(struct cmpl_base) * softc->def_cp_ring.ring.ring_size,
	    &softc->def_cp_ring_mem, 0);
	softc->def_cp_ring.ring.vaddr = softc->def_cp_ring_mem.idi_vaddr;
	softc->def_cp_ring.ring.paddr = softc->def_cp_ring_mem.idi_paddr;

	return (rc);

failed:
	bnxt_hwrm_func_drv_unrgtr(softc, false);
ver_fail:
	bnxt_free_hwrm_dma_mem(softc);
	BNXT_HWRM_LOCK_DESTROY(softc);
dma_fail:
	bnxt_pci_mapping_free(softc);
	pci_disable_busmaster(softc->dev);
	return (rc);
}

static int
bnxt_attach_post(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	if_t ifp = iflib_get_ifp(ctx);
	int capabilities, enabling;
	int rc;

	/* Update link state etc... */
	rc = bnxt_probe_phy(softc);
	if (rc)
		goto failed;

	/* Needs to be done after probing the phy */
	bnxt_add_media_types(softc);
	ifmedia_set(softc->media, IFM_ETHER | IFM_AUTO);

	if_sethwassist(ifp, (CSUM_TCP | CSUM_UDP | CSUM_TCP_IPV6 | CSUM_UDP_IPV6 | CSUM_TSO));

	capabilities =
	    IFCAP_VLAN_MTU | IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_HWTSO |
	    IFCAP_VLAN_HWFILTER | IFCAP_VLAN_HWCSUM | IFCAP_HWCSUM |
	    IFCAP_RXCSUM_IPV6 | IFCAP_TXCSUM_IPV6 |
	    IFCAP_JUMBO_MTU | IFCAP_LRO | IFCAP_TSO4 | IFCAP_TSO6;

	if_setcapabilities(ifp, capabilities);

	/* Don't enable TSO by default until its fixed */
	enabling =
	    IFCAP_VLAN_MTU | IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_HWTSO |
	    IFCAP_VLAN_HWFILTER | IFCAP_VLAN_HWCSUM | IFCAP_HWCSUM |
	    IFCAP_HWCSUM_IPV6 | IFCAP_JUMBO_MTU | IFCAP_LRO;

	if_setcapenable(ifp, enabling);

	// JFV - This needs to be set according to the adapter
	if_setbaudrate(ifp, IF_Gbps(25));

	softc->scctx->isc_max_frame_size = ifp->if_mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;

failed:
	return rc;
}

static int
bnxt_detach(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	int i;

	bnxt_hwrm_func_reset(softc);
	bnxt_clear_ids(softc);
	iflib_irq_free(ctx, &softc->def_cp_ring.irq);
	/* We need to free() these here... */
	for (i = softc->nrxqsets-1; i>=0; i--) {
		iflib_irq_free(ctx, &softc->rx_cp_rings[i].irq);
		iflib_dma_free(&softc->vnic_info[i].mc_list);
	}
	iflib_dma_free(&softc->def_cp_ring_mem);

	/* hwrm is cleaned up in queues_free() since it's called later */
	pci_disable_busmaster(softc->dev);
	bnxt_pci_mapping_free(softc);

	return 0;
}

/* Device configuration */
static void
bnxt_init(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	struct ifnet *ifp = iflib_get_ifp(ctx);
	int i;
	int rc;

	rc = bnxt_hwrm_func_reset(softc);
	if (rc)
		return;
	bnxt_clear_ids(softc);

	/* Allocate the default completion ring */
	softc->def_cp_ring.raw_cons = -1;
	rc = bnxt_hwrm_ring_alloc(softc,
	    HWRM_RING_ALLOC_INPUT_RING_TYPE_CMPL,
	    &softc->def_cp_ring.ring,
	    (uint16_t)HWRM_NA_SIGNATURE,
	    HWRM_NA_SIGNATURE, true);
	if (rc)
		goto fail;

	for (i = 0; i < softc->nrxqsets; i++) {
		/* Allocate the statistics context */
		rc = bnxt_hwrm_stat_ctx_alloc(softc, &softc->rx_cp_rings[i],
		    softc->rx_stats.idi_paddr +
		    (sizeof(struct ctx_hw_stats) * i));
		if (rc)
			goto fail;

		/* Allocation the completion ring */
		softc->rx_cp_rings[i].raw_cons = -1;
		rc = bnxt_hwrm_ring_alloc(softc,
		    HWRM_RING_ALLOC_INPUT_RING_TYPE_CMPL,
		    &softc->rx_cp_rings[i].ring,
		    (uint16_t)HWRM_NA_SIGNATURE,
		    HWRM_NA_SIGNATURE, true);
		if (rc)
			goto fail;

		/* Allocate the RX ring */
		rc = bnxt_hwrm_ring_alloc(softc,
		    HWRM_RING_ALLOC_INPUT_RING_TYPE_RX,
		    &softc->rx_rings[i].ring,
		    (uint16_t)HWRM_NA_SIGNATURE,
		    HWRM_NA_SIGNATURE, false);
		if (rc)
			goto fail;

		/* Allocate the AG ring */
		rc = bnxt_hwrm_ring_alloc(softc,
		    HWRM_RING_ALLOC_INPUT_RING_TYPE_RX,
		    &softc->ag_rings[i].ring,
		    (uint16_t)HWRM_NA_SIGNATURE,
		    HWRM_NA_SIGNATURE, false);
		if (rc)
			goto fail;

		/* Allocate the ring group */
		softc->grp_info[i].stats_ctx =
		    softc->rx_cp_rings[i].stats_ctx_id;
		softc->grp_info[i].rx_ring_id = softc->rx_rings[i].ring.phys_id;
		softc->grp_info[i].ag_ring_id = softc->ag_rings[i].ring.phys_id;
		softc->grp_info[i].cp_ring_id = softc->rx_cp_rings[i].ring.phys_id;
		rc = bnxt_hwrm_ring_grp_alloc(softc, &softc->grp_info[i]);
		if (rc)
			goto fail;

		/* Allocate the vnic */
		softc->vnic_info[i].ring_grp = softc->grp_info[i].grp_id;
		softc->vnic_info[i].mru = softc->scctx->isc_max_frame_size;
		rc = bnxt_hwrm_vnic_alloc(softc, &softc->vnic_info[i]);
		if (rc)
			goto fail;
		rc = bnxt_hwrm_vnic_ctx_alloc(softc, &softc->vnic_info[i]);
		if (rc)
			goto fail;
		rc = bnxt_hwrm_vnic_cfg(softc, &softc->vnic_info[i]);
		if (rc)
			goto fail;
		if (i == 0) {
			rc = bnxt_hwrm_set_filter(softc, &softc->vnic_info[i]);
			if (rc)
				goto fail;
		}
	}

	for (i = 0; i < softc->ntxqsets; i++) {
		/* Allocate the statistics context */
		rc = bnxt_hwrm_stat_ctx_alloc(softc, &softc->tx_cp_rings[i],
		    softc->tx_stats.idi_paddr +
		    (sizeof(struct ctx_hw_stats) * i));
		if (rc) {
			i--;
			goto fail;
		}

		/* Allocate the completion ring */
		softc->tx_cp_rings[i].raw_cons = -1;
		rc = bnxt_hwrm_ring_alloc(softc,
		    HWRM_RING_ALLOC_INPUT_RING_TYPE_CMPL,
		    &softc->tx_cp_rings[i].ring,
		    (uint16_t)HWRM_NA_SIGNATURE,
		    HWRM_NA_SIGNATURE, false);
		if (rc)
			goto fail;

		/* Allocate the TX ring */
		rc = bnxt_hwrm_ring_alloc(softc,
		    HWRM_RING_ALLOC_INPUT_RING_TYPE_TX,
		    &softc->tx_rings[i].ring,
		    softc->tx_cp_rings[i].ring.phys_id,
		    softc->tx_cp_rings[i].stats_ctx_id, false);
		if (rc)
			goto fail;
	}

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	return;

fail:
	bnxt_hwrm_func_reset(softc);
	bnxt_clear_ids(softc);
	ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
	return;
}

static void
bnxt_stop(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);

	bnxt_hwrm_func_reset(softc);
	bnxt_clear_ids(softc);
	if_setdrvflagbits(iflib_get_ifp(ctx), 0, IFF_DRV_RUNNING);
	return;
}

static void
bnxt_multi_set(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	struct bnxt_vnic_info *vnic = &softc->vnic_info[0];
	if_t ifp = iflib_get_ifp(ctx);
	uint8_t *mta;
	int cnt, mcnt;

	mcnt = if_multiaddr_count(ifp, -1);

	if (mcnt > BNXT_MAX_MC_ADDRS) {
		vnic->rx_mask |= HWRM_CFA_L2_SET_RX_MASK_INPUT_MASK_ALL_MCAST;
		bnxt_hwrm_cfa_l2_set_rx_mask(softc, vnic);
	}
	else {
		vnic->rx_mask &= ~HWRM_CFA_L2_SET_RX_MASK_INPUT_MASK_ALL_MCAST;
		mta = vnic->mc_list.idi_vaddr;
		bzero(mta, vnic->mc_list_size);
		if_multiaddr_array(ifp, mta, &cnt, mcnt);
		vnic->mc_list_count = cnt;
		vnic->rx_mask |=
		    HWRM_CFA_L2_SET_RX_MASK_INPUT_MASK_MCAST;
		if (bnxt_hwrm_cfa_l2_set_rx_mask(softc, vnic))
			device_printf(softc->dev,
			    "set_multi: rx_mask set failed\n");
	}
}

static int
bnxt_mtu_set(if_ctx_t ctx, uint32_t mtu)
{
	device_printf(iflib_get_dev(ctx), "STUB: %s @ %s:%d\n", __func__, __FILE__, __LINE__);
	return ENOSYS;
}

static void
bnxt_media_status(if_ctx_t ctx, struct ifmediareq * ifmr)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	struct bnxt_link_info *link_info = &softc->link_info;

	bnxt_update_link(softc, true);

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;

        if (link_info->link_up)
		ifmr->ifm_status |= IFM_ACTIVE;
	else
		ifmr->ifm_status &= ~IFM_ACTIVE;

	if (link_info->duplex == BNXT_LINK_DUPLEX_FULL)
		ifmr->ifm_active |= IFM_FDX;
	else
		ifmr->ifm_active |= IFM_HDX;

	switch (link_info->link_speed) {
        case BNXT_LINK_SPEED_100MB:
		ifmr->ifm_active |= IFM_100_TX;
                break;
        case BNXT_LINK_SPEED_1GB:
		ifmr->ifm_active |= IFM_1000_T;
                break;
        case BNXT_LINK_SPEED_2_5GB:
		ifmr->ifm_active |= IFM_2500_SX;
                break;
        case BNXT_LINK_SPEED_10GB:
		ifmr->ifm_active |= IFM_10G_T;
                break;
        case BNXT_LINK_SPEED_20GB:
		ifmr->ifm_active |= IFM_20G_KR2;
                break;
        case BNXT_LINK_SPEED_25GB:
		ifmr->ifm_active |= IFM_25G_CR;
                break;
        case BNXT_LINK_SPEED_40GB:
		ifmr->ifm_active |= IFM_40G_CR4;
                break;
        case BNXT_LINK_SPEED_50GB:
		ifmr->ifm_active |= IFM_50G_CR2;
                break;
        default:
		return;
	}

	if (link_info->pause == BNXT_LINK_PAUSE_BOTH) 
		ifmr->ifm_active |= (IFM_ETH_RXPAUSE | IFM_ETH_TXPAUSE);
	else if (link_info->pause == BNXT_LINK_PAUSE_TX)
		ifmr->ifm_active |= IFM_ETH_TXPAUSE;
	else if (link_info->pause == BNXT_LINK_PAUSE_RX)
		ifmr->ifm_active |= IFM_ETH_RXPAUSE;

	return;
}

static int
bnxt_media_change(if_ctx_t ctx)
{
	device_printf(iflib_get_dev(ctx), "STUB: %s @ %s:%d\n", __func__, __FILE__, __LINE__);
	return ENOSYS;
}

static int
bnxt_promisc_set(if_ctx_t ctx, int flags)
{
	device_printf(iflib_get_dev(ctx), "STUB: %s @ %s:%d\n", __func__, __FILE__, __LINE__);
	return ENOSYS;
}

static uint64_t
bnxt_get_counter(if_ctx_t ctx, ift_counter cnt)
{
	if_t ifp = iflib_get_ifp(ctx);

	if (cnt < IFCOUNTERS)
		return if_get_counter_default(ifp, cnt);

	device_printf(iflib_get_dev(ctx), "STUB: %s(ctx, %d >= %d) @ %s:%d\n", __func__, cnt, IFCOUNTERS, __FILE__, __LINE__);
	return 0;
}

static void
bnxt_update_admin_status(if_ctx_t ctx)
{
	/* TODO: do we need to do anything here? */
	return;
}

/* Interrupt enable / disable */
static void
bnxt_enable_intr(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	int i;

	BNXT_CP_ARM_DB(&softc->def_cp_ring.ring, softc->def_cp_ring.raw_cons);
	for (i = 0; i < softc->nrxqsets; i++)
		BNXT_CP_ARM_DB(&softc->rx_cp_rings[i].ring, softc->rx_cp_rings[i].raw_cons);
	return;
}

static void
bnxt_queue_intr_enable(if_ctx_t ctx, uint16_t qid)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);

	BNXT_CP_ARM_DB(&softc->rx_cp_rings[qid].ring,
	    softc->rx_cp_rings[qid].raw_cons);
	return;
}

static void
bnxt_disable_intr(if_ctx_t ctx)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	int i;

	BNXT_CP_DISABLE_DB(&softc->def_cp_ring.ring, softc->def_cp_ring.raw_cons);
	for (i = 0; i < softc->ntxqsets; i++)
		BNXT_CP_DISABLE_DB(&softc->tx_cp_rings[i].ring,
		    softc->tx_cp_rings[i].raw_cons);
	for (i = 0; i < softc->nrxqsets; i++)
		BNXT_CP_DISABLE_DB(&softc->rx_cp_rings[i].ring,
		    softc->rx_cp_rings[i].raw_cons);

	return;
}

static int
bnxt_msix_intr_assign(if_ctx_t ctx, int msix)
{
	struct bnxt_softc *softc = iflib_get_softc(ctx);
	int rc;
	int i;

	rc = iflib_irq_alloc_generic(ctx, &softc->def_cp_ring.irq, softc->def_cp_ring.ring.id + 1,
	    IFLIB_INTR_ADMIN, bnxt_handle_def_cp, softc, 0, "def_cp");
	if (rc) {
		device_printf(iflib_get_dev(ctx), "Failed to register default completion ring handler\n");
		return rc;
	}

	for (i=0; i<softc->scctx->isc_nrxqsets; i++) {
		rc = iflib_irq_alloc_generic(ctx, &softc->rx_cp_rings[i].irq,
		    softc->rx_cp_rings[i].ring.id + 1, IFLIB_INTR_RX, bnxt_handle_rx_cp,
		    &softc->rx_cp_rings[i], i, "rx_cp");
		if (rc) {
			device_printf(iflib_get_dev(ctx),
			    "Failed to register RX completion ring handler\n");
			i--;
			goto fail;
		}
	}

	for (i=0; i<softc->scctx->isc_ntxqsets; i++)
		iflib_softirq_alloc_generic(ctx, i + 1, IFLIB_INTR_TX, NULL, i, "tx_cp");

	return rc;

fail:
	for (; i>=0; i--)
		iflib_irq_free(ctx, &softc->rx_cp_rings[i].irq);
	iflib_irq_free(ctx, &softc->def_cp_ring.irq);
	return rc;
}

/*
 * Support functions
 */
static int
bnxt_probe_phy(struct bnxt_softc *softc)
{
	struct bnxt_link_info *link_info = &softc->link_info;
	char phy_ver[PHY_VER_STR_LEN];
	int rc = 0;

	rc = bnxt_update_link(softc, false);
	if (rc) {
		device_printf(softc->dev,
		    "Probe phy can't update link (rc: %x)\n", rc);
		return (rc);
	}

	/*initialize the ethool setting copy with NVM settings */
	if (BNXT_AUTO_MODE(link_info->auto_mode))
		link_info->autoneg |= BNXT_AUTONEG_SPEED;

	if (link_info->auto_pause & BNXT_LINK_PAUSE_BOTH) {
		if (link_info->auto_pause == BNXT_LINK_PAUSE_BOTH)
			link_info->autoneg |= BNXT_AUTONEG_FLOW_CTRL;
		link_info->req_flow_ctrl = link_info->auto_pause;
	} else if (link_info->force_pause & BNXT_LINK_PAUSE_BOTH) {
		link_info->req_flow_ctrl = link_info->force_pause;
	}
	link_info->req_duplex = link_info->duplex_setting;
	if (link_info->autoneg & BNXT_AUTONEG_SPEED)
		link_info->req_link_speed = link_info->auto_link_speed;
	else
		link_info->req_link_speed = link_info->force_link_speed;
	link_info->advertising = link_info->auto_link_speeds;
	snprintf(phy_ver, PHY_VER_STR_LEN, " ph %d.%d.%d",
		 link_info->phy_ver[0],
		 link_info->phy_ver[1],
		 link_info->phy_ver[2]);
	strcat(softc->fw_ver_str, phy_ver);
	return (rc);
}

static void
bnxt_add_media_types(struct bnxt_softc *softc)
{
	struct bnxt_link_info *link_info = &softc->link_info;
	uint16_t	supported;

	supported = link_info->support_speeds;

	if (supported &  BNXT_LINK_SPEED_MSK_100MB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_100_TX, 0, NULL);
	if (supported &  BNXT_LINK_SPEED_MSK_1GB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_1000_T, 0, NULL);
	if (supported &  BNXT_LINK_SPEED_MSK_2_5GB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_2500_SX, 0, NULL);
	if (supported &  BNXT_LINK_SPEED_MSK_10GB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_10G_T, 0, NULL);
	if (supported &  BNXT_LINK_SPEED_MSK_20GB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_20G_KR2, 0, NULL);
	if (supported &  BNXT_LINK_SPEED_MSK_25GB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_25G_CR, 0, NULL);
	if (supported &  BNXT_LINK_SPEED_MSK_40GB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_40G_CR4, 0, NULL);
	if (supported &  BNXT_LINK_SPEED_MSK_50GB)
		ifmedia_add(softc->media, IFM_ETHER | IFM_50G_CR2, 0, NULL);

	ifmedia_add(softc->media, IFM_ETHER | IFM_AUTO, 0, NULL);

	return;
}

static int
bnxt_map_bar(struct bnxt_softc *softc, struct bnxt_bar_info *bar, int bar_num, bool shareable)
{
	uint32_t	flag;

	if (bar->res != NULL) {
		device_printf(softc->dev, "Bar %d already mapped\n", bar_num);
		return EDOOFUS;
	}

	bar->rid = PCIR_BAR(bar_num);
	flag = RF_ACTIVE;
	if (shareable)
		flag |= RF_SHAREABLE;

	if ((bar->res =
		bus_alloc_resource_any(softc->dev,
			   SYS_RES_MEMORY,
			   &bar->rid,
			   flag)) == NULL) {
		device_printf(softc->dev,
		    "PCI BAR%d mapping failure\n", bar_num);
		return (ENXIO);
	}
	bar->tag = rman_get_bustag(bar->res);
	bar->handle = rman_get_bushandle(bar->res);
	bar->size = rman_get_size(bar->res);

	return 0;
}

static int
bnxt_pci_mapping(struct bnxt_softc *softc)
{
	int rc;

	rc = bnxt_map_bar(softc, &softc->hwrm_bar, 0, true);
	if (rc)
		return rc;

	rc = bnxt_map_bar(softc, &softc->doorbell_bar, 2, false);

	return rc;
}

static void
bnxt_pci_mapping_free(struct bnxt_softc *softc)
{
	if (softc->hwrm_bar.res != NULL)
		bus_release_resource(softc->dev, SYS_RES_MEMORY,
		    softc->hwrm_bar.rid, softc->hwrm_bar.res);
	softc->hwrm_bar.res = NULL;

	if (softc->doorbell_bar.res != NULL)
		bus_release_resource(softc->dev, SYS_RES_MEMORY,
		    softc->doorbell_bar.rid, softc->doorbell_bar.res);
	softc->doorbell_bar.res = NULL;
}

static int
bnxt_update_link(struct bnxt_softc *softc, bool chng_link_state)
{
	struct bnxt_link_info *link_info = &softc->link_info;
	struct hwrm_port_phy_qcfg_input req = {0};
	struct hwrm_port_phy_qcfg_output *resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	uint8_t link_up = link_info->link_up;
	int rc = 0;

	BNXT_HWRM_LOCK(softc);
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_PORT_PHY_QCFG, -1, -1);

	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc) {
		BNXT_HWRM_UNLOCK(softc);
		return rc;
	}

	memcpy(&link_info->phy_qcfg_resp, resp, sizeof(*resp));
	link_info->phy_link_status = resp->link;
	link_info->duplex =  resp->duplex;
	link_info->pause = resp->pause;
	link_info->auto_mode = resp->auto_mode;
	link_info->auto_pause = resp->auto_pause;
	link_info->force_pause = resp->force_pause;
	link_info->duplex_setting = resp->duplex;
	if (link_info->phy_link_status == BNXT_LINK_LINK)
		link_info->link_speed = le16toh(resp->link_speed);
	else
		link_info->link_speed = 0;

	link_info->force_link_speed = le16toh(resp->force_link_speed);
	link_info->auto_link_speed = le16toh(resp->auto_link_speed);
	link_info->support_speeds = le16toh(resp->support_speeds);
	link_info->auto_link_speeds = le16toh(resp->auto_link_speed_mask);
	link_info->preemphasis = le32toh(resp->preemphasis);
	link_info->phy_ver[0] = resp->phy_maj;
	link_info->phy_ver[1] = resp->phy_min;
	link_info->phy_ver[2] = resp->phy_bld;
	link_info->media_type = resp->media_type;
	link_info->transceiver = resp->xcvr_pkg_type;
	link_info->phy_addr = resp->eee_config_phy_addr &
			      HWRM_PORT_PHY_QCFG_OUTPUT_PHY_ADDR_MASK;

	/* TODO: need to add more logic to report VF link */
	if (chng_link_state) {
		if (link_info->phy_link_status == BNXT_LINK_LINK)
			link_info->link_up = 1;
		else
			link_info->link_up = 0;
		if (link_up != link_info->link_up)
			bnxt_report_link(softc);
	} else {
		/* alwasy link down if not require to update link state */
		link_info->link_up = 0;
	}
	BNXT_HWRM_UNLOCK(softc);
	return (0);
}

void
bnxt_report_link(struct bnxt_softc *softc)
{
	const char *duplex = NULL, *flow_ctrl = NULL;

        if (softc->link_info.link_up) {
                if (softc->link_info.duplex == BNXT_LINK_DUPLEX_FULL)
                        duplex = "full duplex";
                else
                        duplex = "half duplex";
                if (softc->link_info.pause == BNXT_LINK_PAUSE_BOTH)
                        flow_ctrl = "FC - receive & transmit";
                else if (softc->link_info.pause == BNXT_LINK_PAUSE_TX)
                        flow_ctrl = "FC - transmit";
                else if (softc->link_info.pause == BNXT_LINK_PAUSE_RX)
                        flow_ctrl = "FC - receive";
                else
                        flow_ctrl = "none";
		iflib_link_state_change(softc->ctx, LINK_STATE_UP);
                device_printf(softc->dev, "Link is UP %s, %s\n", duplex,
		    flow_ctrl);
        } else {
		iflib_link_state_change(softc->ctx, LINK_STATE_DOWN);
                device_printf(softc->dev, "Link is Down %s, %s\n", duplex,
		    flow_ctrl);
        }
}

static int
bnxt_handle_rx_cp(void *arg)
{
	struct bnxt_cp_ring *cpr = arg;

	/* Disable further interrupts */
	BNXT_CP_DISABLE_DB(&cpr->ring,
		    cpr->raw_cons);
	return FILTER_SCHEDULE_THREAD;
}

static int
bnxt_handle_def_cp(void *arg)
{
	struct bnxt_softc *softc = arg;

	device_printf(softc->dev, "STUB: %s @ %s:%d\n", __func__, __FILE__, __LINE__);
	return FILTER_HANDLED;
	return FILTER_SCHEDULE_THREAD;
}

static void
bnxt_clear_ids(struct bnxt_softc *softc)
{
	int i;

	softc->def_cp_ring.stats_ctx_id = HWRM_NA_SIGNATURE;
	softc->def_cp_ring.ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
	for (i = 0; i < softc->ntxqsets; i++) {
		softc->tx_cp_rings[i].stats_ctx_id = HWRM_NA_SIGNATURE;
		softc->tx_cp_rings[i].ring.phys_id =
		    (uint16_t)HWRM_NA_SIGNATURE;
		softc->tx_rings[i].ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
	}
	for (i = 0; i < softc->nrxqsets; i++) {
		softc->rx_cp_rings[i].stats_ctx_id = HWRM_NA_SIGNATURE;
		softc->rx_cp_rings[i].ring.phys_id =
		    (uint16_t)HWRM_NA_SIGNATURE;
		softc->rx_rings[i].ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->ag_rings[i].ring.phys_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->grp_info[i].grp_id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].id = (uint16_t)HWRM_NA_SIGNATURE;
		softc->vnic_info[i].ctx_id = (uint16_t)HWRM_NA_SIGNATURE;
	}
}
