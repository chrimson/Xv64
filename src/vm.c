#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
struct segdesc gdt[NSEGS];

// Chris Limson 2022-05-12
#ifndef x64
// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpunum()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);

  // Map cpu, and curproc
  c->gdt[SEG_KCPU] = SEG(STA_W, &c->cpu, 8, 0);

  lgdt(c->gdt, sizeof(c->gdt));
  loadgs(SEG_KCPU << 3);
  
  // Initialize cpu-local storage.
  cpu = c;
  proc = 0;
}
#else
__thread struct cpu *cpu;
__thread struct proc *proc;

static pde_t *kpml4;
static pde_t *kpdpt;
static pde_t *iopgdir;
static pde_t *kpgdir0;
static pde_t *kpgdir1;

void wrmsr(uint msr, uint64 val);

void tvinit(void) {}
void idtinit(void) {}

static void mkgate(uint *idt, uint n, void *kva, uint pl, uint trap) {
  uint64 addr = (uint64) kva;
  n *= 4;
  trap = trap ? 0x8F00 : 0x8E00; // TRAP vs INTERRUPT gate;
  idt[n+0] = (addr & 0xFFFF) | ((SEG_KCODE << 3) << 16);
  idt[n+1] = (addr & 0xFFFF0000) | trap | ((pl & 3) << 13); // P=1 DPL=pl
  idt[n+2] = addr >> 32;
  idt[n+3] = 0;
}

static void tss_set_rsp(uint *tss, uint n, uint64 rsp) {
  tss[n*2 + 1] = rsp;
  tss[n*2 + 2] = rsp >> 32;
}

extern void* vectors[];

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  uint64 *gdt;
  uint *tss;
  uint64 addr;
  void *local;
  struct cpu *c;
  uint *idt = (uint*) kalloc();
  int n;
  memset(idt, 0, PGSIZE);

  for (n = 0; n < 256; n++)
    mkgate(idt, n, vectors[n], 0, 0);
  mkgate(idt, 64, vectors[64], 3, 1);

  lidt((void*) idt, PGSIZE);

  // create a page for cpu local storage 
  local = kalloc();
  memset(local, 0, PGSIZE);

  gdt = (uint64*) local;
  tss = (uint*) (((char*) local) + 1024);
  tss[16] = 0x00680000; // IO Map Base = End of TSS

  // point FS smack in the middle of our local storage page
  wrmsr(0xC0000100, ((uint64) local) + (PGSIZE / 2));

  c = &cpus[cpunum()];
  c->local = local;

  cpu = c;
  proc = 0;

  addr = (uint64) tss;
  gdt[0] =         0x0000000000000000;
  gdt[SEG_KCODE] = 0x0020980000000000;  // Code, DPL=0, R/X
  gdt[SEG_UCODE] = 0x0020F80000000000;  // Code, DPL=3, R/X
  gdt[SEG_KDATA] = 0x0000920000000000;  // Data, DPL=0, W
  gdt[SEG_KCPU]  = 0x0000000000000000;  // unused
  gdt[SEG_UDATA] = 0x0000F20000000000;  // Data, DPL=3, W
  gdt[SEG_TSS+0] = (0x0067) | ((addr & 0xFFFFFF) << 16) |
                   (0x00E9LL << 40) | (((addr >> 24) & 0xFF) << 56);
  gdt[SEG_TSS+1] = (addr >> 32);

  lgdt((void*) gdt, 8 * sizeof(uint64));

  ltr(SEG_TSS << 3);
};
#endif

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)p2v(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table 
    // entries, if necessary.
    *pde = v2p(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uintp size, uintp pa, int perm)
{
  char *a, *last;
  pte_t *pte;
  
  a = (char*)PGROUNDDOWN((uintp)va);
  last = (char*)PGROUNDDOWN(((uintp)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Chris Limson 2022-05-12
#ifndef x64
static struct kmap {
  void *virt;
  uintp phys_start;
  uintp phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVBASE,  DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (p2v(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start, 
                (uint)k->phys_start, k->perm) < 0)
      return 0;
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(v2p(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  pushcli();
  cpu->gdt[SEG_TSS] = SEG16(STS_T32A, &cpu->ts, sizeof(cpu->ts)-1, 0);
  cpu->gdt[SEG_TSS].s = 0;
  cpu->ts.ss0 = SEG_KDATA << 3;
  cpu->ts.esp0 = (uintp)proc->kstack + KSTACKSIZE;
  ltr(SEG_TSS << 3);
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");
  lcr3(v2p(p->pgdir));  // switch to new address space
  popcli();
}
#else
pde_t*
setupkvm(void)
{
  pde_t *pml4 = (pde_t*) kalloc();
  pde_t *pdpt = (pde_t*) kalloc();
  pde_t *pgdir = (pde_t*) kalloc();

  memset(pml4, 0, PGSIZE);
  memset(pdpt, 0, PGSIZE);
  memset(pgdir, 0, PGSIZE);
  pml4[511] = v2p(kpdpt) | PTE_P | PTE_W | PTE_U;
  pml4[0] = v2p(pdpt) | PTE_P | PTE_W | PTE_U;
  pdpt[0] = v2p(pgdir) | PTE_P | PTE_W | PTE_U; 

  // virtual backpointers
  pgdir[511] = ((uintp) pml4) | PTE_P;
  pgdir[510] = ((uintp) pdpt) | PTE_P;

  return pgdir;
};

void
kvmalloc(void)
{
  int n;
  kpml4 = (pde_t*) kalloc();
  kpdpt = (pde_t*) kalloc();
  kpgdir0 = (pde_t*) kalloc();
  kpgdir1 = (pde_t*) kalloc();
  iopgdir = (pde_t*) kalloc();
  memset(kpml4, 0, PGSIZE);
  memset(kpdpt, 0, PGSIZE);
  memset(iopgdir, 0, PGSIZE);
  kpml4[511] = v2p(kpdpt) | PTE_P | PTE_W;
  kpdpt[511] = v2p(kpgdir1) | PTE_P | PTE_W;
  kpdpt[510] = v2p(kpgdir0) | PTE_P | PTE_W;
  kpdpt[509] = v2p(iopgdir) | PTE_P | PTE_W;
  for (n = 0; n < NPDENTRIES; n++) {
    kpgdir0[n] = (n << PDXSHIFT) | PTE_PS | PTE_P | PTE_W;
    kpgdir1[n] = ((n + 512) << PDXSHIFT) | PTE_PS | PTE_P | PTE_W;
  }
  for (n = 0; n < 16; n++)
    iopgdir[n] = (DEVSPACE + (n << PDXSHIFT)) | PTE_PS | PTE_P | PTE_W | PTE_PWT | PTE_PCD;
  switchkvm();
}

void
switchkvm(void)
{
  lcr3(v2p(kpml4));
}

void
switchuvm(struct proc *p)
{
  void *pml4;
  uint *tss;
  pushcli();
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");
  tss = (uint*) (((char*) cpu->local) + 1024);
  tss_set_rsp(tss, 0, (uintp)proc->kstack + KSTACKSIZE);
  pml4 = (void*) PTE_ADDR(p->pgdir[511]);
  lcr3(v2p(pml4));
  popcli();
}
#endif

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;
  
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, v2p(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uintp) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, p2v(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uintp a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    mappages(pgdir, (char*)a, PGSIZE, v2p(mem), PTE_W|PTE_U);
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uintp oldsz, uintp newsz)
{
  pte_t *pte;
  uintp a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a += (NPTENTRIES - 1) * PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = p2v(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;
  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, 0x3fa00000, 0);
  for(i = 0; i < NPDENTRIES-2; i++){
    if(pgdir[i] & PTE_P){
      char * v = p2v(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uintp pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)p2v(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, v2p(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d);
  return 0;
}


// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)p2v(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uintp n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}
