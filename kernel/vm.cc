#include "types.h"
#include "amd64.h"
#include "mmu.h"
#include "cpu.hh"
#include "kernel.hh"
#include "bits.hh"
#include "spinlock.h"
#include "kalloc.hh"
#include "queue.h"
#include "condvar.h"
#include "proc.hh"
#include "vm.hh"
#include "gc.hh"
#include "crange.hh"
#include "cpputil.hh"
#include "sperf.hh"

enum { vm_debug = 0 };

/*
 * vmnode
 */

vmnode::vmnode(u64 npg, vmntype ntype, inode *i, u64 off, u64 s)
  : npages(npg), ref(0), type(ntype), ip(i), offset(off), sz(s)
{
  if (npg > NELEM(page))
    panic("vmnode too big\n");
  memset(page, 0, npg * sizeof(page[0]));
  if (type == EAGER && ip) {
    assert(allocpg() == 0);
    assert(demand_load() == 0);
  }
}

vmnode::~vmnode()
{
  for(u64 i = 0; i < npages; i++)
    if (page[i])
      kfree(page[i]);
  if (ip)
    iput(ip);
}

void
vmnode::decref()
{
  if(--ref == 0)
    delete this;
}

int
vmnode::allocpg()
{
  for(u64 i = 0; i < npages; i++) {
    if (page[i])
      continue;

    char *p = kalloc();
    if (!p) {
      cprintf("allocpg: out of memory, leaving half-filled vmnode\n");
      return -1;
    }

    memset(p, 0, PGSIZE);
    if(!cmpxch(&page[i], (char*) 0, p))
      kfree(p);
  }
  return 0;
}

vmnode *
vmnode::copy()
{
  vmnode *c = new vmnode(npages, type,
                         (type==ONDEMAND) ? idup(ip) : 0,
                         offset, sz);
  if(c == 0)
    return 0;

  if (!page[0])   // If first page is absent, all pages are absent
    return c;

  if (c->allocpg() < 0) {
    cprintf("vmn_copy: out of memory\n");
    delete c;
    return 0;
  }
  for(u64 i = 0; i < npages; i++)
    if (page[i])
      memmove(c->page[i], page[i], PGSIZE);

  return c;
}

int
vmnode::demand_load()
{
  for (u64 i = 0; i < sz; i += PGSIZE) {
    char *p = page[i / PGSIZE];
    s64 n;
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;

    /*
     * Possible race condition with concurrent demand_load() calls,
     * if the underlying inode's contents change..
     */
    if (readi(ip, p, offset+i, n) != n)
      return -1;
  }
  return 0;
}

/*
 * vma
 */

vma::vma(vmap *vmap, uptr start, uptr end, enum vmatype vtype, vmnode *vmn)
  : range(&vmap->cr, start, end-start),
    vma_start(start), vma_end(end), va_type(vtype), n(vmn)
{
  if (n)
    n->ref++;
}

vma::~vma()
{
  if (n)
    n->decref();
}

/*
 * vmap
 */

vmap::vmap()
  : cr(10), ref(1), pml4(setupkvm()), kshared((char*) ksalloc(slab_kshared))
{
  if (pml4 == 0) {
    cprintf("vmap_alloc: setupkvm out of memory\n");
    goto err;
  }

  if (kshared == NULL) {
    cprintf("vmap::vmap: kshared out of memory\n");
    goto err;
  }

  if (setupkshared(pml4, kshared)) {
    cprintf("vmap::vmap: setupkshared out of memory\n");
    goto err;
  }

  return;

 err:
  if (kshared)
    ksfree(slab_kshared, kshared);
  if (pml4)
    freevm(pml4);
}

vmap::~vmap()
{
  if (kshared)
    ksfree(slab_kshared, kshared);
  if (pml4)
    freevm(pml4);
}

void
vmap::decref()
{
  if (--ref == 0)
    delete this;
}

bool
vmap::replace_vma(vma *a, vma *b)
{
  auto span = cr.search_lock(a->vma_start, a->vma_end - a->vma_start);
  if (a->deleted())
    return false;
  for (auto e: span)
    assert(a == e);
  span.replace(b);
  return true;
}

vmap*
vmap::copy(int share)
{
  vmap *nm = new vmap();
  if(nm == 0)
    return 0;

  for (range *r: cr) {
    vma *e = (vma *) r;

    struct vma *ne;
    if (share) {
      ne = new vma(nm, e->vma_start, e->vma_end, COW, e->n);

      // if the original vma wasn't COW, replace it with a COW vma
      if (e->va_type != COW) {
        vma *repl = new vma(this, e->vma_start, e->vma_end, COW, e->n);
        replace_vma(e, repl);
        updatepages(pml4, e->vma_start, e->vma_end, [](atomic<pme_t>* p) {
            for (;;) {
              pme_t v = p->load();
              if (v & PTE_LOCK)
                continue;
              if (!(v & PTE_P) || !(v & PTE_U) || !(v & PTE_W) ||
                  cmpxch(p, v, PTE_ADDR(v) | PTE_P | PTE_U | PTE_COW))
                break;
            }
          });
      }
    } else {
      ne = new vma(nm, e->vma_start, e->vma_end, PRIVATE, e->n->copy());
    }

    if (ne == 0)
      goto err;

    if (ne->n == 0) {
      delete ne;
      goto err;
    }

    auto span = nm->cr.search_lock(ne->vma_start, ne->vma_end - ne->vma_start);
    for (auto x __attribute__((unused)): span)
      assert(0);  /* span must be empty */
    span.replace(ne);
  }

  if (share)
    tlbflush();  // Reload hardware page table

  return nm;

 err:
  delete nm;
  return 0;
}

// Does any vma overlap start..start+len?
// If yes, return the vma pointer.
// If no, return 0.
// This code can't handle regions at the very end
// of the address space, e.g. 0xffffffff..0x0
// We key vma's by their end address.
vma *
vmap::lookup(uptr start, uptr len)
{
  if (start + len < start)
    panic("vmap::lookup bad len");

  range *r = cr.search(start, len);
  if (r != 0) {
    vma *e = (vma *) r;
    if (e->vma_end <= e->vma_start)
      panic("malformed va");
    if (e->vma_start < start+len && e->vma_end > start)
      return e;
  }

  return 0;
}

int
vmap::insert(vmnode *n, uptr vma_start, int dotlb)
{
  ANON_REGION("vmap::insert", &perfgroup);

  vma *e;

  {
    // new scope to release the search lock before tlbflush
    u64 len = n->npages * PGSIZE;
    auto span = cr.search_lock(vma_start, len);
    for (auto r: span) {
      vma *rvma = (vma*) r;
      cprintf("vmap::insert: overlap with 0x%lx--0x%lx\n", rvma->vma_start, rvma->vma_end);
      return -1;
    }

    // XXX handle overlaps

    e = new vma(this, vma_start, vma_start+len, PRIVATE, n);
    if (e == 0) {
      cprintf("vmap::insert: out of vmas\n");
      return -1;
    }

    span.replace(e);
  }

  bool needtlb = false;
  updatepages(pml4, e->vma_start, e->vma_end, [&needtlb](atomic<pme_t> *p) {
      for (;;) {
        pme_t v = p->load();
        if (v & PTE_LOCK)
          continue;
        if (cmpxch(p, v, (pme_t) 0))
          break;
        if (v != 0)
          needtlb = true;
      }
    });
  if (needtlb && dotlb)
    tlbflush();
  return 0;
}

int
vmap::remove(uptr vma_start, uptr len)
{
  {
    // new scope to release the search lock before tlbflush
    uptr vma_end = vma_start + len;

    auto span = cr.search_lock(vma_start, len);
    for (auto r: span) {
      vma *rvma = (vma*) r;
      if (rvma->vma_start < vma_start || rvma->vma_end > vma_end) {
        cprintf("vmap::remove: partial unmap not supported\n");
        return -1;
      }
    }

    // XXX handle partial unmap

    span.replace(0);
  }

  bool needtlb = false;
  updatepages(pml4, vma_start, vma_start + len, [&needtlb](atomic<pme_t> *p) {
      for (;;) {
        pme_t v = p->load();
        if (v & PTE_LOCK)
          continue;
        if (cmpxch(p, v, (pme_t) 0))
          break;
        if (v != 0)
          needtlb = true;
      }
    });
  if (needtlb)
    tlbflush();
  return 0;
}

/*
 * pagefault handling code on vmap
 */

int
vmap::pagefault_wcow(vma *m)
{
  // Always make a copy of n, even if this process has the only ref, 
  // because other processes may change ref count while this process 
  // is handling wcow.
  struct vmnode *nodecopy = m->n->copy();
  if (nodecopy == 0) {
    cprintf("pagefault_wcow: out of mem\n");
    return -1;
  }

  vma *repl = new vma(this, m->vma_start, m->vma_end, PRIVATE, nodecopy);

  replace_vma(m, repl);
  updatepages(pml4, m->vma_start, m->vma_end, [](atomic<pme_t> *p) {
      for (;;) {
        pme_t v = p->load();
        if (v & PTE_LOCK)
          continue;
        if (cmpxch(p, v, (pme_t) 0))
          break;
      }
    });

  return 0;
}

int
vmap::pagefault(uptr va, u32 err)
{
  if (va >= USERTOP)
    return -1;

  atomic<pme_t> *pte = walkpgdir(pml4, va, 1);

 retry:
  pme_t ptev = pte->load();

  // optimize checks of args to syscals
  if ((ptev & (PTE_P|PTE_U|PTE_W)) == (PTE_P|PTE_U|PTE_W)) {
    // XXX using pagefault() as a security check in syscalls is prone to races
    return 0;
  }

  if (ptev & PTE_LOCK) {
    // locked, might as well wait for the other pagefaulting core..
    goto retry;
  }

  scoped_gc_epoch gc;
  vma *m = lookup(va, 1);
  if (m == 0)
    return -1;

  u64 npg = (PGROUNDDOWN(va) - m->vma_start) / PGSIZE;
  if (vm_debug)
    cprintf("pagefault: err 0x%x va 0x%lx type %d ref %lu pid %d\n",
            err, va, m->va_type, m->n->ref.load(), myproc()->pid);

  if (m->n && !m->n->page[npg])
    if (m->n->allocpg() < 0)
      panic("pagefault: couldn't allocate pages");

  if (m->n && m->n->type == ONDEMAND)
    if (m->n->demand_load() < 0)
      panic("pagefault: couldn't load");

  if (m->va_type == COW && (err & FEC_WR)) {
    if (pagefault_wcow(m) < 0)
      return -1;

    tlbflush();
    goto retry;
  }

  if (!cmpxch(pte, ptev, ptev | PTE_LOCK))
    goto retry;

  if (m->deleted()) {
    *pte = ptev;
    goto retry;
  }

  if (m->va_type == COW) {
    *pte = v2p(m->n->page[npg]) | PTE_P | PTE_U | PTE_COW;
  } else {
    assert(m->n->ref == 1);
    *pte = v2p(m->n->page[npg]) | PTE_P | PTE_U | PTE_W;
  }

  return 1;
}

int
pagefault(struct vmap *vmap, uptr va, u32 err)
{
  return vmap->pagefault(va, err);
}

// Copy len bytes from p to user address va in vmap.
// Most useful when vmap is not the current page table.
int
vmap::copyout(uptr va, void *p, u64 len)
{
  char *buf = (char*)p;
  while(len > 0){
    uptr va0 = (uptr)PGROUNDDOWN(va);
    scoped_gc_epoch gc;
    vma *vma = lookup(va, 1);
    if(vma == 0)
      return -1;

    vma->n->allocpg();
    uptr pn = (va0 - vma->vma_start) / PGSIZE;
    char *p0 = vma->n->page[pn];
    if(p0 == 0)
      panic("copyout: missing page");
    uptr n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(p0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}