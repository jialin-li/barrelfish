#include <pager/pager.h>
#include <stdio.h>
#define LARGE_PAGE_PFAULT
// hardcoded for ARM pmap (should get from pmap)
#define VSPACE_BEGIN   ((lvaddr_t)1UL*1024*1024*1024) // 1G
#define SZ_2MB 2097152u
#define SZ_4K 4096u
static bool is_in_pmap(genvaddr_t vaddr)
{
    struct pmap *pmap = get_current_pmap();

    errval_t err = pmap->f.lookup(pmap, vaddr, NULL);
    return err_is_ok(err);
}

static errval_t alloc_mem(struct capref *retframe, size_t size)
{
    printf("allocating page with size %lx \n", size);
    assert(retframe);
    size_t frame_sz = size;
    errval_t err = frame_alloc(retframe, frame_sz, &frame_sz);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "frame_alloc");
        return err;
    }
    if (frame_sz > size) {
        printf("alloc_mem: wasting %zu bytes of memory\n", frame_sz - size);
    }
    return SYS_ERR_OK;
}

static errval_t handle_pagefault(void *addr)
{
    printf("A pagefault happened \n");
    errval_t result = SYS_ERR_OK;
    errval_t err;
    genvaddr_t vaddr = vspace_lvaddr_to_genvaddr((lvaddr_t)addr);
    if (vaddr > VSPACE_BEGIN) {
        if (is_in_pmap(vaddr)) {
            printf("handle_pagefault: returning -- mapping exists already in pmap?\n");
        } else {
            printf("handle_pagefault: no mapping for address, allocating frame\n");
            
	    struct capref frame;
	    size_t size = SZ_4K;
	    vregion_flags_t flags = VREGION_FLAGS_READ_WRITE;  
	   
#ifdef LARGE_PAGE_PFAULT
            err = alloc_mem(&frame, SZ_2MB);
            if (err_is_ok(err)) {
		flags |= VREGION_FLAGS_LARGE;
            	size = SZ_2MB;
	    } 
#endif
	    if (size == SZ_4K) {
	    	err = alloc_mem(&frame, size);
            	if (err_is_fail(err)) {
			DEBUG_ERR(err, "alloc 4k failed");
			return err;
            	}
	    }
            
	    struct pmap *pmap = get_current_pmap();
	    err = pmap->f.map(pmap, vaddr, frame, 0, size, flags, NULL, NULL);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "pmap->f.map");
            }
            printf("handle_pagefault: returning -- did install page\n");
            return SYS_ERR_OK;
        }
    } else {
        printf("handle_pagefault: invalid access to %p (< 0x%lx)\n", addr, VSPACE_BEGIN);
        // TODO: good error code
        return LIB_ERR_PMAP_ADDR_NOT_FREE;
    }

    return result;
}

static void exn_handler(enum exception_type type, int subtype,
                        void *addr, arch_registers_state_t *regs,
                        arch_registers_fpu_state_t *fpuregs)
{
    printf("exn_handler: exception type=%d, subtype=%d, addr=%p\n",
            type, subtype, addr);
    errval_t err;
    if (type == EXCEPT_PAGEFAULT) {
        err = handle_pagefault(addr);
        if (err_is_fail(err)) {
            // could not handle page fault, exiting for now
            // TODO: do something sensible here
            exit(1);
        }
    } else {
        printf("exn_handler: don't know what to do with exception type %d\n", type);
    }
    return;
}

#define INTERNAL_STACK_SIZE (1<<14)
static char internal_ex_stack[INTERNAL_STACK_SIZE];

errval_t pager_install_handler(char *ex_stack, size_t stack_size)
{
    printf("in page_install_handler\n");
    // setup exception stack pointers
    char *ex_stack_top = NULL;
    if (ex_stack && stack_size >= 4096u) {
        ex_stack_top = ex_stack + stack_size;
    } else { // use our exception stack region
        ex_stack = internal_ex_stack;
        ex_stack_top = ex_stack + INTERNAL_STACK_SIZE;
    }
    assert(ex_stack);
    assert(ex_stack_top);

    exception_handler_fn old_handler;
    errval_t err;
    void *old_stack_base, *old_stack_top;

    err = thread_set_exception_handler(exn_handler, &old_handler, 
            ex_stack, ex_stack_top, &old_stack_base, &old_stack_top);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "thread_set_exception_handler");
    }
    return 0;
}
