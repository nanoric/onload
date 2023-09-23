/****************************************************************************
 * Driver for Solarflare network controllers and boards
 * Copyright 2018 Solarflare Communications Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */
#ifndef EFX_EF100_NIC_H
#define EFX_EF100_NIC_H

#include "net_driver.h"
#include "nic.h"
#include "mcdi_pcol.h"

#ifdef EFX_NOT_UPSTREAM
#ifdef CONFIG_SFC_DRIVERLINK

#ifdef EFX_C_MODEL
#define EF100_ONLOAD_VIS 4
#else
#define EF100_ONLOAD_VIS 64
#endif

#define EF100_ONLOAD_IRQS 8
#endif

#define EF100_BRIDGE_DOWNSTREAM_PCI_DEVICE 0x9434
#define EF100_BRIDGE_UPSTREAM_PCI_DEVICE 0x913f
#endif
#define EF100_QDMA_ADDR_REGIONS_MAX	MC_CMD_GET_DESC_ADDR_REGIONS_OUT_REGIONS_MAXNUM

extern const struct efx_nic_type ef100_pf_nic_type;
extern const struct efx_nic_type ef100_vf_nic_type;

int ef100_get_mac_address(struct efx_nic *efx, u8 *mac_address,
			  int client_handle, bool empty_ok);
int ef100_probe_netdev_pf(struct efx_nic *efx);
int ef100_probe_vf(struct efx_nic *efx);
void ef100_remove(struct efx_nic *efx);

enum {
	EF100_STAT_port_tx_bytes = GENERIC_STAT_COUNT,
	EF100_STAT_port_tx_packets,
	EF100_STAT_port_tx_pause,
	EF100_STAT_port_tx_unicast,
	EF100_STAT_port_tx_multicast,
	EF100_STAT_port_tx_broadcast,
	EF100_STAT_port_tx_lt64,
	EF100_STAT_port_tx_64,
	EF100_STAT_port_tx_65_to_127,
	EF100_STAT_port_tx_128_to_255,
	EF100_STAT_port_tx_256_to_511,
	EF100_STAT_port_tx_512_to_1023,
	EF100_STAT_port_tx_1024_to_15xx,
	EF100_STAT_port_tx_15xx_to_jumbo,
	EF100_STAT_port_rx_bytes,
	EF100_STAT_port_rx_packets,
	EF100_STAT_port_rx_good,
	EF100_STAT_port_rx_bad,
	EF100_STAT_port_rx_bad_bytes,
	EF100_STAT_port_rx_pause,
	EF100_STAT_port_rx_unicast,
	EF100_STAT_port_rx_multicast,
	EF100_STAT_port_rx_broadcast,
	EF100_STAT_port_rx_lt64,
	EF100_STAT_port_rx_64,
	EF100_STAT_port_rx_65_to_127,
	EF100_STAT_port_rx_128_to_255,
	EF100_STAT_port_rx_256_to_511,
	EF100_STAT_port_rx_512_to_1023,
	EF100_STAT_port_rx_1024_to_15xx,
	EF100_STAT_port_rx_15xx_to_jumbo,
	EF100_STAT_port_rx_gtjumbo,
	EF100_STAT_port_rx_bad_gtjumbo,
	EF100_STAT_port_rx_align_error,
	EF100_STAT_port_rx_length_error,
	EF100_STAT_port_rx_overflow,
	EF100_STAT_port_rx_nodesc_drops,
	EF100_STAT_COUNT
};

/* Keep this in sync with the contents of bar_config_name. */
enum ef100_bar_config {
	EF100_BAR_CONFIG_NONE,
	EF100_BAR_CONFIG_EF100,
	EF100_BAR_CONFIG_VDPA,
};

#if defined(EFX_USE_KCOMPAT) && !defined(EFX_HAVE_VDPA_MGMT_INTERFACE)
#ifdef CONFIG_SFC_VDPA
enum ef100_vdpa_class {
	EF100_VDPA_CLASS_NONE,
	EF100_VDPA_CLASS_NET,
};
#endif
#endif

enum {
	EFX_EF100_TEST = 1,
	EFX_EF100_REFILL,
};
#define EFX_EF100_DRVGEN_MAGIC(_code, _data)	((_code) | ((_data) << 8))
#define EFX_EF100_DRVGEN_CODE(_magic)	((_magic) & 0xff)
#define EFX_EF100_DRVGEN_DATA(_magic)	((_magic) >> 8)

/** QDMA address region
 * This is the driver equivalent of DEVEL_NIC_ADDR_REGION structuredef
 * in YAML.
 */
struct ef100_addr_region {
	dma_addr_t qdma_addr;
	dma_addr_t trgt_addr;
	u32 size_log2;
	u32 trgt_alignment_log2;
};

struct ef100_nic_data {
	struct efx_nic *efx;
	struct efx_buffer mcdi_buf;
	unsigned long mcdi_buf_use;
	u32 datapath_caps;
	u32 datapath_caps2;
	u32 datapath_caps3;
	unsigned int pf_index;
	unsigned int vf_index;
	u16 warm_boot_count;
	u8 port_id[ETH_ALEN];
	enum ef100_bar_config bar_config;
#if defined(EFX_USE_KCOMPAT) && !defined(EFX_HAVE_VDPA_MGMT_INTERFACE)
#ifdef CONFIG_SFC_VDPA
	enum ef100_vdpa_class vdpa_class;
#endif
#endif
	struct mutex bar_config_lock; /* lock to control access to bar config */
	u64 licensed_features;
	unsigned long *evq_phases;
	u64 stats[EF100_STAT_COUNT];
	spinlock_t vf_reps_lock; /* Synchronises 'all-VFreps' operations */
	unsigned int vf_rep_count; /* usually but not always efx->vf_count */
	struct net_device **vf_rep; /* local VF reps */
	struct list_head rem_reps; /* remote reps */
	u32 base_mport;
	u32 own_mport;
	u32 local_mae_intf; /* interface_idx that corresponds to us, in mport enumerate */
	bool have_mport; /* base_mport was populated successfully */
	bool have_own_mport; /* own_mport was populated successfully */
	bool have_local_intf; /* local_mae_intf was populated successfully */
	bool filters_up; /* filter table has been upped */
	bool grp_mae; /* MAE Privilege */
	bool vdpa_supported; /* true if vdpa is supported on this PCIe FN */
#if defined(EFX_USE_KCOMPAT) && defined(EFX_TC_OFFLOAD) && \
    !defined(EFX_HAVE_FLOW_INDR_BLOCK_CB_REGISTER)
	spinlock_t udp_tunnels_lock;
	struct list_head udp_tunnels;
#endif
	u16 tso_max_hdr_len;
	u16 tso_max_payload_num_segs;
	u16 tso_max_frames;
	unsigned int tso_max_payload_len;
	unsigned int addr_mapping_type;
	struct ef100_addr_region addr_region[EF100_QDMA_ADDR_REGIONS_MAX];
};

void __ef100_detach_reps(struct efx_nic *efx);
void __ef100_attach_reps(struct efx_nic *efx);

#define efx_ef100_has_cap(caps, flag) \
	(!!((caps) & BIT_ULL(MC_CMD_GET_CAPABILITIES_V10_OUT_ ## flag ## _LBN)))

int efx_ef100_init_datapath_caps(struct efx_nic *efx);
int ef100_phy_probe(struct efx_nic *efx);
int ef100_filter_table_probe(struct efx_nic *efx);
int efx_ef100_lookup_client_id(struct efx_nic *efx, efx_qword_t pciefn,
			       u32 *id);

static inline bool
ef100_region_addr_is_populated(struct ef100_addr_region *region)
{
	return region->size_log2 != 0;
}

static inline bool efx_have_mport_journal_event(struct efx_nic *efx)
{
	struct ef100_nic_data *nic_data = efx->nic_data;

	return efx_ef100_has_cap(nic_data->datapath_caps3,
				 DYNAMIC_MPORT_JOURNAL);
}

int efx_ef100_set_bar_config(struct efx_nic *efx,
			     enum ef100_bar_config new_config);
#endif	/* EFX_EF100_NIC_H */
