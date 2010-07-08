/* ohci.c - OHCI Support.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/dl.h>
#include <grub/mm.h>
#include <grub/usb.h>
#include <grub/usbtrans.h>
#include <grub/misc.h>
#include <grub/pci.h>
#include <grub/cpu/pci.h>
#include <grub/cpu/io.h>
#include <grub/time.h>
#include <grub/cs5536.h>
#include <grub/loader.h>

struct grub_ohci_hcca
{
  /* Pointers to Interrupt Endpoint Descriptors.  Not used by
     GRUB.  */
  grub_uint32_t inttable[32];

  /* Current frame number.  */
  grub_uint16_t framenumber;

  grub_uint16_t pad;

  /* List of completed TDs.  */
  grub_uint32_t donehead;

  grub_uint8_t reserved[116];
} __attribute__((packed));

/* OHCI General Transfer Descriptor */
struct grub_ohci_td
{
  /* Information used to construct the TOKEN packet.  */
  grub_uint32_t token;
  grub_uint32_t buffer; /* LittleEndian physical address */
  grub_uint32_t next_td; /* LittleEndian physical address */
  grub_uint32_t buffer_end; /* LittleEndian physical address */
  /* next values are not for OHCI HW */
  grub_uint32_t prev_td_phys; /* we need it to find previous TD
                               * physical address in CPU endian */
  grub_uint32_t link_td; /* pointer to next free/chained TD
                          * pointer as uint32 */
  grub_uint32_t tr_index; /* index of TD in transfer */
  grub_uint8_t pad[4]; /* padding to 32 bytes */
} __attribute__((packed));

/* OHCI Endpoint Descriptor.  */
struct grub_ohci_ed
{
  grub_uint32_t target;
  grub_uint32_t td_tail;
  grub_uint32_t td_head;
  grub_uint32_t next_ed;
} __attribute__((packed));

typedef volatile struct grub_ohci_td *grub_ohci_td_t;
typedef volatile struct grub_ohci_ed *grub_ohci_ed_t;

/* Experimental change of ED/TD allocation */
/* Little bit similar as in UHCI */
/* Implementation assumes:
 *      32-bits architecture - XXX: fix for 64-bits
 *      memory allocated by grub_memalign_dma32 must be continuous
 *      in virtual and also in physical memory */
struct grub_ohci
{
  volatile grub_uint32_t *iobase;
  volatile struct grub_ohci_hcca *hcca;
  grub_uint32_t hcca_addr;
  struct grub_pci_dma_chunk *hcca_chunk;
  grub_ohci_ed_t ed_ctrl; /* EDs for CONTROL */
  grub_uint32_t ed_ctrl_addr;
  struct grub_pci_dma_chunk *ed_ctrl_chunk;
  grub_ohci_ed_t ed_bulk; /* EDs for BULK */
  grub_uint32_t ed_bulk_addr;
  struct grub_pci_dma_chunk *ed_bulk_chunk;
  grub_ohci_td_t td; /* TDs */
  grub_uint32_t td_addr;
  struct grub_pci_dma_chunk *td_chunk;
  struct grub_ohci *next;
  grub_ohci_td_t td_free; /* Pointer to first free TD */
  int bad_OHCI;
};

static struct grub_ohci *ohci;

typedef enum
{
  GRUB_OHCI_REG_REVISION = 0x00,
  GRUB_OHCI_REG_CONTROL,
  GRUB_OHCI_REG_CMDSTATUS,
  GRUB_OHCI_REG_INTSTATUS,
  GRUB_OHCI_REG_INTENA,
  GRUB_OHCI_REG_INTDIS,
  GRUB_OHCI_REG_HCCA,
  GRUB_OHCI_REG_PERIODIC,
  GRUB_OHCI_REG_CONTROLHEAD,
  GRUB_OHCI_REG_CONTROLCURR,
  GRUB_OHCI_REG_BULKHEAD,
  GRUB_OHCI_REG_BULKCURR,
  GRUB_OHCI_REG_DONEHEAD,
  GRUB_OHCI_REG_FRAME_INTERVAL,
  GRUB_OHCI_REG_PERIODIC_START = 16,
  GRUB_OHCI_REG_RHUBA = 18,
  GRUB_OHCI_REG_RHUBPORT = 21,
  GRUB_OHCI_REG_LEGACY_CONTROL = 0x100,
  GRUB_OHCI_REG_LEGACY_INPUT = 0x104,
  GRUB_OHCI_REG_LEGACY_OUTPUT = 0x108,
  GRUB_OHCI_REG_LEGACY_STATUS = 0x10c
} grub_ohci_reg_t;

#define GRUB_OHCI_RHUB_PORT_POWER_MASK 0x300
#define GRUB_OHCI_RHUB_PORT_ALL_POWERED 0x200

#define GRUB_OHCI_REG_FRAME_INTERVAL_FSMPS_MASK 0x8fff0000
#define GRUB_OHCI_REG_FRAME_INTERVAL_FSMPS_SHIFT 16
#define GRUB_OHCI_REG_FRAME_INTERVAL_FI_SHIFT 0

/* XXX: Is this choice of timings sane?  */
#define GRUB_OHCI_FSMPS 0x2778
#define GRUB_OHCI_PERIODIC_START 0x257f
#define GRUB_OHCI_FRAME_INTERVAL 0x2edf

#define GRUB_OHCI_SET_PORT_ENABLE (1 << 1)
#define GRUB_OHCI_CLEAR_PORT_ENABLE (1 << 0)
#define GRUB_OHCI_SET_PORT_RESET (1 << 4)
#define GRUB_OHCI_SET_PORT_RESET_STATUS_CHANGE (1 << 20)

#define GRUB_OHCI_REG_CONTROL_BULK_ENABLE (1 << 5)
#define GRUB_OHCI_REG_CONTROL_CONTROL_ENABLE (1 << 4)

#define GRUB_OHCI_RESET_CONNECT_CHANGE (1 << 16)
#define GRUB_OHCI_CTRL_EDS 16
#define GRUB_OHCI_BULK_EDS 16
#define GRUB_OHCI_TDS 256

#define GRUB_OHCI_ED_ADDR_MASK 0x7ff

#define GRUB_OHCI_ED_PHYS2VIRT(o, bulk, x) ( !(x) ? NULL : ( \
  (bulk) ? \
  (grub_ohci_ed_t)((x) - (o)->ed_bulk_addr + (grub_uint32_t)(o)->ed_bulk) \
  : \
  (grub_ohci_ed_t)((x) - (o)->ed_ctrl_addr + (grub_uint32_t)(o)->ed_ctrl) ) )
  
#define GRUB_OHCI_ED_VIRT2PHYS(o, bulk, x) ( !(x) ? 0 : ( \
  (bulk) ? \
  ((grub_uint32_t)(x) - (grub_uint32_t)(o)->ed_bulk + (o)->ed_bulk_addr) \
  : \
  ((grub_uint32_t)(x) - (grub_uint32_t)(o)->ed_ctrl + (o)->ed_ctrl_addr) ) )

#define GRUB_OHCI_TD_PHYS2VIRT(o, x) ( !(x) ? NULL : \
  (grub_ohci_td_t)((x) - (o)->td_addr + (grub_uint32_t)(o)->td) )

#define GRUB_OHCI_TD_VIRT2PHYS(o, x) ( !(x) ? 0 : \
  ((grub_uint32_t)(x) - (grub_uint32_t)(o)->td + (o)->td_addr) )

  
static grub_uint32_t
grub_ohci_readreg32 (struct grub_ohci *o, grub_ohci_reg_t reg)
{
  return grub_le_to_cpu32 (*(o->iobase + reg));
}

static void
grub_ohci_writereg32 (struct grub_ohci *o,
		      grub_ohci_reg_t reg, grub_uint32_t val)
{
  *(o->iobase + reg) = grub_cpu_to_le32 (val);
}



/* Iterate over all PCI devices.  Determine if a device is an OHCI
   controller.  If this is the case, initialize it.  */
static int NESTED_FUNC_ATTR
grub_ohci_pci_iter (grub_pci_device_t dev,
		    grub_pci_id_t pciid)
{
  grub_uint32_t interf;
  grub_uint32_t base;
  grub_pci_address_t addr;
  struct grub_ohci *o;
  grub_uint32_t revision;
  int cs5536;
  int j;
  
  /* Determine IO base address.  */
  grub_dprintf ("ohci", "pciid = %x\n", pciid);

  if (pciid == GRUB_CS5536_PCIID)
    {
      grub_uint64_t basereg;

      cs5536 = 1;
      basereg = grub_cs5536_read_msr (dev, GRUB_CS5536_MSR_USB_OHCI_BASE);
      if (!(basereg & GRUB_CS5536_MSR_USB_BASE_MEMORY_ENABLE))
	{
	  /* Shouldn't happen.  */
	  grub_dprintf ("ohci", "No OHCI address is assigned\n");
	  return 0;
	}
      base = (basereg & GRUB_CS5536_MSR_USB_BASE_ADDR_MASK);
      basereg |= GRUB_CS5536_MSR_USB_BASE_BUS_MASTER;
      basereg &= ~GRUB_CS5536_MSR_USB_BASE_PME_ENABLED;
      basereg &= ~GRUB_CS5536_MSR_USB_BASE_PME_STATUS;
      grub_cs5536_write_msr (dev, GRUB_CS5536_MSR_USB_OHCI_BASE, basereg);
    }
  else
    {
      grub_uint32_t class_code;
      grub_uint32_t class;
      grub_uint32_t subclass;

      addr = grub_pci_make_address (dev, GRUB_PCI_REG_CLASS);
      class_code = grub_pci_read (addr) >> 8;
      
      interf = class_code & 0xFF;
      subclass = (class_code >> 8) & 0xFF;
      class = class_code >> 16;

      /* If this is not an OHCI controller, just return.  */
      if (class != 0x0c || subclass != 0x03 || interf != 0x10)
	return 0;

      addr = grub_pci_make_address (dev, GRUB_PCI_REG_ADDRESS_REG0);
      base = grub_pci_read (addr);

#if 0
      /* Stop if there is no IO space base address defined.  */
      if (! (base & 1))
	return 0;
#endif

      grub_dprintf ("ohci", "class=0x%02x 0x%02x interface 0x%02x\n",
		    class, subclass, interf);
    }

  /* Allocate memory for the controller and register it.  */
  o = grub_malloc (sizeof (*o));
  if (! o)
    return 1;
  grub_memset ((void*)o, 0, sizeof (*o));
  o->iobase = grub_pci_device_map_range (dev, base, 0x800);

  grub_dprintf ("ohci", "base=%p\n", o->iobase);

  /* Reserve memory for the HCCA.  */
  o->hcca_chunk = grub_memalign_dma32 (256, 256);
  if (! o->hcca_chunk)
    goto fail;
  o->hcca = grub_dma_get_virt (o->hcca_chunk);
  o->hcca_addr = grub_dma_get_phys (o->hcca_chunk);
  grub_memset ((void*)o->hcca, 0, sizeof(*o->hcca));
  grub_dprintf ("ohci", "hcca: chunk=%p, virt=%p, phys=0x%02x\n",
                o->hcca_chunk, o->hcca, o->hcca_addr);

  /* Reserve memory for ctrl EDs.  */
  o->ed_ctrl_chunk = grub_memalign_dma32 (16, sizeof(struct grub_ohci_ed)*GRUB_OHCI_CTRL_EDS);
  if (! o->ed_ctrl_chunk)
    goto fail;
  o->ed_ctrl = grub_dma_get_virt (o->ed_ctrl_chunk);
  o->ed_ctrl_addr = grub_dma_get_phys (o->ed_ctrl_chunk);
  /* Preset EDs */
  grub_memset ((void*)o->ed_ctrl, 0, sizeof(struct grub_ohci_ed) * GRUB_OHCI_CTRL_EDS);
  for (j=0; j < GRUB_OHCI_CTRL_EDS; j++)
    o->ed_ctrl[j].target = grub_cpu_to_le32 (1 << 14); /* skip */
    
  grub_dprintf ("ohci", "EDs-C: chunk=%p, virt=%p, phys=0x%02x\n",
                o->ed_ctrl_chunk, o->ed_ctrl, o->ed_ctrl_addr);

  /* Reserve memory for bulk EDs.  */
  o->ed_bulk_chunk = grub_memalign_dma32 (16, sizeof(struct grub_ohci_ed)*GRUB_OHCI_BULK_EDS);
  if (! o->ed_bulk_chunk)
    goto fail;
  o->ed_bulk = grub_dma_get_virt (o->ed_bulk_chunk);
  o->ed_bulk_addr = grub_dma_get_phys (o->ed_bulk_chunk);
  /* Preset EDs */
  grub_memset ((void*)o->ed_bulk, 0, sizeof(struct grub_ohci_ed) * GRUB_OHCI_BULK_EDS);
  for (j=0; j < GRUB_OHCI_BULK_EDS; j++)
    o->ed_bulk[j].target = grub_cpu_to_le32 (1 << 14); /* skip */

  grub_dprintf ("ohci", "EDs-B: chunk=%p, virt=%p, phys=0x%02x\n",
                o->ed_bulk_chunk, o->ed_bulk, o->ed_bulk_addr);

  /* Reserve memory for TDs.  */
  o->td_chunk = grub_memalign_dma32 (32, sizeof(struct grub_ohci_td)*GRUB_OHCI_TDS);
  /* Why is it aligned on 32 boundary if spec. says 16 ?
   * We have structure 32 bytes long and we don't want cross
   * 4K boundary inside structure. */
  if (! o->td_chunk)
    goto fail;
  o->td_free = o->td = grub_dma_get_virt (o->td_chunk);
  o->td_addr = grub_dma_get_phys (o->td_chunk);
  /* Preset free TDs chain in TDs */
  grub_memset ((void*)o->td, 0, sizeof(struct grub_ohci_td) * GRUB_OHCI_TDS);
  for (j=0; j < (GRUB_OHCI_TDS-1); j++)
    o->td[j].link_td = (grub_uint32_t)&o->td[j+1];

  grub_dprintf ("ohci", "TDs: chunk=%p, virt=%p, phys=0x%02x\n",
                o->td_chunk, o->td, o->td_addr);

  /* Check if the OHCI revision is actually 1.0 as supported.  */
  revision = grub_ohci_readreg32 (o, GRUB_OHCI_REG_REVISION);
  grub_dprintf ("ohci", "OHCI revision=0x%02x\n", revision & 0xFF);
  if ((revision & 0xFF) != 0x10)
    goto fail;

  {
    grub_uint32_t control;
    /* Check SMM/BIOS ownership of OHCI (SMM = USB Legacy Support driver for BIOS) */
    control = grub_ohci_readreg32 (o, GRUB_OHCI_REG_CONTROL);
    if ((control & 0x100) != 0)
      {
	unsigned i;
	grub_dprintf("ohci", "OHCI is owned by SMM\n");
	/* Do change of ownership */
	/* Ownership change request */
	grub_ohci_writereg32 (o, GRUB_OHCI_REG_CMDSTATUS, (1<<3)); /* XXX: Magic.  */
	/* Waiting for SMM deactivation */
	for (i=0; i < 10; i++)
	  {
	    if ((grub_ohci_readreg32 (o, GRUB_OHCI_REG_CONTROL) & 0x100) == 0)
	      {
		grub_dprintf("ohci", "Ownership changed normally.\n");
		break;
	      }
	    grub_millisleep (100);
          }
	if (i >= 10)
	  {
	    grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROL,
				  grub_ohci_readreg32 (o, GRUB_OHCI_REG_CONTROL) & ~0x100);
	    grub_dprintf("ohci", "Ownership changing timeout, change forced !\n");
	  }
      }
    else if (((control & 0x100) == 0) && 
	     ((control & 0xc0) != 0)) /* Not owned by SMM nor reset */
      {
	grub_dprintf("ohci", "OHCI is owned by BIOS\n");
	/* Do change of ownership - not implemented yet... */
	/* In fact we probably need to do nothing ...? */
      }
    else
      {
	grub_dprintf("ohci", "OHCI is not owned by SMM nor BIOS\n");
	/* We can setup OHCI. */
      }  
  }

  /* Suspend the OHCI by issuing a reset.  */
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_CMDSTATUS, 1); /* XXX: Magic.  */
  grub_millisleep (1);
  grub_dprintf ("ohci", "OHCI reset\n");

  grub_ohci_writereg32 (o, GRUB_OHCI_REG_FRAME_INTERVAL,
			(GRUB_OHCI_FSMPS
			 << GRUB_OHCI_REG_FRAME_INTERVAL_FSMPS_SHIFT)
			| (GRUB_OHCI_FRAME_INTERVAL
			   << GRUB_OHCI_REG_FRAME_INTERVAL_FI_SHIFT));

  grub_ohci_writereg32 (o, GRUB_OHCI_REG_PERIODIC_START,
			GRUB_OHCI_PERIODIC_START);

  /* Setup the HCCA.  */
  o->hcca->donehead = 0;
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_HCCA, o->hcca_addr);
  grub_dprintf ("ohci", "OHCI HCCA\n");

  /* Misc. pre-sets. */
  o->hcca->donehead = 0;
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, 0x7f); /* Clears everything */
  /* We don't want modify CONTROL/BULK HEAD registers.
   * So we assign to HEAD registers zero ED from related array
   * and we will not use this ED, it will be always skipped.
   * It should not produce notable performance penalty (I hope). */
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLHEAD, o->ed_ctrl_addr);
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLCURR, 0);
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKHEAD, o->ed_bulk_addr);
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKCURR, 0);

  /* Check OHCI Legacy Support */
  if ((revision & 0x100) != 0)
    {
      grub_dprintf ("ohci", "Legacy Support registers detected\n");
      grub_dprintf ("ohci", "Current state of legacy control reg.: 0x%04x\n",
		    grub_ohci_readreg32 (o, GRUB_OHCI_REG_LEGACY_CONTROL));
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_LEGACY_CONTROL,
			    (grub_ohci_readreg32 (o, GRUB_OHCI_REG_LEGACY_CONTROL)) & ~1);
      grub_dprintf ("ohci", "OHCI Legacy Support disabled.\n");
    }

  /* Enable the OHCI + enable CONTROL and BULK LIST.  */
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROL,
			(2 << 6)
			| GRUB_OHCI_REG_CONTROL_CONTROL_ENABLE
			| GRUB_OHCI_REG_CONTROL_BULK_ENABLE );
  grub_dprintf ("ohci", "OHCI enable: 0x%02x\n",
		(grub_ohci_readreg32 (o, GRUB_OHCI_REG_CONTROL) >> 6) & 3);

  /* Power on all ports */
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_RHUBA,
                       (grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBA)
                        & ~GRUB_OHCI_RHUB_PORT_POWER_MASK)
                       | GRUB_OHCI_RHUB_PORT_ALL_POWERED);
  /* Now we have hot-plugging, we need to wait for stable power only */
  grub_millisleep (100);

  /* Link to ohci now that initialisation is successful.  */
  o->next = ohci;
  ohci = o;

  return 0;

 fail:
  if (o)
    grub_dma_free (o->td_chunk);
    grub_dma_free (o->ed_bulk_chunk);
    grub_dma_free (o->ed_ctrl_chunk);
    grub_dma_free (o->hcca_chunk);
  grub_free (o);

  return 1;
}


static void
grub_ohci_inithw (void)
{
  grub_pci_iterate (grub_ohci_pci_iter);
}



static int
grub_ohci_iterate (int (*hook) (grub_usb_controller_t dev))
{
  struct grub_ohci *o;
  struct grub_usb_controller dev;

  for (o = ohci; o; o = o->next)
    {
      dev.data = o;
      if (hook (&dev))
	return 1;
    }

  return 0;
}

static grub_ohci_ed_t
grub_ohci_find_ed (struct grub_ohci *o, int bulk, grub_uint32_t target)
{
  grub_ohci_ed_t ed, ed_next;
  grub_uint32_t target_addr = target & GRUB_OHCI_ED_ADDR_MASK;
  int count;
  int i;

  /* Use proper values and structures. */
  if (bulk)
    {    
      count = GRUB_OHCI_BULK_EDS;
      ed = o->ed_bulk;
      ed_next = GRUB_OHCI_ED_PHYS2VIRT(o, bulk,
                  grub_le_to_cpu32 (ed->next_ed) );
    }
  else
    {
      count = GRUB_OHCI_CTRL_EDS;
      ed = o->ed_ctrl;
      ed_next = GRUB_OHCI_ED_PHYS2VIRT(o, bulk,
                  grub_le_to_cpu32 (ed->next_ed) );
    }

   /* First try to find existing ED with proper target address */
  for (i = 0; ; )
    {
      if (i && /* We ignore zero ED */
           ((ed->target & GRUB_OHCI_ED_ADDR_MASK) == target_addr))
        return ed; /* Found proper existing ED */
      i++;
      if (ed_next && (i < count))
        {
          ed = ed_next;
          ed_next = GRUB_OHCI_ED_PHYS2VIRT(o, bulk,
                      grub_le_to_cpu32 (ed->next_ed) );
          continue;
        }
      break;
    }
  /* ED with target_addr does not exist, we have to add it */
  /* Have we any free ED in array ? */
  if (i >= count) /* No. */
    return NULL;
  /* Currently we simply take next ED in array, no allocation
   * function is used. It should be no problem until hot-plugging
   * will be implemented, i.e. until we will need to de-allocate EDs
   * of unplugged devices. */
  /* We can link new ED to previous ED safely as the new ED should
   * still have set skip bit. */
  ed->next_ed = grub_cpu_to_le32 ( GRUB_OHCI_ED_VIRT2PHYS (o,
                                     bulk, &ed[1]));
  return &ed[1];
}

static grub_ohci_td_t
grub_ohci_alloc_td (struct grub_ohci *o)
{
  grub_ohci_td_t ret;

  /* Check if there is a Transfer Descriptor available.  */
  if (! o->td_free)
    return NULL;

  ret = o->td_free; /* Take current free TD */
  o->td_free = (grub_ohci_td_t)ret->link_td; /* Advance to next free TD in chain */
  ret->link_td = 0; /* Reset link_td in allocated TD */
  return ret;
}

static void
grub_ohci_free_td (struct grub_ohci *o, grub_ohci_td_t td)
{
  grub_memset ( (void*)td, 0, sizeof(struct grub_ohci_td) ); 
  td->link_td = (grub_uint32_t) o->td_free; /* Cahin new free TD & rest */
  o->td_free = td; /* Change address of first free TD */
}

static void
grub_ohci_free_tds (struct grub_ohci *o, grub_ohci_td_t td)
{
  if (!td)
    return;
    
  /* Unchain first TD from previous TD if it is chained */
  if (td->prev_td_phys)
    {
      grub_ohci_td_t td_prev_virt = GRUB_OHCI_TD_PHYS2VIRT(o,
                                      td->prev_td_phys);

      if (td == (grub_ohci_td_t) td_prev_virt->link_td)
        td_prev_virt->link_td = 0;
    }
  
  /* Free all TDs from td  (chained by link_td) */
  while (td)
    {
      grub_ohci_td_t tdprev;
      
      /* Unlink the queue.  */
      tdprev = td;
      td = (grub_ohci_td_t) td->link_td;

      /* Free the TD.  */
      grub_ohci_free_td (o, tdprev);
    }
}

static void
grub_ohci_transaction (grub_ohci_td_t td,
		       grub_transfer_type_t type, unsigned int toggle,
		       grub_size_t size, grub_uint32_t data)
{
  grub_uint32_t token;
  grub_uint32_t buffer;
  grub_uint32_t buffer_end;

  grub_dprintf ("ohci", "OHCI transaction td=%p type=%d, toggle=%d, size=%d\n",
		td, type, toggle, size);

  switch (type)
    {
    case GRUB_USB_TRANSFER_TYPE_SETUP:
      token = 0 << 19;
      break;
    case GRUB_USB_TRANSFER_TYPE_IN:
      token = 2 << 19;
      break;
    case GRUB_USB_TRANSFER_TYPE_OUT:
      token = 1 << 19;
      break;
    default:
      token = 0;
      break;
    }

  /* Set the token (Always generate interrupt - bits 21-23 = 0).  */
  token |= toggle << 24;
  token |= 1 << 25;

  /* Set "Not accessed" error code */
  token |= 15 << 28;

  buffer = data;
  buffer_end = buffer + size - 1;

  /* Set correct buffer values in TD if zero transfer occurs */
  if (size)
    {
      buffer = (grub_uint32_t) data;
      buffer_end = buffer + size - 1;
      td->buffer = grub_cpu_to_le32 (buffer);
      td->buffer_end = grub_cpu_to_le32 (buffer_end);
    }
  else 
    {
      td->buffer = 0;
      td->buffer_end = 0;
    }

  /* Set the rest of TD */
  td->token = grub_cpu_to_le32 (token);
  td->next_td = 0;
}

static grub_usb_err_t
grub_ohci_transfer (grub_usb_controller_t dev,
		    grub_usb_transfer_t transfer)
{
  struct grub_ohci *o = (struct grub_ohci *) dev->data;
  grub_ohci_ed_t ed_virt;
  int bulk = 0;
  grub_ohci_td_t td_head_virt;
  grub_ohci_td_t td_current_virt;
  grub_ohci_td_t td_next_virt;
  grub_ohci_td_t tderr_virt = NULL;
  grub_uint32_t target;
  grub_uint32_t td_head_phys;
  grub_uint32_t td_tail_phys;
  grub_uint32_t td_last_phys;
  grub_uint32_t tderr_phys = 0;
  grub_uint32_t status;
  grub_uint32_t control;
  grub_uint8_t errcode = 0;
  grub_usb_err_t err = GRUB_USB_ERR_NONE;
  int i;
  grub_uint64_t maxtime;
  grub_uint64_t bad_OHCI_delay = 0;
  int err_halt = 0;
  int err_timeout = 0;
  int err_unrec = 0;
  grub_uint32_t intstatus;

  /* Pre-set target for ED - we need it to find proper ED */
  /* Set the device address.  */
  target = transfer->devaddr;
  /* Set the endpoint. It should be masked, we need 4 bits only. */
  target |= (transfer->endpoint & 15) << 7;
  /* Set the device speed.  */
  target |= (transfer->dev->speed == GRUB_USB_SPEED_LOW) << 13;
  /* Set the maximum packet size.  */
  target |= transfer->max << 16;

  /* Determine if transfer type is bulk - we need to select proper ED */
  switch (transfer->type)
    {
      case GRUB_USB_TRANSACTION_TYPE_BULK:
        bulk = 1;
	break;

      case GRUB_USB_TRANSACTION_TYPE_CONTROL:
        break;

      default :
        return GRUB_USB_ERR_INTERNAL;
    }

  /* Find proper ED or add new ED */
  ed_virt = grub_ohci_find_ed (o, bulk, target);
  if (!ed_virt)
    {
      grub_dprintf ("ohci","Fatal: No free ED !\n");
      return GRUB_USB_ERR_INTERNAL;
    }
  
  /* Take pointer to first TD from ED */
  td_head_phys = grub_le_to_cpu32 (ed_virt->td_head) & ~0xf;
  td_tail_phys = grub_le_to_cpu32 (ed_virt->td_tail) & ~0xf;

  /* Sanity check - td_head should be equal to td_tail */
  if (td_head_phys != td_tail_phys) /* Should never happen ! */
    {
      grub_dprintf ("ohci", "Fatal: HEAD is not equal to TAIL !\n");
      grub_dprintf ("ohci", "HEAD = 0x%02x, TAIL = 0x%02x\n",
                    td_head_phys, td_tail_phys);
      /* XXX: Fix: What to do ? */
      return GRUB_USB_ERR_INTERNAL;
    }
  
  /* Now we should handle first TD. If ED is newly allocated,
   * we must allocate the first TD. */
  if (!td_head_phys)
    {
      td_head_virt = grub_ohci_alloc_td (o);
      if (!td_head_virt)
        return GRUB_USB_ERR_INTERNAL; /* We don't need de-allocate ED */
      /* We can set td_head only when ED is not active, i.e.
       * when it is newly allocated. */
      ed_virt->td_head = grub_cpu_to_le32 ( GRUB_OHCI_TD_VIRT2PHYS (o,
                                              td_head_virt) );
      ed_virt->td_tail = ed_virt->td_head;
    }
  else
    td_head_virt = GRUB_OHCI_TD_PHYS2VIRT ( o, td_head_phys );
    
  /* Set TDs */
  td_last_phys = td_head_phys; /* initial value to make compiler happy... */
  for (i = 0, td_current_virt = td_head_virt;
       i < transfer->transcnt; i++)
    {
      grub_usb_transaction_t tr = &transfer->transactions[i];

      grub_ohci_transaction (td_current_virt, tr->pid, tr->toggle,
			     tr->size, tr->data);

      /* Set index of TD in transfer */
      td_current_virt->tr_index = (grub_uint32_t) i;

      /* No IRQ request in TD if bad_OHCI set */
      if (o->bad_OHCI)
        td_current_virt->token |= grub_cpu_to_le32 ( 7 << 21);
      
      /* Remember last used (processed) TD phys. addr. */
      td_last_phys = GRUB_OHCI_TD_VIRT2PHYS (o, td_current_virt);
      
      /* Allocate next TD */
      td_next_virt = grub_ohci_alloc_td (o);
      if (!td_next_virt) /* No free TD, cancel transfer and free TDs except head TD */
        {
          if (i) /* if i==0 we have nothing to free... */
            grub_ohci_free_tds (o,
              GRUB_OHCI_TD_PHYS2VIRT(o,
                grub_le_to_cpu32 (td_head_virt->next_td) ) );
          /* Reset head TD */
          grub_memset ( (void*)td_head_virt, 0,
                        sizeof(struct grub_ohci_td) );
          grub_dprintf ("ohci", "Fatal: No free TD !");
          return GRUB_USB_ERR_INTERNAL;
        }

      /* Chain TDs */
      td_current_virt->link_td = (grub_uint32_t) td_next_virt;
      td_current_virt->next_td = grub_cpu_to_le32 (
                                   GRUB_OHCI_TD_VIRT2PHYS (o,
                                     td_next_virt) );
      td_next_virt->prev_td_phys = GRUB_OHCI_TD_VIRT2PHYS (o,
                                td_current_virt);
      td_current_virt = td_next_virt;
    }

  grub_dprintf ("ohci", "Tail TD (not processed) = %p\n",
                td_current_virt);
  
  /* Setup the Endpoint Descriptor for transfer.  */
  /* First set necessary fields in TARGET but keep (or set) skip bit */
  /* Note: It could be simpler if speed, format and max. packet
   * size never change after first allocation of ED.
   * But unfortunately max. packet size may change during initial
   * setup sequence and we must handle it. */
  ed_virt->target = grub_cpu_to_le32 (target | (1 << 14));
  /* Set td_tail */
  ed_virt->td_tail = grub_cpu_to_le32 ( GRUB_OHCI_TD_VIRT2PHYS (o,
                                          td_current_virt) );
  /* Now reset skip bit */
  ed_virt->target = grub_cpu_to_le32 (target);
  /* ed_virt->td_head = grub_cpu_to_le32 (td_head); Must not be changed, it is maintained by OHCI */
  /* ed_virt->next_ed = grub_cpu_to_le32 (0); Handled by grub_ohci_find_ed, do not change ! */

  grub_dprintf ("ohci", "program OHCI\n");

  /* Program the OHCI to actually transfer.  */
  switch (transfer->type)
    {
    case GRUB_USB_TRANSACTION_TYPE_BULK:
      {
	grub_dprintf ("ohci", "BULK list filled\n");
	/* Set BulkListFilled.  */
	grub_ohci_writereg32 (o, GRUB_OHCI_REG_CMDSTATUS, 1 << 2);
	/* Read back of register should ensure it is really written */
	grub_ohci_readreg32 (o, GRUB_OHCI_REG_CMDSTATUS);
	break;
      }

    case GRUB_USB_TRANSACTION_TYPE_CONTROL:
      {
	grub_dprintf ("ohci", "CONTROL list filled\n");
	/* Set ControlListFilled.  */
	grub_ohci_writereg32 (o, GRUB_OHCI_REG_CMDSTATUS, 1 << 1);
	/* Read back of register should ensure it is really written */
	grub_ohci_readreg32 (o, GRUB_OHCI_REG_CMDSTATUS);
	break;
      }
    }

  /* Safety measure to avoid a hang. */
  maxtime = grub_get_time_ms () + 1000;
	
  /* Wait until the transfer is completed or STALLs.  */
  do
    {
      /* Check transfer status */
      intstatus = grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);
      if (!o->bad_OHCI && (intstatus & 0x2) != 0)
        {
          /* Remember last successful TD */
          tderr_phys = grub_le_to_cpu32 (o->hcca->donehead) & ~0xf;
          /* Reset DoneHead */
	  o->hcca->donehead = 0;
          grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, (1 << 1));
          /* Read back of register should ensure it is really written */
          grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);
          /* if TD is last, finish */
          if (tderr_phys == td_last_phys)
            {
              if (grub_le_to_cpu32 (ed_virt->td_head) & 1)
                err_halt = 1;
              break;
            }
	  continue;
        }

      if ((intstatus & 0x10) != 0)
        { /* Unrecoverable error - only reset can help...! */
          err_unrec = 1;
          break;
        }

      /* Detected a HALT.  */
      if (err_halt || (grub_le_to_cpu32 (ed_virt->td_head) & 1))
        {
          err_halt = 1;
          /* ED is halted, but donehead event can happened in the meantime */
          intstatus = grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);
          if (!o->bad_OHCI && (intstatus & 0x2) != 0)
            /* Don't break loop now, first do donehead action(s) */
            continue;
          break;
        }

      /* bad OHCI handling */
      if ( (grub_le_to_cpu32 (ed_virt->td_head) & ~0xf) ==
           (grub_le_to_cpu32 (ed_virt->td_tail) & ~0xf) ) /* Empty ED */
        {
          if (o->bad_OHCI) /* Bad OHCI detected previously */
            {
              /* Try get last successful TD. */
              tderr_phys = grub_le_to_cpu32 (o->hcca->donehead) & ~0xf;
              if (tderr_phys)/* Reset DoneHead if we were successful */
                {
                  o->hcca->donehead = 0;
                  grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, (1 << 1));
                  /* Read back of register should ensure it is really written */
                  grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);
                }
              /* Check the HALT bit */
              if (grub_le_to_cpu32 (ed_virt->td_head) & 1)
                err_halt = 1;
              break;
            }
          else /* Detection of bad OHCI */
            /* We should wait short time (~2ms) before we say that
             * it is bad OHCI to prevent some hazard -
             * donehead can react in the meantime. This waiting is done
             * only once per OHCI driver "live cycle". */
            if (!bad_OHCI_delay) /* Set delay time */
              bad_OHCI_delay = grub_get_time_ms () + 2;
            else if (grub_get_time_ms () >= bad_OHCI_delay)
              o->bad_OHCI = 1;
          continue;
        }
        
      /* Timeout ? */
      if (grub_get_time_ms () > maxtime)
      	{
      	  err_timeout = 1;
      	  break;
      	}

      grub_cpu_idle ();
    }
  while (1);

  /* There are many ways how the loop above can finish:
   * - normally without any error via INTSTATUS WDH bit
   *   : tderr_phys == td_last_phys, td_head == td_tail
   * - normally with error via HALT bit in ED TD HEAD
   *   : td_head = next TD after TD with error
   *   : tderr_phys = last processed and retired TD with error,
   *     i.e. should be != 0
   *   : if bad_OHCI == TRUE, tderr_phys will be probably invalid
   * - unrecoverable error - I never seen it but it could be
   *   : err_unrec == TRUE, other values can contain anything...
   * - timeout, it can be caused by:
   *  -- bad USB device - some devices have some bugs, see Linux source
   *     and related links
   *  -- bad OHCI controller - e.g. lost interrupts or does not set
   *     proper bits in INTSTATUS when real IRQ not enabled etc.,
   *     see Linux source and related links
   *     One known bug is handled - if transfer finished
   *     successfully (i.e. HEAD==TAIL, last transfer TD is retired,
   *     HALT bit is not set) and WDH bit is not set in INTSTATUS - in
   *     this case we set o->bad_OHCI=TRUE and do alternate loop
   *     and error handling - but there is problem how to find retired
   *     TD with error code if HALT occurs and if DONEHEAD is not
   *     working - we need to find TD previous to current ED HEAD
   *  -- bad code of this driver or some unknown reasons - :-(
   *     it can be e.g. bad handling of EDs/TDs/toggle bit...
   */

  /* Remember target for debug and set skip flag in ED */
  /* It should be normaly not necessary but we need it at least
   * in case of timeout */
  target = grub_le_to_cpu32 ( ed_virt->target );
  ed_virt->target = grub_cpu_to_le32 (target | (1 << 14));
  /* Read registers for debug - they should be read now because
   * debug prints case unwanted delays, so something can happen
   * in the meantime... */
  control = grub_ohci_readreg32 (o, GRUB_OHCI_REG_CONTROL);
  status = grub_ohci_readreg32 (o, GRUB_OHCI_REG_CMDSTATUS);
  intstatus = grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);
  /* Now print debug values - to have full info what happened */
  grub_dprintf ("ohci", "loop finished: control=0x%02x status=0x%02x\n",
		control, status);
  grub_dprintf ("ohci", "intstatus=0x%02x \n\t\t tderr_phys=0x%02x, td_last_phys=0x%02x\n",
		intstatus, tderr_phys, td_last_phys);
  grub_dprintf ("ohci", "err_unrec=%d, err_timeout=%d \n\t\t err_halt=%d, bad_OHCI=%d\n",
		err_unrec, err_timeout, err_halt, o->bad_OHCI);
  grub_dprintf ("ohci", "TARGET=0x%02x, HEAD=0x%02x, TAIL=0x%02x\n",
                target,
                grub_le_to_cpu32 (ed_virt->td_head),
                grub_le_to_cpu32 (ed_virt->td_tail) );

  if (!err_halt && !err_unrec && !err_timeout) /* normal finish */
    {
      /* Simple workaround if donehead is not working */
      if (o->bad_OHCI &&
           ( !tderr_phys || (tderr_phys != td_last_phys) ) )
        {
          grub_dprintf ("ohci", "normal finish, but tderr_phys corrected\n");
          tderr_phys = td_last_phys;
          /* I hope we can do it as transfer (most probably) finished OK */
        }
      /* Prepare pointer to last processed TD */
      tderr_virt = GRUB_OHCI_TD_PHYS2VIRT (o, tderr_phys);
      /* Set index of last processed TD */
      if (tderr_virt)
        transfer->last_trans = tderr_virt->tr_index;
      else
        transfer->last_trans = -1;
    }

  else if (err_halt) /* error, ED is halted by OHCI, i.e. can be modified */
    {
      /* First we must get proper tderr_phys value */
      if (o->bad_OHCI) /* In case of bad_OHCI tderr_phys can be wrong */
        {
          if ( tderr_phys ) /* check if tderr_phys points to TD with error */
            errcode = grub_le_to_cpu32 ( GRUB_OHCI_TD_PHYS2VIRT (o,
                                           tderr_phys)->token )
                      >> 28;
          if ( !tderr_phys || !errcode ) /* tderr_phys not valid or points to wrong TD */
            { /* Retired TD with error should be previous TD to ED->td_head */
              tderr_phys = GRUB_OHCI_TD_PHYS2VIRT (o,
                             grub_le_to_cpu32 ( ed_virt->td_head) & ~0xf )
                           ->prev_td_phys;
            }
        }

      /* Even if we have "good" OHCI, in some cases
       * tderr_phys can be zero, check it */
      else if ( !tderr_phys )
        { /* Retired TD with error should be previous TD to ED->td_head */
          tderr_phys = GRUB_OHCI_TD_PHYS2VIRT (o,
                         grub_le_to_cpu32 ( ed_virt->td_head) & ~0xf )
                       ->prev_td_phys;
        }

      /* Prepare pointer to last processed TD and get error code */
      tderr_virt = GRUB_OHCI_TD_PHYS2VIRT (o, tderr_phys);
      /* Set index of last processed TD */
      if (tderr_virt)
        {
          errcode = grub_le_to_cpu32 ( tderr_virt->token ) >> 28;
          transfer->last_trans = tderr_virt->tr_index;
        }
      else
        transfer->last_trans = -1;

      /* Evaluation of error code */
      grub_dprintf ("ohci", "OHCI tderr_phys=0x%02x, errcode=0x%02x\n",
                    tderr_phys, errcode);
      switch (errcode)
	{
	case 0:
	  /* XXX: Should not happen!  */
	  grub_error (GRUB_ERR_IO, "OHCI without reporting the reason");
	  err = GRUB_USB_ERR_INTERNAL;
	  break;

	case 1:
	  /* XXX: CRC error.  */
	  err = GRUB_USB_ERR_TIMEOUT;
	  break;

	case 2:
	  err = GRUB_USB_ERR_BITSTUFF;
	  break;

	case 3:
	  /* XXX: Data Toggle error.  */
	  err = GRUB_USB_ERR_DATA;
	  break;

	case 4:
	  err = GRUB_USB_ERR_STALL;
	  break;

	case 5:
	  /* XXX: Not responding.  */
	  err = GRUB_USB_ERR_TIMEOUT;
	  break;

	case 6:
	  /* XXX: PID Check bits failed.  */
	  err = GRUB_USB_ERR_BABBLE;
	  break;

	case 7:
	  /* XXX: PID unexpected failed.  */
	  err = GRUB_USB_ERR_BABBLE;
	  break;

	case 8:
	  /* XXX: Data overrun error.  */
	  err = GRUB_USB_ERR_DATA;
	  grub_dprintf ("ohci", "Overrun, failed TD address: %p, index: %d\n",
	                tderr_virt, tderr_virt->tr_index);
	  break;

	case 9:
	  /* XXX: Data underrun error.  */
	  err = GRUB_USB_ERR_DATA;
	  grub_dprintf ("ohci", "Underrun, failed TD address: %p, index: %d\n",
	                tderr_virt, tderr_virt->tr_index);
	  grub_dprintf ("ohci", "Underrun, number of not transferred bytes: %d\n",
	                1 + grub_le_to_cpu32 (tderr_virt->buffer_end)
	                  - grub_le_to_cpu32 (tderr_virt->buffer));
	  break;

	case 10:
	  /* XXX: Reserved.  */
	  err = GRUB_USB_ERR_NAK;
	  break;

	case 11:
	  /* XXX: Reserved.  */
	  err = GRUB_USB_ERR_NAK;
	  break;

	case 12:
	  /* XXX: Buffer overrun.  */
	  err = GRUB_USB_ERR_DATA;
	  break;

	case 13:
	  /* XXX: Buffer underrun.  */
	  err = GRUB_USB_ERR_DATA;
	  break;

	default:
	  err = GRUB_USB_ERR_NAK;
	  break;
	}

    }
        
  else if (err_unrec)      
    {
      /* Don't try to get error code and last processed TD for proper
       * toggle bit value - anything can be invalid */
      err = GRUB_USB_ERR_UNRECOVERABLE;
      grub_dprintf("ohci", "Unrecoverable error!");

      /* Do OHCI reset in case of unrecoverable error - maybe we will need
       * do more - re-enumerate bus etc. (?) */

      /* Suspend the OHCI by issuing a reset.  */
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CMDSTATUS, 1); /* XXX: Magic.  */
      /* Read back of register should ensure it is really written */
      grub_ohci_readreg32 (o, GRUB_OHCI_REG_CMDSTATUS);
      grub_millisleep (1);
      grub_dprintf ("ohci", "Unrecoverable error - OHCI reset\n");

      /* Misc. resets. */
      o->hcca->donehead = 0;
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, 0x7f); /* Clears everything */
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLHEAD, o->ed_ctrl_addr);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLCURR, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKHEAD, o->ed_bulk_addr);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKCURR, 0);
      /* Read back of register should ensure it is really written */
      grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);

      /* Enable the OHCI.  */
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROL,
                            (2 << 6)
                            | GRUB_OHCI_REG_CONTROL_CONTROL_ENABLE
                            | GRUB_OHCI_REG_CONTROL_BULK_ENABLE );
    } 

  else if (err_timeout)
    {
      /* In case of timeout do not detect error from TD */    
      err = GRUB_ERR_TIMEOUT;
      grub_dprintf("ohci", "Timeout !\n");

      /* We should wait for next SOF to be sure that ED is unaccessed
       * by OHCI */
      /* SF bit reset. (SF bit indicates Start Of Frame (SOF) packet) */
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, (1<<2));
      /* Wait for new SOF */
      while ((grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS) & 0x4) == 0);

      /* Now we must find last processed TD if bad_OHCI == TRUE */
      if (o->bad_OHCI)
        { /* Retired TD with error should be previous TD to ED->td_head */
          tderr_phys = GRUB_OHCI_TD_PHYS2VIRT (o,
                         grub_le_to_cpu32 ( ed_virt->td_head) & ~0xf)
                       ->prev_td_phys;
        }
      tderr_virt = GRUB_OHCI_TD_PHYS2VIRT (o, tderr_phys);
      if (tderr_virt)
        transfer->last_trans = tderr_virt->tr_index;
      else
        transfer->last_trans = -1;

    }

    /* Set empty ED - set HEAD = TAIL = last (not processed) TD */
    ed_virt->td_head = grub_cpu_to_le32 ( grub_le_to_cpu32 (
                                            ed_virt->td_tail) & ~0xf); 

  /* At this point always should be:
   * ED has skip bit set and halted or empty or after next SOF,
   *    i.e. it is safe to free all TDs except last not processed
   * ED HEAD == TAIL == phys. addr. of td_current_virt */

  /* Reset DoneHead - sanity cleanup */
  o->hcca->donehead = 0;
  grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, (1 << 1));
  /* Read back of register should ensure it is really written */
  grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);

  /* Un-chainig of last TD */
  if (td_current_virt->prev_td_phys)
    {
      grub_ohci_td_t td_prev_virt
                = GRUB_OHCI_TD_PHYS2VIRT (o, td_current_virt->prev_td_phys);

      td_next_virt = (grub_ohci_td_t) td_prev_virt->link_td;
      if (td_current_virt == td_next_virt)
        td_prev_virt->link_td = 0;
    }

  grub_dprintf ("ohci", "OHCI finished, freeing, err=0x%02x, errcode=0x%02x\n",
                err, errcode);
  grub_ohci_free_tds (o, td_head_virt);

  return err;
}

static grub_err_t
grub_ohci_portstatus (grub_usb_controller_t dev,
		      unsigned int port, unsigned int enable)
{
   struct grub_ohci *o = (struct grub_ohci *) dev->data;
   grub_uint64_t endtime;

   grub_dprintf ("ohci", "begin of portstatus=0x%02x\n",
                 grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBPORT + port));

   if (!enable) /* We don't need reset port */
     {
       /* Disable the port and wait for it. */
       grub_ohci_writereg32 (o, GRUB_OHCI_REG_RHUBPORT + port,
                             GRUB_OHCI_CLEAR_PORT_ENABLE);
       endtime = grub_get_time_ms () + 1000;
       while ((grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBPORT + port)
               & (1 << 1)))
         if (grub_get_time_ms () > endtime)
           return grub_error (GRUB_ERR_IO, "OHCI Timed out - disable");

       grub_dprintf ("ohci", "end of portstatus=0x%02x\n",
         grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBPORT + port));
       return GRUB_ERR_NONE;
     }
     
   /* Reset the port */
   grub_ohci_writereg32 (o, GRUB_OHCI_REG_RHUBPORT + port,
			 GRUB_OHCI_SET_PORT_RESET);
   grub_millisleep (50); /* For root hub should be nominaly 50ms */
 
    /* End the reset signaling.  */
   grub_ohci_writereg32 (o, GRUB_OHCI_REG_RHUBPORT + port,
			 GRUB_OHCI_SET_PORT_RESET_STATUS_CHANGE);
   grub_millisleep (10);

   /* Enable the port and wait for it. */
   grub_ohci_writereg32 (o, GRUB_OHCI_REG_RHUBPORT + port,
                         GRUB_OHCI_SET_PORT_ENABLE);
   endtime = grub_get_time_ms () + 1000;
   while (! (grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBPORT + port)
           & (1 << 1)))
     if (grub_get_time_ms () > endtime)
       return grub_error (GRUB_ERR_IO, "OHCI Timed out - enable");

   grub_millisleep (10);

   /* Reset bit Connect Status Change */
   grub_ohci_writereg32 (o, GRUB_OHCI_REG_RHUBPORT + port,
                         GRUB_OHCI_RESET_CONNECT_CHANGE);

   grub_dprintf ("ohci", "end of portstatus=0x%02x\n",
		 grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBPORT + port));
 
   return GRUB_ERR_NONE;
}

static grub_usb_speed_t
grub_ohci_detect_dev (grub_usb_controller_t dev, int port, int *changed)
{
   struct grub_ohci *o = (struct grub_ohci *) dev->data;
   grub_uint32_t status;

   status = grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBPORT + port);

   grub_dprintf ("ohci", "detect_dev status=0x%02x\n", status);

  /* Connect Status Change bit - it detects change of connection */
  *changed = ((status & GRUB_OHCI_RESET_CONNECT_CHANGE) != 0);

   if (! (status & 1))
     return GRUB_USB_SPEED_NONE;
   else if (status & (1 << 9))
     return GRUB_USB_SPEED_LOW;
   else
     return GRUB_USB_SPEED_FULL;
}

static int
grub_ohci_hubports (grub_usb_controller_t dev)
{
  struct grub_ohci *o = (struct grub_ohci *) dev->data;
  grub_uint32_t portinfo;

  portinfo = grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBA);

  grub_dprintf ("ohci", "root hub ports=%d\n", portinfo & 0xFF);

  return portinfo & 0xFF;
}

static grub_err_t
grub_ohci_fini_hw (int noreturn __attribute__ ((unused)))
{
  struct grub_ohci *o;

  for (o = ohci; o; o = o->next)
    {
      int i, nports = grub_ohci_readreg32 (o, GRUB_OHCI_REG_RHUBA) & 0xff;
      grub_uint64_t maxtime;

      /* Set skip in all EDs */
      if (o->ed_bulk)
        for (i=0; i < GRUB_OHCI_BULK_EDS; i++)
          o->ed_bulk[i].target |= grub_cpu_to_le32 (1 << 14); /* skip */
      if (o->ed_ctrl)
        for (i=0; i < GRUB_OHCI_CTRL_EDS; i++)
          o->ed_ctrl[i].target |= grub_cpu_to_le32 (1 << 14); /* skip */

      /* We should wait for next SOF to be sure that all EDs are
       * unaccessed by OHCI. But OHCI can be non-functional, so
       * more than 1ms timeout have to be applied. */
      /* SF bit reset. (SF bit indicates Start Of Frame (SOF) packet) */
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, (1<<2));
      maxtime = grub_get_time_ms () + 2;
      /* Wait for new SOF or timeout */
      while ( ((grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS) & 0x4)
                 == 0) || (grub_get_time_ms () >= maxtime) );

      for (i = 0; i < nports; i++)
	grub_ohci_writereg32 (o, GRUB_OHCI_REG_RHUBPORT + i,
			      GRUB_OHCI_CLEAR_PORT_ENABLE);

      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CMDSTATUS, 1);
      grub_millisleep (1);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_HCCA, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLHEAD, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLCURR, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKHEAD, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKCURR, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_DONEHEAD, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROL, 0);
      /* Read back of register should ensure it is really written */
      grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);

#if 0 /* Is this necessary before booting? Probably not .(?)
       * But it must be done if module is removed ! (Or not ?)
       * How to do it ? - Probably grub_ohci_restore_hw should be more
       * complicated. (?)
       * (If we do it, we need to reallocate EDs and TDs in function
       * grub_ohci_restore_hw ! */

      /* Free allocated EDs and TDs */
      grub_dma_free (o->td_chunk);
      grub_dma_free (o->ed_bulk_chunk);
      grub_dma_free (o->ed_ctrl_chunk);
      grub_dma_free (o->hcca_chunk);
#endif
    }
  grub_millisleep (10);

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_ohci_restore_hw (void)
{
  struct grub_ohci *o;

  for (o = ohci; o; o = o->next)
    {
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_HCCA, o->hcca_addr);
      o->hcca->donehead = 0;
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_INTSTATUS, 0x7f); /* Clears everything */
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLHEAD, o->ed_ctrl_addr);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROLCURR, 0);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKHEAD, o->ed_bulk_addr);
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_BULKCURR, 0);
      /* Read back of register should ensure it is really written */
      grub_ohci_readreg32 (o, GRUB_OHCI_REG_INTSTATUS);

      /* Enable the OHCI.  */
      grub_ohci_writereg32 (o, GRUB_OHCI_REG_CONTROL,
                            (2 << 6)
                            | GRUB_OHCI_REG_CONTROL_CONTROL_ENABLE
                            | GRUB_OHCI_REG_CONTROL_BULK_ENABLE );
    }

  return GRUB_ERR_NONE;
}


static struct grub_usb_controller_dev usb_controller =
{
  .name = "ohci",
  .iterate = grub_ohci_iterate,
  .transfer = grub_ohci_transfer,
  .hubports = grub_ohci_hubports,
  .portstatus = grub_ohci_portstatus,
  .detect_dev = grub_ohci_detect_dev
};

GRUB_MOD_INIT(ohci)
{
  grub_ohci_inithw ();
  grub_usb_controller_dev_register (&usb_controller);
  grub_loader_register_preboot_hook (grub_ohci_fini_hw, grub_ohci_restore_hw,
				     GRUB_LOADER_PREBOOT_HOOK_PRIO_DISK);
}

GRUB_MOD_FINI(ohci)
{
  grub_ohci_fini_hw (0);
  grub_usb_controller_dev_unregister (&usb_controller);
}
