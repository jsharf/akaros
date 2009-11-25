/* See COPYRIGHT for copyright information. */

#ifdef __SHARC__
#pragma nosharc
#endif

#include <arch/arch.h>
#include <arch/mmu.h>
#include <arch/bitmask.h>
#include <elf.h>
#include <smp.h>

#include <atomic.h>
#include <string.h>
#include <assert.h>
#include <process.h>
#include <pmap.h>
#include <trap.h>
#include <monitor.h>
#include <manager.h>
#include <stdio.h>
#include <schedule.h>
#include <kmalloc.h>

#include <ros/syscall.h>
#include <ros/error.h>

env_t *envs = NULL;		// All environments
atomic_t num_envs;
// TODO: make this a struct of info including the pointer and cacheline-align it
// This lets the kernel know what process is running on the core it traps into.
// A lot of the Env business, including this and its usage, will change when we
// redesign the env as a multi-process.
env_t* (RO curenvs)[MAX_NUM_CPUS] = {[0 ... (MAX_NUM_CPUS-1)] NULL};

#define ENVGENSHIFT	12		// >= LOGNENV

//
// Converts an envid to an env pointer.
//
// RETURNS
//   0 on success, -EBADENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
int
envid2env(envid_t envid, env_t **env_store, bool checkperm)
{
	env_t *e;

	// If envid is zero, return the current environment.
	if (envid == 0) {
		*env_store = current;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that env_t
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
	e = &envs[ENVX(envid)];
	if (e->state == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -EBADENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	// TODO: should check for current being null
	if (checkperm && e != current && e->env_parent_id != current->env_id) {
		*env_store = 0;
		return -EBADENV;
	}

	*env_store = e;
	return 0;
}

//
// Mark all environments in 'envs' as free, set their env_ids to 0,
// and insert them into the proc_freelist.
// Insert in reverse order, so that the first call to env_alloc()
// returns envs[0].
// TODO: get rid of this whole array bullshit
//
void
env_init(void)
{
	int i;

	schedule_init();
	// core 0 is not idle, all others are (for now)
	spin_lock(&idle_lock);
	num_idlecores = num_cpus; // hack to use all cores
	for (i = 0; i < num_idlecores; i++)
		idlecoremap[i] = i; // hack to use all cores
	spin_unlock(&idle_lock);
	atomic_init(&num_envs, 0);
	TAILQ_INIT(&proc_freelist);
	assert(envs != NULL);
	for (i = NENV-1; i >= 0; i--) {
		// these should already be set from when i memset'd the array to 0
		envs[i].state = ENV_FREE;
		envs[i].end_text_segment = (void*)UTEXT;
		envs[i].end_data_segment = (void*)UTEXT;
		envs[i].env_id = 0;
		TAILQ_INSERT_HEAD(&proc_freelist, &envs[i], proc_link);
	}
}

//
// Initialize the kernel virtual memory layout for environment e.
// Allocate a page directory, set e->env_pgdir and e->env_cr3 accordingly,
// and initialize the kernel portion of the new environment's address space.
// Do NOT (yet) map anything into the user portion
// of the environment's virtual address space.
//
// Returns 0 on success, < 0 on error.  Errors include:
//	-ENOMEM if page directory or table could not be allocated.
//
static int
env_setup_vm(env_t *e)
WRITES(e->env_pgdir, e->env_cr3, e->env_procinfo, e->env_procdata)
{
	int i, r;
	page_t *pgdir = NULL;
	page_t *pginfo[PROCINFO_NUM_PAGES] = {NULL};
	page_t *pgdata[PROCDATA_NUM_PAGES] = {NULL};
	static page_t * RO shared_page = 0;

	/*
	 * First, allocate a page for the pgdir of this process and up
	 * its reference count since this will never be done elsewhere
	 */
	r = kpage_alloc(&pgdir);
	if(r < 0) return r;

	/*
	 * Next, set up the e->env_pgdir and e->env_cr3 pointers to point
	 * to this newly allocated page and clear its contents
	 */
	memset(page2kva(pgdir), 0, PGSIZE);
	e->env_pgdir = (pde_t *COUNT(NPDENTRIES)) TC(page2kva(pgdir));
	e->env_cr3 =   (physaddr_t) TC(page2pa(pgdir));

	/*
	 * Now start filling in the pgdir with mappings required by all newly
	 * created address spaces
	 */

	// Map in the kernel to the top of every address space
	// should be able to do this so long as boot_pgdir never has
	// anything put below UTOP
	// TODO check on this!  had a nasty bug because of it
	// this is a bit wonky, since if it's not PGSIZE, lots of other things are
	// screwed up...
	memcpy(e->env_pgdir, boot_pgdir, NPDENTRIES*sizeof(pde_t));

	// VPT and UVPT map the env's own page table, with
	// different permissions.
	e->env_pgdir[PDX(VPT)]  = PTE(LA2PPN(e->env_cr3), PTE_P | PTE_KERN_RW);
	e->env_pgdir[PDX(UVPT)] = PTE(LA2PPN(e->env_cr3), PTE_P | PTE_USER_RO);

	/*
	 * Now allocate and insert all pages required for the shared
	 * procinfo structure into the page table
	 */
	for(int i=0; i<PROCINFO_NUM_PAGES; i++) {
		if(upage_alloc(e, &pginfo[i]) < 0)
			goto env_setup_vm_error;
		if(page_insert(e->env_pgdir, pginfo[i], (void*SNT)(UINFO + i*PGSIZE),
		               PTE_USER_RO) < 0)
			goto env_setup_vm_error;
	}

	/*
	 * Now allocate and insert all pages required for the shared
	 * procdata structure into the page table
	 */
	for(int i=0; i<PROCDATA_NUM_PAGES; i++) {
		if(upage_alloc(e, &pgdata[i]) < 0)
			goto env_setup_vm_error;
		if(page_insert(e->env_pgdir, pgdata[i], (void*SNT)(UDATA + i*PGSIZE),
		               PTE_USER_RW) < 0)
			goto env_setup_vm_error;
	}

	/*
	 * Now, set e->env_procinfo, and e->env_procdata to point to
	 * the proper pages just allocated and clear them out.
	 */
	e->env_procinfo = (procinfo_t *SAFE) TC(page2kva(pginfo[0]));
	e->env_procdata = (procdata_t *SAFE) TC(page2kva(pgdata[0]));

	memset(e->env_procinfo, 0, sizeof(procinfo_t));
	memset(e->env_procdata, 0, sizeof(procdata_t));

	/* Finally, set up the Global Shared Data page for all processes.
	 * Can't be trusted, but still very useful at this stage for us.
	 * Consider removing when we have real processes.
	 * (TODO).  Note the page is alloced only the first time through
	 */
	if (!shared_page) {
		if(upage_alloc(e, &shared_page) < 0)
			goto env_setup_vm_error;
		// Up it, so it never goes away.  One per user, plus one from page_alloc
		// This is necessary, since it's in the per-process range of memory that
		// gets freed during page_free.
		page_incref(shared_page);
	}

	// Inserted into every process's address space at UGDATA
	if(page_insert(e->env_pgdir, shared_page, (void*SNT)UGDATA, PTE_USER_RW) < 0)
		goto env_setup_vm_error;

	return 0;

env_setup_vm_error:
	page_free(shared_page);
	for(int i=0; i< PROCDATA_NUM_PAGES; i++) {
		page_free(pgdata[i]);
	}
	for(int i=0; i< PROCINFO_NUM_PAGES; i++) {
		page_free(pginfo[i]);
	}
	env_user_mem_free(e);
	page_free(pgdir);
	return -ENOMEM;
}

static void
proc_init_procinfo(struct proc* p)
{
	p->env_procinfo->id = (p->env_id & 0x3FF);

	// TODO: maybe do something smarter here
	p->env_procinfo->max_harts = MAX(1,num_cpus); // hack to use all cores
}

// Sets up argc/argv in procinfo.  Returns number of
// args successfully imported (because of size restrictions).
// The procinfo pages must have been mapped into the user's
// address space before this function can be called.
size_t
proc_init_argc_argv(struct proc* p, size_t nargs, const char** args)
{
	// TODO: right now we assume procinfo can be directly addressed
	// by the kernel (i.e. it's continguous.
	static_assert(sizeof(struct procinfo) <= PGSIZE);

	if(nargs > PROCINFO_MAX_ARGC)
		nargs = PROCINFO_MAX_ARGC;

	char* argv[PROCINFO_MAX_ARGC] = {0};
	static_assert(sizeof(argv) == sizeof(p->env_procinfo->argv));

	size_t size = 0, argc;
	for(argc = 0; argc < nargs; argc++)
	{
		size_t len = strnlen(args[argc],PROCINFO_MAX_ARGV_SIZE);
		if(size+len+1 > PROCINFO_MAX_ARGV_SIZE)
			break;
		memcpy(&p->env_procinfo->argv_buf[size],args[argc],len+1);
		argv[argc] = (char*)(UINFO+offsetof(struct procinfo,argv_buf)+size);
		size += len+1;
	}

	p->env_procinfo->argc = argc;
	memcpy(p->env_procinfo->argv,argv,sizeof(argv));

	return argc;
}

//
// Allocates and initializes a new environment.
// On success, the new environment is stored in *newenv_store.
//
// Returns 0 on success, < 0 on failure.  Errors include:
//	-ENOFREEENV if all NENVS environments are allocated
//	-ENOMEM on memory exhaustion
//
int
env_alloc(env_t **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	env_t *e;

	spin_lock(&freelist_lock);
	e = TAILQ_FIRST(&proc_freelist);
	if (e) {
		TAILQ_REMOVE(&proc_freelist, e, proc_link);
		spin_unlock(&freelist_lock);
	} else {
		spin_unlock(&freelist_lock);
		return -ENOFREEENV;
	}

    { INITSTRUCT(*e)

	// Setup the default map of where to get cache colors from
	e->cache_colors_map = global_cache_colors_map;
	e->next_cache_color = 0;

	// Allocate and set up the page directory for this environment.
	if ((r = env_setup_vm(e)) < 0) {
		spin_lock(&freelist_lock);
		TAILQ_INSERT_HEAD(&proc_freelist, e, proc_link);
		spin_unlock(&freelist_lock);
		return r;
	}

	// Generate an env_id for this environment.
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	if (generation <= 0)	// Don't create a negative env_id.
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs);

	// Set the basic status variables.
	e->proc_lock = 0;
	e->env_parent_id = parent_id;
	proc_set_state(e, PROC_CREATED);
	e->env_runs = 0;
	e->env_refcnt = 1;
	e->env_flags = 0;
	e->env_entry = 0; // cheating.  this really gets set in load_icode
	e->num_vcores = 0;
	for (int i = 0; i < MAX_NUM_CPUS; i++)
		e->vcoremap[i] = -1;
	memset(&e->resources, 0, sizeof(e->resources));

	memset(&e->env_ancillary_state, 0, sizeof(e->env_ancillary_state));
	memset(&e->env_tf, 0, sizeof(e->env_tf));
	proc_init_trapframe(&e->env_tf);

	proc_init_procinfo(e);

	/*
	 * Initialize the contents of the e->env_procdata structure
	 */
	// Initialize the generic syscall ring buffer
	SHARED_RING_INIT(&e->env_procdata->syscallring);
	// Initialize the backend of the syscall ring buffer
	BACK_RING_INIT(&e->syscallbackring,
	               &e->env_procdata->syscallring,
	               SYSCALLRINGSIZE);

	// Initialize the generic sysevent ring buffer
	SHARED_RING_INIT(&e->env_procdata->syseventring);
	// Initialize the frontend of the sysevent ring buffer
	FRONT_RING_INIT(&e->syseventfrontring,
	                &e->env_procdata->syseventring,
	                SYSEVENTRINGSIZE);

	*newenv_store = e;
	atomic_inc(&num_envs);

	printk("[%08x] new env %08x\n", current ? current->env_id : 0, e->env_id);
	} // INIT_STRUCT
	return 0;
}

//
// Allocate len bytes of physical memory for environment env,
// and map it at virtual address va in the environment's address space.
// Pages are zeroed by upage_alloc.
// Pages should be writable by user and kernel.
// Panic if any allocation attempt fails.
//
void
env_segment_alloc(env_t *e, void *SNT va, size_t len)
{
	void *SNT start, *SNT end;
	size_t num_pages;
	int i, r;
	page_t *page;
	pte_t *pte;

	start = ROUNDDOWN(va, PGSIZE);
	end = ROUNDUP(va + len, PGSIZE);
	if (start >= end)
		panic("Wrap-around in memory allocation addresses!");
	if ((uintptr_t)end > UTOP)
		panic("Attempting to map above UTOP!");
	num_pages = LA2PPN(end - start);

	for (i = 0; i < num_pages; i++, start += PGSIZE) {
		// skip if a page is already mapped.  yes, page_insert will page_remove
		// whatever page was already there, but if we are seg allocing adjacent
		// regions, we don't want to destroy that old mapping/page
		// though later on we are told we can ignore this...
		pte = pgdir_walk(e->env_pgdir, start, 0);
		if (pte && *pte & PTE_P)
			continue;
		if ((r = upage_alloc(e, &page)) < 0)
			panic("env_segment_alloc: %e", r);
		page_insert(e->env_pgdir, page, start, PTE_USER_RW);
	}
}

void
env_segment_free(env_t *e, void *SNT va, size_t len)
{
	void *SNT start, *SNT end;
	size_t num_pages;
	page_t *page;
	pte_t *pte;

	// Round this up this time so we don't free the page that va is actually on
	start = ROUNDUP(va, PGSIZE);
	end = ROUNDUP(va + len, PGSIZE);
	if (start >= end)
		panic("Wrap-around in memory free addresses!");
	if ((uintptr_t)end > UTOP)
		panic("Attempting to unmap above UTOP!");
	// page_insert/pgdir_walk alloc a page and read/write to it via its address
	// starting from pgdir (e's), so we need to be using e's pgdir
	assert(e->env_cr3 == rcr3());
	num_pages = LA2PPN(end - start);

	for (int i = 0; i < num_pages; i++, start += PGSIZE) {
		// skip if a page is already unmapped. 
		pte = pgdir_walk(e->env_pgdir, start, 0);
		if (pte && *pte & PTE_P)
			page_remove(e->env_pgdir,start);
	}
}

// this helper function handles all cases of copying to/from user/kernel
// or between two users.
static error_t
load_icode_memcpy(env_t* e, env_t* binary_env, void* dest, const void* src, size_t len)
{
	if(src < (void*)UTOP)
	{
		if(binary_env == NULL)
			return -EFAULT;

		if(e == NULL)
			return memcpy_from_user(binary_env,dest,src,len);
		else
		{
			// TODO: do something more elegant & faster here.
			// e.g. a memcpy_from_user_to_user
			uint8_t kbuf[1024];
			while(len > 0)
			{
				size_t thislen = MIN(len,sizeof(kbuf));
				if(memcpy_from_user(binary_env,kbuf,src,thislen))
					return -EFAULT;
				if(memcpy_to_user(e,dest,kbuf,thislen))
					panic("destination env isn't mapped!");
				len -= thislen;
				src += thislen;
				dest += thislen;
			}
			return ESUCCESS;
		}

	}
	else
	{
		if(binary_env != NULL)
			return -EFAULT;

		if(e == NULL)
			memcpy(dest,src,len);
		else if(memcpy_to_user(e,dest,src,len))
			panic("destination env isn't mapped!");

		return ESUCCESS;
	}
}

//
// Set up the initial program binary, stack, and processor flags
// for a user process.
//
// This function loads all loadable segments from the ELF binary image
// into the environment's user memory, starting at the appropriate
// virtual addresses indicated in the ELF program header.
// At the same time it clears to zero any portions of these segments
// that are marked in the program header as being mapped
// but not actually present in the ELF file - i.e., the program's bss section.
//
// Finally, this function maps one page for the program's initial stack.
static void*
load_icode(env_t *SAFE e, env_t* binary_env, uint8_t *COUNT(size) binary, size_t size)
{
	// asw: copy the headers because they might not be aligned.
	elf_t elfhdr;
	proghdr_t phdr;
	void* _end = 0;

	assert(load_icode_memcpy(NULL,binary_env,&elfhdr, binary, sizeof(elfhdr)) == ESUCCESS);

	int i, r;

	// is this an elf?
	assert(elfhdr.e_magic == ELF_MAGIC);
	// make sure we have proghdrs to load
	assert(elfhdr.e_phnum);

	// TODO: how do we do a runtime COUNT?
	{TRUSTEDBLOCK // zra: TRUSTEDBLOCK until validation is done.
	for (i = 0; i < elfhdr.e_phnum; i++) {
		// copy phdr to kernel mem
		assert(load_icode_memcpy(NULL,binary_env,&phdr, binary + elfhdr.e_phoff + i*sizeof(phdr), sizeof(phdr)) == ESUCCESS);

		if (phdr.p_type != ELF_PROG_LOAD)
			continue;
		// TODO: validate elf header fields!
		// seg alloc creates PTE_U|PTE_W pages.  if you ever want to change
		// this, there will be issues with overlapping sections
		_end = MAX(_end, (void*)(phdr.p_va + phdr.p_memsz));
		env_segment_alloc(e, (void*SNT)phdr.p_va, phdr.p_memsz);

		// copy section to user mem
		assert(load_icode_memcpy(e,binary_env,(void*)phdr.p_va, binary + phdr.p_offset, phdr.p_filesz) == ESUCCESS);

		//no need to memclr the remaining p_memsz-p_filesz bytes
		//because upage_alloc'd pages are zeroed
	}}

	proc_set_program_counter(&e->env_tf, elfhdr.e_entry);
	e->env_entry = elfhdr.e_entry;

	// Now map USTACK_NUM_PAGES pages for the program's initial stack
	// starting at virtual address USTACKTOP - USTACK_NUM_PAGES*PGSIZE.
	env_segment_alloc(e, (void*SNT)(USTACKTOP - USTACK_NUM_PAGES*PGSIZE), 
	                  USTACK_NUM_PAGES*PGSIZE);
	
	return _end;
}

//
// Allocates a new env and loads the named elf binary into it.
//
env_t* env_create()
{
	env_t *e;
	int r;
	envid_t curid;

	curid = (current ? current->env_id : 0);
	if ((r = env_alloc(&e, curid)) < 0)
		panic("env_create: %e", r);

	// default PC: will cause page fault if not otherwise set.
	proc_set_program_counter(&e->env_tf, 0);
	e->end_text_segment = 0;
	e->end_data_segment = 0;

	return e;
}

void env_load_icode(env_t* e, env_t* binary_env, uint8_t* binary, size_t size)
{
	/* Load the binary and set the current locations of the elf segments.
	 * All end-of-segment pointers are page aligned (invariant) */
	e->end_text_segment = load_icode(e, binary_env, binary, size);
	e->end_data_segment = e->end_text_segment;
}

//
// Frees env e and all memory it uses.
//
void
env_free(env_t *e)
{
	physaddr_t pa;

	// Note the environment's demise.
	printk("[%08x] free env %08x\n", current ? current->env_id : 0, e->env_id);
	// All parts of the kernel should have decref'd before env_free was called.
	assert(e->env_refcnt == 0);

	// Free any colors allocated to this process
	if(e->cache_colors_map != global_cache_colors_map) {
		for(int i=0; i<llc_cache->num_colors; i++)
			cache_color_free(llc_cache, e->cache_colors_map);
		cache_colors_map_free(e->cache_colors_map);
	}

	// Flush all mapped pages in the user portion of the address space
	env_user_mem_free(e);

	// free the page directory
	pa = e->env_cr3;
	e->env_pgdir = 0;
	e->env_cr3 = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	e->state = ENV_FREE;
	spin_lock(&freelist_lock);
	TAILQ_INSERT_HEAD(&proc_freelist, e, proc_link);
	spin_unlock(&freelist_lock);
}


#define PER_CPU_THING(type,name)\
type SLOCKED(name##_lock) * RWPROTECT name;\
type SLOCKED(name##_lock) *\
(get_per_cpu_##name)()\
{\
	{ R_PERMITTED(global(name))\
		return &name[core_id()];\
	}\
}


/* This is the top-half of an interrupt handler, where the bottom half is
 * proc_run (which never returns).  Just add it to the delayed work queue,
 * which (incidentally) can only hold one item at this point.
 *
 * Note this is rather old, and meant to run a RUNNABLE_S on a worker core.
 */
#ifdef __IVY__
void run_env_handler(trapframe_t *tf, env_t * data)
#else
void run_env_handler(trapframe_t *tf, void * data)
#endif
{
	assert(data);
	struct work TP(env_t *) job;
	struct workqueue TP(env_t *) *CT(1) workqueue =
	    TC(&per_cpu_info[core_id()].workqueue);
	// this doesn't work, and making it a TP(env_t) is wrong
	// zra: When you want to use other types, let me know, and I can help
    // make something that Ivy is happy with. 
#ifdef __IVY__
	job.func = proc_run;
#else
	job.func = (func_t)proc_run;
#endif
	job.data = data;
	if (enqueue_work(workqueue, &job))
		panic("Failed to enqueue work!");
}
