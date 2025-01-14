#include "ata.h"

#include <arch.h>
#include <cpu.h>
#include <dev/driver.h>
#include <dev/mbr.h>
#include <lock.h>
#include <mem.h>
#include <module.h>
#include <pci.h>
#include <phys.h>
#include <printf.h>
#include <ck/ptr.h>
#include <sched.h>
#include <time.h>
#include <util.h>
#include <vga.h>
#include <wait.h>

#include "../majors.h"


class ATADriver : public dev::Driver {
 public:
  ATADriver() { set_name("ata"); }
};

static ck::ref<ATADriver> ata_driver = nullptr;

// #define DEBUG
// #define DO_TRACE

#ifdef CONFIG_ATA_DEBUG
#define INFO(fmt, args...) KINFO("ATA: " fmt, ##args)
#else
#define INFO(fmt, args...)
#endif

#ifdef DO_TRACE
#define TRACE printf("[ATA]: (%d) %s\n", __LINE__, __PRETTY_FUNCTION__)
#else
#define TRACE
#endif

#define ATA_IRQ0 (14)
#define ATA_IRQ1 (15)
#define ATA_IRQ2 (11)
#define ATA_IRQ3 (9)

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_WRITE_DMA 0xCA

#define ATA_REG_DATA 0x00
#define ATA_REG_ERROR 0x01
#define ATA_REG_FEATURES 0x01
#define ATA_REG_SECCOUNT0 0x02
#define ATA_REG_LBA0 0x03
#define ATA_REG_LBA1 0x04
#define ATA_REG_LBA2 0x05
#define ATA_REG_HDDEVSEL 0x06
#define ATA_REG_COMMAND 0x07
#define ATA_REG_STATUS 0x07
#define ATA_REG_SECCOUNT1 0x08
#define ATA_REG_LBA3 0x09
#define ATA_REG_LBA4 0x0A
#define ATA_REG_LBA5 0x0B
#define ATA_REG_CONTROL 0x0C
#define ATA_REG_ALTSTATUS 0x0C
#define ATA_REG_DEVADDRESS 0x0D

// Bus Master Reg Command
#define BMR_COMMAND_DMA_START 0x1
#define BMR_COMMAND_DMA_STOP 0x0
#define BMR_COMMAND_READ 0x8
#define BMR_STATUS_INT 0x4
#define BMR_STATUS_ERR 0x2

// static int ata_dev_init(fs::blkdev& d);
// static int ata_rw_block(fs::blkdev& b, void* data, int block, bool write);
struct wait_queue ata_wq;

/**
 * TODO: use per-channel ATA mutex locks. Right now every ata drive is locked
 * the same way
 */
static spinlock drive_lock;


// for the interrupts...
u16 primary_master_status = 0;
u16 primary_master_bmr_status = 0;
u16 primary_master_bmr_command = 0;

dev::ATADisk::ATADisk(u16 portbase, bool master) : dev::Disk(*ata_driver) {
  drive_lock.lock();
  m_io_base = portbase;
  TRACE;
  sector_size = 512;
  set_block_size(sector_size);
  this->master = master;

  data_port = portbase;
  error_port = portbase + 1;
  sector_count_port = portbase + 2;
  lba_low_port = portbase + 3;
  lba_mid_port = portbase + 4;
  lba_high_port = portbase + 5;
  device_port = portbase + 6;
  command_port = portbase + 7;
  control_port = portbase + 0x206;

  drive_lock.unlock();
}



dev::ATADisk::~ATADisk() {
  drive_lock.lock();
  TRACE;
  free(id_buf);
  drive_lock.unlock();
}

void dev::ATADisk::select_device() {
  TRACE;
  device_port.out(master ? 0xA0 : 0xB0);
}



bool dev::ATADisk::identify() {
  scoped_lock l(drive_lock);

  // select the correct device
  select_device();
  // clear the HOB bit, idk what that is.
  control_port.out(0);


  device_port.out(0xA0);
  uint8_t status = command_port.in();


  INFO("Status: %x\n", status);
  // not valid, no device on that bus
  if (status == 0xFF) {
    return false;
  }

  select_device();
  sector_count_port.out(0);
  lba_low_port.out(0);
  lba_mid_port.out(0);
  lba_high_port.out(0);

  // command for identify
  command_port.out(0xEC);

  status = command_port.in();
  if (status == 0x00) {
    return false;
  }

  // read until the device is ready
  while (((status & 0x80) == 0x80) && ((status & 0x01) != 0x01)) {
    status = command_port.in();
  }

  if (status & 0x01) {
    printf(KERN_ERROR "error identifying ATA drive. status=%02x\n", status);
    return false;
  }

  id_buf = (u16*)malloc(sizeof(u16) * 256);
  for (u16 i = 0; i < 256; i++)
    id_buf[i] = data_port.in();

  uint8_t C = id_buf[1];
  uint8_t H = id_buf[3];
  uint8_t S = id_buf[6];

  n_sectors = (C * H) * S;
  set_block_count(n_sectors);

  set_size(block_count() * block_size());


  m_pci_dev = pci::find_generic_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE);
#ifdef CONFIG_ATA_DMA

  if (m_pci_dev != nullptr) {
    m_pci_dev->enable_bus_mastering();

    use_dma = true;
    // bar4 contains information for DMA
    bar4 = m_pci_dev->get_bar(4).raw;
    if (bar4 & 0x1) bar4 = bar4 & 0xfffffffc;

    bmr_command = bar4;
    bmr_status = bar4 + 2;
    bmr_prdt = bar4 + 4;

    if (this->master && data_port.m_port == 0x1F0) {
      // printf("PRIMARY MASTER\n");
      primary_master_status = m_io_base;
      primary_master_bmr_status = bmr_status;
      primary_master_bmr_command = bmr_command;
    }
  } else {
    printf("can't use ata without DMA\n");
    return false;
  }
#endif


  return true;
}

int dev::ATADisk::read_blocks(uint32_t sector, void* data, int n) {
  TRACE;

  // TODO: also check for scheduler avail
  if (use_dma) {
    return read_blocks_dma(sector, data, n);
  }

  // take a scoped lock
  scoped_lock lck(drive_lock);

  // printf("read block %d\n", sector);

  if (sector & 0xF0000000) return -EINVAL;


  // select the correct device, and put bits of the address
  device_port.out((master ? 0xE0 : 0xF0) | ((sector & 0x0F000000) >> 24));
  error_port.out(0);
  // read a single sector
  sector_count_port.out(n);

  lba_low_port.out((sector & 0x00FF));
  lba_mid_port.out((sector & 0xFF00) >> 8);
  lba_high_port.out((sector & 0xFF0000) >> 16);

  // read command
  command_port.out(0x20);

  uint8_t status = wait();
  if (status & 0x1) {
    printf(KERN_ERROR "error reading ATA drive\n");
    return false;
  }

  auto* buf = (char*)data;

  for (u16 i = 0; i < sector_size * n; i += 2) {
    u16 d = data_port.in();

    buf[i] = d & 0xFF;
    buf[i + 1] = (d >> 8) & 0xFF;
  }

  return 0;
}

int dev::ATADisk::write_blocks(uint32_t sector, const void* vbuf, int n) {
  TRACE;

  // TODO: also check for scheduler avail
  if (use_dma) {
    return write_blocks_dma(sector, vbuf, n);
  }

  uint8_t* buf = (uint8_t*)vbuf;

  // TODO: use DMA here :^)
  scoped_lock lck(drive_lock);


  if (sector & 0xF0000000) return -EINVAL;

  // select the correct device, and put bits of the address
  device_port.out((master ? 0xE0 : 0xF0) | ((sector & 0x0F000000) >> 24));
  error_port.out(0);
  // write a single sector
  sector_count_port.out(n);

  lba_low_port.out((sector & 0x00FF));
  lba_mid_port.out((sector & 0xFF00) >> 8);
  lba_high_port.out((sector & 0xFF0000) >> 16);

  // write command
  command_port.out(0x30);

  for (u16 i = 0; i < sector_size * n; i += 2) {
    u16 d = buf[i];
    d |= ((u16)buf[i + 1]) << 8;
    data_port.out(d);
  }

  flush();

  return 0;
}

bool dev::ATADisk::flush(void) {
  TRACE;
  device_port.out(master ? 0xE0 : 0xF0);
  command_port.out(0xE7);

  // TODO: schedule out while waiting?
  uint8_t status = command_port.in();
  while (((status & 0x80) == 0x80) && ((status & 0x01) != 0x01)) {
    status = command_port.in();
  }
  if (status & 0x1) {
    printf(KERN_ERROR "error flushing ATA drive\n");
    return false;
  }
  return true;
}

uint8_t dev::ATADisk::wait(void) {
  TRACE;

  if (cpu::in_thread() && false) {
    // wait, but don't allow rude awakening
    ata_wq.wait_noint();
    return 0;
  } else {
    // TODO: schedule out while waiting?
    uint8_t status = command_port.in();
    while (((status & 0x80) == 0x80) && ((status & 0x01) != 0x01)) {
      status = command_port.in();
    }

    return status;
  }
  return -1;
}

u64 dev::ATADisk::sector_count(void) {
  TRACE;
  return n_sectors;
}




bool dev::ATADisk::read_blocks_dma(uint32_t sector, void* data, int n) {
  TRACE;

  if (sector & 0xF0000000) return false;


  scoped_lock lck(drive_lock);


  int buffer_pages = NPAGES(n * block_size() + sizeof(prdt_t));
  auto buffer = phys::alloc(buffer_pages);
  // setup the prdt for DMA
  auto* prdt = static_cast<prdt_t*>(p2v(buffer));
  prdt->transfer_size = sector_size;
  prdt->buffer_phys = (u64)buffer + sizeof(prdt_t);
  prdt->mark_end = 0x8000;

  uint8_t* dma_dst = (uint8_t*)p2v(prdt->buffer_phys);

  // stop bus master
  outb(bmr_command, 0);
  // Set prdt
  outl(bmr_prdt, (u64)buffer);

  // Turn on "Interrupt" and "Error" flag. The error flag should be cleared by
  // hardware.
  outb(bmr_status, inb(bmr_status) | 0x6);

  // set transfer direction
  outb(bmr_command, 0x8);

  // select the correct device, and put bits of the address
  device_port.out((master ? 0xE0 : 0xF0) | ((sector & 0x0F000000) >> 24));
  // clear the error port
  error_port.out(0);
  // read a single sector
  sector_count_port.out(n);



  lba_low_port.out((sector & 0x00FF));
  lba_mid_port.out((sector & 0xFF00) >> 8);
  lba_high_port.out((sector & 0xFF0000) >> 16);

  // read DMA command
  command_port.out(0xC8);

  // start bus master
  outb(bmr_command, 0x9);

  int i = 0;

  while (1) {
    i++;
    auto status = inb(bmr_status);
    auto dstatus = command_port.in();
    if (!(status & 0x04)) {
      continue;
    }
    if (!(dstatus & 0x80)) {
      break;
    }
  }

  memcpy(data, dma_dst, sector_size * n);
  phys::free(buffer, buffer_pages);
  return true;
}
bool dev::ATADisk::write_blocks_dma(uint32_t sector, const void* data, int n) {
  if (sector & 0xF0000000) return false;
  drive_lock.lock();

  int buffer_pages = NPAGES(n * block_size() + sizeof(prdt_t));
  auto buffer = phys::alloc(buffer_pages);
  // setup the prdt for DMA
  auto* prdt = static_cast<prdt_t*>(p2v(buffer));
  prdt->transfer_size = sector_size * n;
  prdt->buffer_phys = (u64)buffer + sizeof(prdt_t);
  prdt->mark_end = 0x8000;

  uint8_t* dma_dst = (uint8_t*)p2v(prdt->buffer_phys);

  // copy our data
  memcpy(dma_dst, data, sector_size * n);

  // stop bus master
  outb(bmr_command, 0);
  // Set prdt
  outl(bmr_prdt, (u64)buffer);

  // Turn on "Interrupt" and "Error" flag. The error flag should be cleared by
  // hardware.
  outb(bmr_status, inb(bmr_status) | 0x6);

  // set transfer direction
  outb(bmr_command, 0x8);

  // select the correct device, and put bits of the address
  device_port.out((master ? 0xE0 : 0xF0) | ((sector & 0x0F000000) >> 24));
  // clear the error port
  error_port.out(0);

  // read a single sector
  sector_count_port.out(n);

  lba_low_port.out((sector & 0x00FF));
  lba_mid_port.out((sector & 0xFF00) >> 8);
  lba_high_port.out((sector & 0xFF0000) >> 16);

  // start bus master
  outb(bmr_command, 0x9);

  // write DMA command
  command_port.out(ATA_CMD_WRITE_DMA);

  int i = 0;

  while (1) {
    i++;
    auto status = inb(bmr_status);
    auto dstatus = command_port.in();
    if (!(status & 0x04)) {
      continue;
    }
    if (!(dstatus & 0x80)) {
      break;
    }
  }
  phys::free(buffer, buffer_pages);

  drive_lock.unlock();


  return true;
}




static void ata_interrupt(int intr, reg_t* fr, void*) {
  inb(primary_master_status);
  inb(primary_master_bmr_status);

  outb(primary_master_bmr_status, BMR_COMMAND_DMA_STOP);


  irq::eoi(intr);
  // INFO("interrupt: err=%d\n", fr->err);
}


static void query_and_add_drive(u16 addr, int id, bool master) {
  printf(KERN_DEBUG "ATA Query %04x:%d\n", addr, id);
  auto drive = new dev::ATADisk(addr, master);

  if (drive->identify()) {
    dev::register_disk(drive);
  }
}
static void ata_initialize(void) {}




extern void piix_init(void);


static void ata_init(void) {
  ata_driver = ck::make_ref<ATADriver>();
  dev::Driver::add(ata_driver);
  // TODO: make a new IRQ dispatch system to make this more general
  irq::install(ATA_IRQ0, ata_interrupt, "ATA Drive");
  // smp::ioapicenable(ATA_IRQ0, 0);

  irq::install(ATA_IRQ1, ata_interrupt, "ATA Drive");
  // smp::ioapicenable(ATA_IRQ1, 0);

  // register all the ata drives on the system
  query_and_add_drive(0x1F0, 0, true);
  query_and_add_drive(0x1F0, 1, false);
}

module_init("ata", ata_init);
