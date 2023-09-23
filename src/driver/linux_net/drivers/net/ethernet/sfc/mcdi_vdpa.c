// SPDX-License-Identifier: GPL-2.0
/* Driver for Xilinx network controllers and boards
 * Copyright 2020 Xilinx Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#include <linux/vdpa.h>
#include "ef100_vdpa.h"
#include "efx.h"
#include "nic.h"
#include "mcdi_vdpa.h"
#include "mcdi_pcol.h"
#include "ef100_rep.h"

#if defined(CONFIG_SFC_VDPA)

#ifdef VDPA_TEST
#include <uapi/linux/virtio_config.h>
static u64 ef100vdpa_features = (1ULL << VIRTIO_F_ANY_LAYOUT) |
			      (1ULL << VIRTIO_F_VERSION_1) |
#if defined(EFX_USE_KCOMPAT) && defined(EFX_HAVE_VIRTIO_F_IN_ORDER)
			      (1ULL << VIRTIO_F_IN_ORDER) |
#endif
			      (1ULL << VIRTIO_NET_F_MAC) |
			      (1ULL << VIRTIO_NET_F_MTU) |
			      (1ULL << VIRTIO_NET_F_SPEED_DUPLEX);
#endif

/* value of target_vf in case of MCDI invocations on a VF
 * for virtqueue create, delete and doorbell offset
 */
#define MCDI_TARGET_VF_VAL_VF_NULL 0xFFFF

struct efx_vring_ctx *efx_vdpa_vring_init(struct efx_nic *efx,  u32 vi,
					  enum ef100_vdpa_vq_type vring_type)
{
	struct efx_vring_ctx *vring_ctx;
	u32 queue_cmd;

	vring_ctx = kzalloc(sizeof(*vring_ctx), GFP_KERNEL);
	if (!vring_ctx)
		return ERR_PTR(-ENOMEM);
	if (vring_type == EF100_VDPA_VQ_TYPE_NET_RXQ) {
		queue_cmd = MC_CMD_VIRTIO_INIT_QUEUE_REQ_NET_RXQ;
	} else if (vring_type == EF100_VDPA_VQ_TYPE_NET_TXQ) {
		queue_cmd = MC_CMD_VIRTIO_INIT_QUEUE_REQ_NET_TXQ;
	} else {
		pr_err("%s: Invalid Queue type %u\n", __func__, vring_type);
		kfree(vring_ctx);
		return ERR_PTR(-ENOMEM);
	}

	vring_ctx->nic = efx;
	vring_ctx->vf_index = MCDI_TARGET_VF_VAL_VF_NULL;
	vring_ctx->vi_index = vi;
	vring_ctx->mcdi_vring_type = queue_cmd;
	return vring_ctx;
}

void efx_vdpa_vring_fini(struct efx_vring_ctx *vring_ctx)
{
	kfree(vring_ctx);
}

int efx_vdpa_get_features(struct efx_nic *efx,
			  enum ef100_vdpa_device_type type,
			  u64 *featuresp)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VIRTIO_GET_FEATURES_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_VIRTIO_GET_FEATURES_OUT_LEN);
	u32 high_val = 0, low_val = 0;
	ssize_t outlen;
	int rc;

	if (!efx) {
		pr_err("%s: Invalid NIC pointer\n", __func__);
		return -EINVAL;
	}
	if (!featuresp) {
		pr_err("%s: Invalid features pointer\n", __func__);
		return -EINVAL;
	}
	if (type != EF100_VDPA_DEVICE_TYPE_NET)
		return -EINVAL;

	MCDI_SET_DWORD(inbuf, VIRTIO_GET_FEATURES_IN_DEVICE_ID,
		       MC_CMD_VIRTIO_GET_FEATURES_IN_NET);
	rc = efx_mcdi_rpc(efx, MC_CMD_VIRTIO_GET_FEATURES, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);
	if (rc)
		return rc;
	if (outlen < MC_CMD_VIRTIO_GET_FEATURES_OUT_LEN)
		return -EIO;
	low_val = MCDI_DWORD(outbuf, VIRTIO_GET_FEATURES_OUT_FEATURES_LO);
	high_val = MCDI_DWORD(outbuf, VIRTIO_GET_FEATURES_OUT_FEATURES_HI);
	*featuresp = ((u64)high_val << 32) | low_val;
	return 0;
}

int efx_vdpa_verify_features(struct efx_nic *efx,
			     enum ef100_vdpa_device_type type, u64 features)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VIRTIO_TEST_FEATURES_IN_LEN);
	ssize_t outlen;
	int rc;

	if (!efx) {
		pr_err("%s: Invalid NIC pointer\n", __func__);
		return -EINVAL;
	}

	if (type != EF100_VDPA_DEVICE_TYPE_NET) {
		pr_err("%s: Device type not supported %d\n", __func__, type);
		return -EINVAL;
	}

	BUILD_BUG_ON(MC_CMD_VIRTIO_TEST_FEATURES_OUT_LEN != 0);
	MCDI_SET_DWORD(inbuf, VIRTIO_TEST_FEATURES_IN_DEVICE_ID,
		       MC_CMD_VIRTIO_GET_FEATURES_IN_NET);
	MCDI_SET_DWORD(inbuf, VIRTIO_TEST_FEATURES_IN_FEATURES_LO,
		       features & 0xFFFFFFFF);
	MCDI_SET_DWORD(inbuf, VIRTIO_TEST_FEATURES_IN_FEATURES_HI,
		       (features >> 32) & 0xFFFFFFFF);
	rc = efx_mcdi_rpc(efx, MC_CMD_VIRTIO_TEST_FEATURES, inbuf, sizeof(inbuf),
			  NULL, 0, &outlen);
	if (rc)
		return rc;
	if (outlen != MC_CMD_VIRTIO_TEST_FEATURES_OUT_LEN)
		return -EIO;
	return 0;
}

int efx_vdpa_vring_create(struct efx_vring_ctx *vring_ctx,
			  struct efx_vring_cfg *vring_cfg,
			  struct efx_vring_dyn_cfg *vring_dyn_cfg)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VIRTIO_INIT_QUEUE_REQ_LEN);
	struct efx_nic *efx;
	ssize_t outlen;
	int rc;

#ifdef EFX_NOT_UPSTREAM
	pr_info("%s: vf:%u type:%u vi:%u last_avail: %x last_used: %x\n",
		__func__, vring_ctx->vf_index,
		vring_ctx->mcdi_vring_type, vring_ctx->vi_index,
		vring_dyn_cfg ? vring_dyn_cfg->avail_idx : U32_MAX,
		vring_dyn_cfg ? vring_dyn_cfg->used_idx : U32_MAX);
#endif
	efx = vring_ctx->nic;

	BUILD_BUG_ON(MC_CMD_VIRTIO_INIT_QUEUE_RESP_LEN != 0);

	MCDI_SET_BYTE(inbuf, VIRTIO_INIT_QUEUE_REQ_QUEUE_TYPE,
		      vring_ctx->mcdi_vring_type);
	MCDI_SET_WORD(inbuf, VIRTIO_INIT_QUEUE_REQ_TARGET_VF,
		      vring_ctx->vf_index);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_INSTANCE,
		       vring_ctx->vi_index);

	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_SIZE, vring_cfg->size);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_DESC_TBL_ADDR_LO,
		       vring_cfg->desc & 0xFFFFFFFF);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_DESC_TBL_ADDR_HI,
		       vring_cfg->desc >> 32);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_AVAIL_RING_ADDR_LO,
		       vring_cfg->avail & 0xFFFFFFFF);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_AVAIL_RING_ADDR_HI,
		       vring_cfg->avail >> 32);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_USED_RING_ADDR_LO,
		       vring_cfg->used & 0xFFFFFFFF);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_USED_RING_ADDR_HI,
		       vring_cfg->used >> 32);

	if (vring_cfg->use_pasid) {
		MCDI_POPULATE_DWORD_1(inbuf, VIRTIO_INIT_QUEUE_REQ_FLAGS,
				      VIRTIO_INIT_QUEUE_REQ_USE_PASID, 1);
		MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_PASID,
			       vring_cfg->pasid);
	}
	MCDI_SET_WORD(inbuf, VIRTIO_INIT_QUEUE_REQ_MSIX_VECTOR,
		      vring_cfg->msix_vector);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_FEATURES_LO,
		       vring_cfg->features & 0xFFFFFFFF);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_FEATURES_HI,
		       vring_cfg->features >> 32);

	if (vring_dyn_cfg) {
		MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_INITIAL_PIDX,
			       vring_dyn_cfg->avail_idx);
		MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_INITIAL_CIDX,
			       vring_dyn_cfg->used_idx);
	}
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_MPORT_SELECTOR,
		       MAE_MPORT_SELECTOR_ASSIGNED);

	rc = efx_mcdi_rpc(efx, MC_CMD_VIRTIO_INIT_QUEUE, inbuf, sizeof(inbuf),
			  NULL, 0, &outlen);
	if (rc)
		return rc;
	if (outlen != MC_CMD_VIRTIO_INIT_QUEUE_RESP_LEN)
		return -EMSGSIZE;
	return 0;
}

int efx_vdpa_vring_destroy(struct efx_vring_ctx *vring_ctx,
			   struct efx_vring_dyn_cfg *vring_dyn_cfg)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VIRTIO_FINI_QUEUE_REQ_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_VIRTIO_FINI_QUEUE_RESP_LEN);
	struct efx_nic *efx;
	ssize_t outlen;
	int rc;

	efx = vring_ctx->nic;
#ifdef EFX_NOT_UPSTREAM
	pr_info("%s: Called for vf:0x%x type:%u vi:%u\n", __func__,
		vring_ctx->vf_index, vring_ctx->mcdi_vring_type,
		vring_ctx->vi_index);
#endif

	MCDI_SET_BYTE(inbuf, VIRTIO_FINI_QUEUE_REQ_QUEUE_TYPE,
		      vring_ctx->mcdi_vring_type);
	MCDI_SET_WORD(inbuf, VIRTIO_INIT_QUEUE_REQ_TARGET_VF,
		      vring_ctx->vf_index);
	MCDI_SET_DWORD(inbuf, VIRTIO_INIT_QUEUE_REQ_INSTANCE,
		       vring_ctx->vi_index);
	rc = efx_mcdi_rpc(efx, MC_CMD_VIRTIO_FINI_QUEUE, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);

	if (rc)
		return rc;

	if (outlen <  MC_CMD_VIRTIO_FINI_QUEUE_RESP_LEN)
		return -EMSGSIZE;

	if (vring_dyn_cfg) {
		vring_dyn_cfg->avail_idx = MCDI_DWORD(outbuf,
						      VIRTIO_FINI_QUEUE_RESP_FINAL_PIDX);
		vring_dyn_cfg->used_idx = MCDI_DWORD(outbuf,
						     VIRTIO_FINI_QUEUE_RESP_FINAL_CIDX);
	}

	return 0;
}

int efx_vdpa_get_doorbell_offset(struct efx_vring_ctx *vring_ctx,
				 u32 *offsetp)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VIRTIO_GET_DOORBELL_OFFSET_REQ_LEN);
	MCDI_DECLARE_BUF(outbuf,
			 MC_CMD_VIRTIO_GET_NET_DOORBELL_OFFSET_RESP_LEN);
	struct efx_nic *efx = vring_ctx->nic;
	ssize_t outlen;
	int rc;

#ifdef EFX_NOT_UPSTREAM
	pr_info("%s: Called for vf:0x%x type:%u vi:%u\n", __func__,
		vring_ctx->vf_index, vring_ctx->mcdi_vring_type,
		vring_ctx->vi_index);
#endif
	if (!offsetp)
		return -EINVAL;

	if (vring_ctx->mcdi_vring_type != MC_CMD_VIRTIO_INIT_QUEUE_REQ_NET_RXQ &&
	    vring_ctx->mcdi_vring_type != MC_CMD_VIRTIO_INIT_QUEUE_REQ_NET_TXQ) {
		pci_err(efx->pci_dev,
			"%s: Invalid Queue type %u\n",
			__func__, vring_ctx->mcdi_vring_type);
		return -EINVAL;
	}

	MCDI_SET_BYTE(inbuf, VIRTIO_GET_DOORBELL_OFFSET_REQ_DEVICE_ID,
		      MC_CMD_VIRTIO_GET_FEATURES_IN_NET);
	MCDI_SET_WORD(inbuf, VIRTIO_GET_DOORBELL_OFFSET_REQ_TARGET_VF,
		      vring_ctx->vf_index);
	MCDI_SET_DWORD(inbuf, VIRTIO_GET_DOORBELL_OFFSET_REQ_INSTANCE,
		       vring_ctx->vi_index);

	rc = efx_mcdi_rpc(efx, MC_CMD_VIRTIO_GET_DOORBELL_OFFSET, inbuf,
			  sizeof(inbuf), outbuf, sizeof(outbuf), &outlen);
	if (rc)
		return rc;

	if (outlen < MC_CMD_VIRTIO_GET_NET_DOORBELL_OFFSET_RESP_LEN)
		return -EIO;

	if (vring_ctx->mcdi_vring_type == MC_CMD_VIRTIO_INIT_QUEUE_REQ_NET_RXQ)
		*offsetp = MCDI_DWORD(outbuf,
				      VIRTIO_GET_NET_DOORBELL_OFFSET_RESP_RX_DBL_OFFSET);
	else
		*offsetp = MCDI_DWORD(outbuf,
				      VIRTIO_GET_NET_DOORBELL_OFFSET_RESP_TX_DBL_OFFSET);

	return 0;
}

int efx_vdpa_get_mac_address(struct efx_nic *efx, u8 *mac_address)
{
	struct ef100_nic_data *nic_data = efx->nic_data;
	unsigned int vf_index = nic_data->vf_index;
	efx_qword_t pciefn;
	u32 clid;
	int rc;

	/* Construct PCIE_FUNCTION structure for the VF */
	EFX_POPULATE_QWORD_3(pciefn,
			     PCIE_FUNCTION_PF, PCIE_FUNCTION_PF_NULL,
			     PCIE_FUNCTION_VF, vf_index,
			     PCIE_FUNCTION_INTF, PCIE_INTERFACE_CALLER);
	/* look up VF's client ID */
	rc = efx_ef100_lookup_client_id(efx, pciefn, &clid);
	if (rc) {
		pr_err("%s: Failed to get VF %u client ID, rc %d\n",
		       __func__, vf_index, rc);
	} else {
		pr_info("%s: VF %u client ID %#x\n", __func__, vf_index, clid);

		/* Get the assigned MAC address */
		rc = ef100_get_mac_address(efx, mac_address, clid,
					   efx->type->is_vf);
	}

	return rc;
}

int efx_vdpa_get_mtu(struct efx_nic *efx, u16 *mtu)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_SET_MAC_EXT_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_SET_MAC_V2_OUT_LEN);
	ssize_t outlen;
	int rc;

	MCDI_SET_DWORD(inbuf, SET_MAC_EXT_IN_CONTROL, 0);
	rc =  efx_mcdi_rpc(efx, MC_CMD_SET_MAC, inbuf, sizeof(inbuf),
			   outbuf, sizeof(outbuf), &outlen);
	if (rc)
		return rc;
	if (outlen < MC_CMD_SET_MAC_V2_OUT_LEN)
		return -EIO;

	*mtu = MCDI_DWORD(outbuf, SET_MAC_V2_OUT_MTU);
	return 0;
}

int efx_vdpa_get_link_details(struct efx_nic *efx, u16 *link_up,
			      u32 *link_speed, u8 *duplex)
{
	/* TODO:MCDI invocation for getting Link details.*/
	*link_up = VIRTIO_NET_S_LINK_UP;
	/* Setting Dummy link speed to 100Mpbs*/
	*link_speed = 100;
	*duplex = DUPLEX_FULL;
	return 0;
}
#endif
