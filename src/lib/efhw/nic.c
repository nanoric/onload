/* SPDX-License-Identifier: GPL-2.0 */
/* X-SPDX-Copyright-Text: (c) Copyright 2002-2020 Xilinx, Inc. */
/****************************************************************************
 * Driver for Solarflare network controllers -
 *          resource management for Xen backend, OpenOnload, etc
 *           (including support for SFE4001 10GBT NIC)
 *
 * This file contains EtherFabric Generic NIC instance (init, interrupts,
 * etc)
 *
 * Copyright 2005-2007: Solarflare Communications Inc,
 *                      9501 Jeronimo Road, Suite 250,
 *                      Irvine, CA 92618, USA
 *
 * Developed and maintained by Solarflare Communications:
 *                      <linux-xen-drivers@solarflare.com>
 *                      <onload-dev@solarflare.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************
 */

#include <ci/efhw/debug.h>
#include <ci/driver/efab/hardware.h>
#include <ci/driver/ci_efct.h>
#include <ci/efhw/ef10.h>
#include <ci/efhw/ef100.h>
#include <ci/efhw/af_xdp.h>
#include <ci/efhw/efct.h>
#include <ci/efhw/nic.h>
#include <ci/efhw/eventq.h>

static void ef10_device_type_init(struct efhw_device_type *dt,
				  char variant, int device_id,
				  int class_revision)
{
	dt->function = device_id & 0x1000 ? EFHW_FUNCTION_VF :
		       EFHW_FUNCTION_PF;
	dt->arch = EFHW_ARCH_EF10;
	dt->variant = variant;
	dt->revision = class_revision;
}


static void ef100_device_type_init(struct efhw_device_type *dt,
				   char variant, int device_id,
				   int class_revision)
{
	dt->function = EFHW_FUNCTION_PF;
	dt->arch = EFHW_ARCH_EF100;
	dt->variant = variant;
	dt->revision = class_revision;
}


/* Return 0 if not a known type */
int efhw_sfc_device_type_init(struct efhw_device_type *dt, struct pci_dev* dev)
{
	int rc;
	u8 class_revision;

	rc = pci_read_config_byte(dev, PCI_CLASS_REVISION, &class_revision);
	if (rc != 0) {
		EFHW_ERR("%s: pci_read_config_byte failed (%d)",
			 __func__, rc);
		return pcibios_err_to_errno(rc);
	}

	/* FIXME: not sure about right way for Xilinx licensing */
	if (dev->vendor != 0x1924 && dev->vendor != 0x10ee)
		return -ENODEV;

	memset(dt, 0, sizeof(*dt));
	
	switch (dev->device) {
	case 0x0703:
	case 0x6703:
	case 0x0710:
	/* Development */
	case 0x0770:
	case 0x7777:
	/* cosim */
	case 0x7778:
	case 0x0803:
    case 0x0813:
		printk(KERN_NOTICE "6000-series and earlier adapters are not "
		                   "supported by Onload\n");
		return -ENODEV;
	case 0x1923:
	case 0x1903:
	case 0x0923:
	case 0x0903:
	case 0x0901:
		ef10_device_type_init(dt, 'A', dev->device, class_revision);
		break;
	case 0x1913:
	case 0x1a03:
	case 0x0913:
	case 0x0a03:
		ef10_device_type_init(dt, 'B', dev->device, class_revision);
		break;
	case 0x1b03:
	case 0x0b03:
		ef10_device_type_init(dt, 'C', dev->device, class_revision);
		break;
	case 0x0100:
		/* FIXME: add properly variants and revisions for EF100 */
		ef100_device_type_init(dt, 'A', dev->device, class_revision);
		break;
	default:
		return -ENODEV;
	}

	return 0;
}


int efhw_nondl_device_type_init(struct efhw_device_type *dt)
{
	*dt = (struct efhw_device_type) {
		.arch = EFHW_ARCH_AF_XDP,
		.variant = 'X',
		.revision = 0,
	};
	return 0;
}


/*--------------------------------------------------------------------
 *
 * NIC Initialisation
 *
 *--------------------------------------------------------------------*/

void efhw_nic_update_pci_info(struct efhw_nic *nic)
{
	struct pci_dev *pci_dev = efhw_nic_get_pci_dev(nic);
	if( !pci_dev )
		return;

	nic->bus_number = pci_dev->bus->number;
	nic->domain = pci_domain_nr(pci_dev->bus);

	pci_dev_put(pci_dev);
}

/* make this separate from initialising data structure
** to allow this to be called at a later time once we can access PCI
** config space to find out what hardware we have
*/
void efhw_nic_init(struct efhw_nic *nic, unsigned flags,
		   const struct efhw_device_type *dev_type, unsigned map_min,
		   unsigned map_max, unsigned vi_base, unsigned vi_shift,
		   unsigned mem_bar, unsigned vi_stride,
		   struct net_device *net_dev, struct device *dev)
{
	nic->devtype = *dev_type;
	nic->flags = flags;
	nic->resetting = 0;
	nic->int_prime_reg = 0;
	nic->vi_irq_n_ranges = 0;
	nic->mtu = 1500 + ETH_HLEN; /* ? + ETH_VLAN_HLEN */
	/* Default: this will get overwritten if better value is known */
	nic->timer_quantum_ns = 4968; 
	nic->vi_min = map_min;
	nic->vi_lim = map_max;
	nic->net_dev = net_dev;
	nic->dev = dev;
	spin_lock_init(&nic->pci_dev_lock);

	switch (nic->devtype.arch) {
#if CI_HAVE_SFC
	case EFHW_ARCH_EF10:
		nic->q_sizes[EFHW_EVQ] = 512 | 1024 | 2048 | 4096 | 8192 |
			16384 | 32768;
		nic->q_sizes[EFHW_TXQ] = 512 | 1024 | 2048;
		nic->q_sizes[EFHW_RXQ] = 512 | 1024 | 2048 | 4096;

		switch (dev_type->variant) {
		case 'C':
			nic->ctr_ap_bar = EF10_MEDFORD2_P_CTR_AP_BAR;
			break;
		default:
			nic->ctr_ap_bar = dev_type->function == EFHW_FUNCTION_PF ?
				EF10_PF_P_CTR_AP_BAR : EF10_VF_P_CTR_AP_BAR;
		}

		if (mem_bar != EFHW_MEM_BAR_UNDEFINED)
			nic->ctr_ap_bar = mem_bar;
		nic->ctr_ap_addr = pci_resource_start(to_pci_dev(dev),
						      nic->ctr_ap_bar);

		nic->num_evqs   = 1024;
		nic->num_dmaqs  = 1024;
		nic->num_timers = 1024;
		/* For EF10 we map VIs on demand.  We don't need mappings
		 * for any other reason as all control ops go via the net
		 * driver and MCDI.
		 */
		nic->efhw_func = &ef10_char_functional_units;
		nic->vi_base = vi_base;
		nic->vi_shift = vi_shift;
		nic->vi_stride = vi_stride;
		break;
	case EFHW_ARCH_EF100:
		/* FIXME: wrong numbers for queues sizes */
		nic->q_sizes[EFHW_EVQ] = 16 | 256 | 512 | 1024 | 2048 | 4096 |
			8192 | 16384;
		nic->q_sizes[EFHW_TXQ] = 16 | 256 | 512 | 1024 | 2048 | 4096 |
			8192 | 16384;
		nic->q_sizes[EFHW_RXQ] = 16 | 256 | 512 | 1024 | 2048 | 4096 |
			8192 | 16384 ;

		nic->ctr_ap_bar = EF100_P_CTR_AP_BAR;

		if (mem_bar != EFHW_MEM_BAR_UNDEFINED)
			nic->ctr_ap_bar = mem_bar;
		nic->ctr_ap_addr = pci_resource_start(to_pci_dev(dev),
						      nic->ctr_ap_bar);

		/* FIXME: wrong numbers for queues numbers*/
		nic->num_evqs   = 1024;
		nic->num_dmaqs  = 1024;
		nic->num_timers = 1024;

		nic->efhw_func = &ef100_char_functional_units;
		nic->vi_base = vi_base;
		nic->vi_shift = vi_shift;
		nic->vi_stride = vi_stride;
		break;
#endif /* CI_HAVE_SFC */
#ifdef EFHW_HAS_AF_XDP
	case EFHW_ARCH_AF_XDP:
		/* No restrictions on queue sizes */
		nic->q_sizes[EFHW_EVQ] = ~0;
		nic->q_sizes[EFHW_TXQ] = ~0;
		nic->q_sizes[EFHW_RXQ] = ~0;
		nic->num_evqs = 1;
		nic->num_dmaqs = 1;
		nic->num_timers = 0;
		nic->efhw_func = &af_xdp_char_functional_units;
		break;
#endif
#if CI_HAVE_EFCT_AUX
	case EFHW_ARCH_EFCT:
		nic->q_sizes[EFHW_EVQ] = 128 | 256 | 512 | 1024 | 2048 | 4096 |
			8192;
		/* The TXQ is SW only, but reflects a limited HW resource */
		nic->q_sizes[EFHW_TXQ] = 512;
		/* RXQ is virtual/software-only, but some restrictions
		 * Limited by CI_EFCT_MAX_SUPERBUFS and XNET-249 to 131,072
		 * Also EF_VI code currently still limited to powers of 2 */ 
		nic->q_sizes[EFHW_RXQ] = 512 | 1024 | 2048 | 4096 | 8192 |
		  16384 | 32768 | 65536 | 131072;
		nic->efhw_func = &efct_char_functional_units;
		break;
#endif
	default:
		EFHW_ASSERT(0);
		break;
	}

	efhw_nic_update_pci_info(nic);
}

void efhw_nic_dtor(struct efhw_nic *nic)
{
	EFHW_ASSERT(nic);

	spin_lock_destroy(&nic->pci_dev_lock);

	EFHW_TRACE("%s: DONE", __FUNCTION__);
}


/* Returns the struct device for the NIC, taking out a reference to it.
 * Callers should call put_device() on the returned pointer to release that
 * reference when they're finished. */
struct device* efhw_nic_get_dev(struct efhw_nic* nic)
{
	struct device* dev = NULL;
	spin_lock_bh(&nic->pci_dev_lock);
	if( nic->dev )
		dev = get_device(nic->dev);
	spin_unlock_bh(&nic->pci_dev_lock);
	return dev;
}


/* Returns the struct net_device for the NIC, taking out a reference to it.
 * Callers should call dev_put() on the returned pointer to release that
 * reference when they're finished. */
struct net_device* efhw_nic_get_net_dev(struct efhw_nic* nic)
{
	struct net_device* dev;
	spin_lock_bh(&nic->pci_dev_lock);
	dev = nic->net_dev;
	if( dev != NULL )
		dev_hold(dev);
	spin_unlock_bh(&nic->pci_dev_lock);
	return dev;
}
EXPORT_SYMBOL(efhw_nic_get_net_dev);

bool efhw_nic_phys_contig_queue(struct efhw_nic *nic, enum efhw_q_type type)
{
	switch(type) {
	case EFHW_EVQ:
		return !!(nic->flags & NIC_FLAG_PHYS_CONTIG_EVQ);
	case EFHW_TXQ:
		return !!(nic->flags & NIC_FLAG_PHYS_CONTIG_TXQ);
	case EFHW_RXQ:
		return !!(nic->flags & NIC_FLAG_PHYS_CONTIG_RXQ);
	default:
		EFHW_ASSERT(false);
		return true;
	};
}
