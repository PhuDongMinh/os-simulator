/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System OS module
 */
#include "cpu.h"
#include "timer.h"
#include "sched.h"
#include "loader.h"
#include "libmem.h"
#include "mm.h"
#ifdef MM64
#include "mm64.h"
#endif
#include "dma.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* The DMA (Verilator RTL + system bus) is built in with -DUSE_DMA */
#ifdef USE_DMA
#include "sysbus.h"
#include "dpi_rtl.h"
#endif

#define PATH_LEN 200

static int time_slot;
static int num_cpus;
/* Written by the loader thread, read by CPU threads: must be atomic */
static atomic_int done = 0;
static struct krnl_t os;

#ifdef MM_PAGING
static unsigned long memramsz;
static unsigned long memswpsz[PAGING_MAX_MMSWP];

struct mmpaging_ld_args {
	/* A dispatched argument struct to compact many-fields passing to loader */
	int vmemsz;
	struct memphy_struct *mram;
	struct memphy_struct **mswp;
	struct memphy_struct *active_mswp;
	int active_mswp_id;
	struct timer_id_t  *timer_id;
};
#endif

static struct ld_args {
	char **path;
	unsigned long *start_time;
#ifdef MLQ_SCHED
	unsigned long *prio;
#endif
} ld_processes;
int num_processes;

struct cpu_args {
	struct timer_id_t *timer_id;
	int id;
};

/* ------------------------------------------------------------------------
 * DMA service: a queue of copy requests and ONE worker thread that drives
 * the RTL simulation. A CPU only queues a request (dma_submit) and goes on
 * with its next instruction.
 *
 * The worker is not a timer device: the RTL has its own clock and runs at
 * its own pace, so the simulated slots do not wait for it. The only place
 * where a CPU waits for the DMA is when a process ends
 * (dma_process_exit), so that its frames are not reused while the RTL is
 * still writing them.
 * ------------------------------------------------------------------------ */
#ifdef USE_DMA

struct dma_req {
	uint32_t pid;
	uint64_t src;
	uint64_t dst;
	uint64_t len;
	dma_done_fn done;
	void *ctx;
	struct dma_req *next;
};

/* Per process bookkeeping */
struct dma_acct {
	uint32_t pid;
	int pending;                /* queued or running */
	int failed;                 /* finished with an error */
	struct dma_acct *next;
};

static pthread_mutex_t dma_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dma_work = PTHREAD_COND_INITIALIZER;     /* worker: a request or stop */
static pthread_cond_t dma_finished = PTHREAD_COND_INITIALIZER; /* waiters: a request completed */
static struct dma_req *dma_head = NULL;
static struct dma_req *dma_tail = NULL;
static struct dma_acct *dma_accts = NULL;
static int dma_running = 0;
static int dma_stop = 0;
static pthread_t dma_thread;

/* dma_lock must be held */
static struct dma_acct *dma_acct_find(uint32_t pid, int create)
{
	struct dma_acct *a;

	for (a = dma_accts; a != NULL; a = a->next)
		if (a->pid == pid)
			return a;
	if (!create)
		return NULL;
	a = (struct dma_acct *)calloc(1, sizeof(struct dma_acct));
	if (a == NULL)
		return NULL;
	a->pid = pid;
	a->next = dma_accts;
	dma_accts = a;
	return a;
}

int dma_submit(uint32_t pid, uint64_t source, uint64_t destination, uint64_t length,
	       dma_done_fn done, void *ctx)
{
	struct dma_req *req = (struct dma_req *)calloc(1, sizeof(struct dma_req));
	struct dma_acct *acct;

	if (req == NULL)
		return -1;
	req->pid = pid;
	req->src = source;
	req->dst = destination;
	req->len = length;
	req->done = done;
	req->ctx = ctx;

	pthread_mutex_lock(&dma_lock);
	acct = (dma_running && !dma_stop) ? dma_acct_find(pid, 1) : NULL;
	if (acct == NULL) {
		pthread_mutex_unlock(&dma_lock);
		free(req);
		return -1;
	}
	acct->pending++;
	if (dma_tail != NULL)
		dma_tail->next = req;
	else
		dma_head = req;
	dma_tail = req;
	pthread_cond_signal(&dma_work);
	pthread_mutex_unlock(&dma_lock);
	return 0;
}

int dma_pending_for(uint32_t pid)
{
	struct dma_acct *a;
	int n = 0;

	pthread_mutex_lock(&dma_lock);
	a = dma_acct_find(pid, 0);
	if (a != NULL)
		n = a->pending;
	pthread_mutex_unlock(&dma_lock);
	return n;
}

int dma_process_exit(uint32_t pid)
{
	struct dma_acct *a;
	int failed = 0;

	pthread_mutex_lock(&dma_lock);
	a = dma_acct_find(pid, 0);
	if (a != NULL) {
		struct dma_acct **pp;

		while (a->pending > 0)
			pthread_cond_wait(&dma_finished, &dma_lock);
		failed = a->failed;
		for (pp = &dma_accts; *pp != a; pp = &(*pp)->next)
			;
		*pp = a->next;
		free(a);
	}
	pthread_mutex_unlock(&dma_lock);
	return failed;
}

/* The only thread that touches the RTL simulation */
static void *dma_worker(void *arg)
{
	(void)arg;

	while (1) {
		struct dma_req *req;
		struct dma_acct *a;
		int rc;

		pthread_mutex_lock(&dma_lock);
		while (dma_head == NULL && !dma_stop)
			pthread_cond_wait(&dma_work, &dma_lock);
		if (dma_head == NULL) {
			/* stop requested and nothing left to do */
			pthread_mutex_unlock(&dma_lock);
			break;
		}
		req = dma_head;
		dma_head = req->next;
		if (dma_head == NULL)
			dma_tail = NULL;
		pthread_mutex_unlock(&dma_lock);

		rc = do_dma(NULL, req->src, req->dst, req->len);
		if (req->done != NULL)
			req->done(req->ctx, rc);

		pthread_mutex_lock(&dma_lock);
		a = dma_acct_find(req->pid, 0);
		if (a != NULL) {
			a->pending--;
			if (rc != 0)
				a->failed++;
		}
		pthread_cond_broadcast(&dma_finished);
		pthread_mutex_unlock(&dma_lock);
		free(req);
	}
	return NULL;
}

/* Initialise the RTL simulation, then start the worker */
static int dma_service_start(void)
{
	if (rtl_dma_init_simulation() != 0) {
		printf("Cannot initialize RTL simulation\n");
		return -1;
	}
	pthread_mutex_lock(&dma_lock);
	dma_stop = 0;
	dma_running = 1;
	pthread_mutex_unlock(&dma_lock);
	if (pthread_create(&dma_thread, NULL, dma_worker, NULL) != 0) {
		printf("Cannot start the DMA worker\n");
		pthread_mutex_lock(&dma_lock);
		dma_running = 0;
		pthread_mutex_unlock(&dma_lock);
		rtl_dma_cleanup_simulation();
		return -1;
	}
	return 0;
}

/* Let the worker finish what is queued, join it, release the RTL */
static void dma_service_stop(void)
{
	struct dma_acct *a;

	if (!dma_running)
		return;
	pthread_mutex_lock(&dma_lock);
	dma_stop = 1;
	pthread_cond_broadcast(&dma_work);
	pthread_mutex_unlock(&dma_lock);
	pthread_join(dma_thread, NULL);

	pthread_mutex_lock(&dma_lock);
	dma_running = 0;
	while (dma_accts != NULL) {
		a = dma_accts;
		dma_accts = a->next;
		free(a);
	}
	pthread_mutex_unlock(&dma_lock);
	rtl_dma_cleanup_simulation();
}

#else /* !USE_DMA: no DMA, requests are refused */

int dma_submit(uint32_t pid, uint64_t source, uint64_t destination, uint64_t length,
	       dma_done_fn done, void *ctx)
{
	(void)pid; (void)source; (void)destination; (void)length;
	(void)done; (void)ctx;
	return -1;
}

int dma_pending_for(uint32_t pid)
{
	(void)pid;
	return 0;
}

int dma_process_exit(uint32_t pid)
{
	(void)pid;
	return 0;
}

static void dma_service_stop(void)
{
}

#endif /* USE_DMA */

/*
 * CPU thread. Rules for the timer:
 *  - every slot, this thread must call next_slot() exactly once
 *  - when it exits, it must call detach_event()
 */
static void *cpu_routine(void *args)
{
	struct timer_id_t *timer_id = ((struct cpu_args *)args)->timer_id;
	int id = ((struct cpu_args *)args)->id;
	int time_left = 0;
	struct pcb_t *proc = NULL;

	while (1) {
		/* 1. Handle the current process, if any */
		if (proc != NULL) {
			if (proc->pc == proc->code->size) {
				/* Cleanup order (API_CONTRACT §4.5):
				 * DMA done -> unregister -> kernel regions -> user memory
				 * -> mm -> PCB. */

				/* 1. Last instruction done. The process may still have DMA
				 * requests in flight: wait for them, so that its memory is
				 * not released while the RTL is writing it. */
				int dma_failed = dma_process_exit(proc->pid);

				if (dma_failed > 0)
					printf("\tCPU %d: PID %2d: %d DMA request(s) failed\n",
					       id, proc->pid, dma_failed);

				/* 2. Unregister: syscalls can no longer find this PCB */
				pthread_mutex_lock((pthread_mutex_t *)proc->krnl->registry_lock);
				for (int j = 0; j < MAX_PROCESS_REGISTRY; j++) {
					if (proc->krnl->proc_registry[j] == proc) {
						proc->krnl->proc_registry[j] = NULL;
						break;
					}
				}
				pthread_mutex_unlock((pthread_mutex_t *)proc->krnl->registry_lock);
#ifdef MM_PAGING
				/* 3. Kernel regions owned by the process */
				for (int k = 0; k < PAGING_MAX_SYMTBL_SZ; k++) {
					if (proc->kmap_slot[k] != -1)
						__kfree(proc, 0, k);
				}

				/* 4. User memory, then the mm itself */
				if (proc->mm != NULL) {
#ifdef MM64
					/* TODO(M1): free_pcb_memph() for MM64 must return the
					 * RAM frames and swap slots first; until then they leak.
					 * Only the page-table tree is released here. */
					mm64_pgd_destroy(proc->mm);
#else
					free_pcb_memph(proc);
#endif
					free(proc->mm);
					proc->mm = NULL;
				}
#endif
				printf("\tCPU %d: Processed %2d has finished\n",
				       id, proc->pid);
				/* TODO(cleanup): the process must also be removed from
				 * krnl->running_list under the scheduler's lock, which needs
				 * a function from sched.c. */
				free(proc->code->text);
				free(proc->code);
				free(proc->page_table); /* legacy field, may be NULL */
				free(proc);
				proc = NULL;
			} else if (time_left == 0) {
				/* The process used up its time slice */
				printf("\tCPU %d: Put process %2d to run queue\n",
				       id, proc->pid);
				put_proc(proc);
				proc = NULL;
			}
		}

		/* 2. No process: ask the scheduler for a new one */
		if (proc == NULL) {
			/* Read "done" BEFORE get_proc(): if the loader adds the
			 * last process and sets done between the two calls, we
			 * must not exit and miss that process. */
			int finished = atomic_load(&done);

			proc = get_proc();
			if (proc == NULL) {
				if (finished) {
					printf("\tCPU %d stopped\n", id);
					break;
				}
				/* Maybe new processes arrive in later slots */
				next_slot(timer_id);
				continue;
			}
			printf("\tCPU %d: Dispatched process %2d\n",
			       id, proc->pid);
			time_left = time_slot;
		}

		/* 3. Run one instruction in this slot. A failing instruction is
		 * reported and the process goes on with the next one. */
		if (proc->pc < proc->code->size && run(proc) != 0)
			printf("\tCPU %d: PID %2d: instruction at pc=%u failed\n",
			       id, proc->pid, proc->pc - 1);
		time_left--;
		next_slot(timer_id);
	}
	detach_event(timer_id);
	pthread_exit(NULL);
}

static void *ld_routine(void *args)
{
#ifdef MM_PAGING
	struct memphy_struct *mram = ((struct mmpaging_ld_args *)args)->mram;
	struct memphy_struct **mswp = ((struct mmpaging_ld_args *)args)->mswp;
	struct memphy_struct *active_mswp = ((struct mmpaging_ld_args *)args)->active_mswp;
	int active_mswp_id = ((struct mmpaging_ld_args *)args)->active_mswp_id;
	struct timer_id_t *timer_id = ((struct mmpaging_ld_args *)args)->timer_id;
#else
	struct timer_id_t *timer_id = (struct timer_id_t *)args;
#endif
	int i = 0;

#ifdef MM_PAGING
	os.mram = mram;
	os.mswp = mswp;
	os.active_mswp = active_mswp;
	os.active_mswp_id = active_mswp_id;
	/* Shared kernel address space: init_mm(.., NULL) creates its root. */
	os.mm = (struct mm_struct *)calloc(1, sizeof(struct mm_struct));
	if (os.mm == NULL || init_mm(os.mm, NULL) != 0) {
		printf("Cannot initialize kernel address space\n");
		exit(1);
	}
#ifdef MM64
	/* Borrowed alias of the kernel root: never allocated/freed separately */
	os.krnl_pgd = os.mm->pgd;
	os.krnl_p4d = NULL;
	os.krnl_pud = NULL;
	os.krnl_pmd = NULL;
	os.krnl_pt = NULL;
#else
	os.krnl_pgd = (uint32_t *)calloc(PAGING_MAX_PGN, sizeof(uint32_t));
#endif
#endif
	printf("ld_routine\n");

	while (i < num_processes) {
		struct pcb_t *proc = load(ld_processes.path[i]);

		for (int k = 0; k < PAGING_MAX_SYMTBL_SZ; k++) proc->kmap_slot[k] = -1;
		proc->krnl = &os;

#ifdef MLQ_SCHED
		if (ld_processes.prio[i] >= MAX_PRIO) {
			printf("\tInvalid priority %lu for %s, clamp to %d\n",
			       ld_processes.prio[i], ld_processes.path[i],
			       MAX_PRIO - 1);
			ld_processes.prio[i] = MAX_PRIO - 1;
		}
		proc->prio = ld_processes.prio[i];
#endif
		/* Wait until the start time of this process */
		while (current_time() < ld_processes.start_time[i]) {
			next_slot(timer_id);
		}
#ifdef MM_PAGING
		proc->mm = (struct mm_struct *)calloc(1, sizeof(struct mm_struct));
		if (proc->mm == NULL || init_mm(proc->mm, proc) != 0) {
			printf("Cannot initialize address space of %s\n",
			       ld_processes.path[i]);
			exit(1);
		}
#endif
		/* Đăng ký tiến trình vào Kernel Registry sau khi đã khởi tạo xong mm */
		pthread_mutex_lock((pthread_mutex_t *)os.registry_lock);
		for (int j = 0; j < MAX_PROCESS_REGISTRY; j++) {
			if (os.proc_registry[j] == NULL) {
				os.proc_registry[j] = proc;
				break;
			}
		}
		pthread_mutex_unlock((pthread_mutex_t *)os.registry_lock);
		
#ifdef MLQ_SCHED
		printf("\tLoaded a process at %s, PID: %d PRIO: %lu\n",
		       ld_processes.path[i], proc->pid, ld_processes.prio[i]);
#else
		printf("\tLoaded a process at %s, PID: %d\n",
		       ld_processes.path[i], proc->pid);
#endif
		add_proc(proc);
		free(ld_processes.path[i]);
		i++;
		next_slot(timer_id);
	}
	free(ld_processes.path);
	free(ld_processes.start_time);
#ifdef MLQ_SCHED
	free(ld_processes.prio);
#endif
	atomic_store(&done, 1);
	detach_event(timer_id);
	pthread_exit(NULL);
}

static void read_config(const char *path)
{
	FILE *file;
	int i;

	if ((file = fopen(path, "r")) == NULL) {
		printf("Cannot find configure file at %s\n", path);
		exit(1);
	}
	if (fscanf(file, "%d %d %d\n", &time_slot, &num_cpus,
		   &num_processes) != 3) {
		printf("Bad configure file: expected "
		       "[time slice] [N CPU] [M processes]\n");
		exit(1);
	}
	ld_processes.path = (char **)malloc(sizeof(char *) * num_processes);
	ld_processes.start_time = (unsigned long *)
		malloc(sizeof(unsigned long) * num_processes);
#ifdef MM_PAGING
	int sit;
#ifdef MM_FIXED_MEMSZ
	/* Backward compatible with the legacy config file (one line only) */
	memramsz = 0x100000000;
	memswpsz[0] = 0x1000000;
	for (sit = 1; sit < PAGING_MAX_MMSWP; sit++)
		memswpsz[sit] = 0;
#else
	/* MEM_RAM_SZ MEM_SWP0_SZ MEM_SWP1_SZ MEM_SWP2_SZ MEM_SWP3_SZ
	 * (size = 0 means that swap device is not used) */
	/* memramsz/memswpsz are unsigned long, so use %lu explicitly:
	 * FORMAT_ARG is "%u" when MM64 is off and would write only 4 bytes */
	fscanf(file, "%lu\n", &memramsz);
	for (sit = 0; sit < PAGING_MAX_MMSWP; sit++)
		fscanf(file, "%lu", &(memswpsz[sit]));
	fscanf(file, "\n");
#endif
#endif

#ifdef MLQ_SCHED
	ld_processes.prio = (unsigned long *)
		malloc(sizeof(unsigned long) * num_processes);
#endif
	for (i = 0; i < num_processes; i++) {
		char proc[100];
		int n;

		ld_processes.path[i] = (char *)malloc(PATH_LEN);
#ifdef MLQ_SCHED
		n = fscanf(file, "%lu %99s %lu\n", &ld_processes.start_time[i],
			   proc, &ld_processes.prio[i]);
		if (n != 3) {
#else
		n = fscanf(file, "%lu %99s\n", &ld_processes.start_time[i],
			   proc);
		if (n != 2) {
#endif
			printf("Bad configure file at process line %d\n", i);
			exit(1);
		}
		snprintf(ld_processes.path[i], PATH_LEN, "input/proc/%s", proc);
	}
	fclose(file);
}

int main(int argc, char *argv[])
{
	char path[PATH_LEN];
	int i;

	/* Read config */
	if (argc != 2) {
		printf("Usage: os [path to configure file]\n");
		return 1;
	}
	snprintf(path, sizeof(path), "input/%s", argv[1]);
	read_config(path);

	pthread_t *cpu = (pthread_t *)malloc(num_cpus * sizeof(pthread_t));
	struct cpu_args *args =
		(struct cpu_args *)malloc(sizeof(struct cpu_args) * num_cpus);
	pthread_t ld;

	/* Init timer: one event per thread (each CPU + loader) */
	for (i = 0; i < num_cpus; i++) {
		args[i].timer_id = attach_event();
		args[i].id = i;
	}
	struct timer_id_t *ld_event = attach_event();
	start_timer();

#ifdef MM_PAGING
	/* Init all MEMPHY: 1 MEMRAM and n of MEMSWP. Zero them first: a swap
	 * device of size 0 is not formatted and must not hold garbage lists. */
	int rdmflag = 1; /* random access memory */

	struct memphy_struct mram;
	struct memphy_struct mswp[PAGING_MAX_MMSWP];
	/* krnl_t.mswp is an array of POINTERS to the devices */
	struct memphy_struct *mswp_ptr[PAGING_MAX_MMSWP];
	int sit;

#ifdef MM64
	/* A PTE can only address PAGING64_MAX_RAMSZ of RAM (16-bit FPN) and
	 * PAGING64_MAX_SWPSZ per swap device: larger devices are clamped. */
	if (memramsz > PAGING64_MAX_RAMSZ) {
		printf("RAM size %lu exceeds the %llu bytes a PTE can address, "
		       "clamped\n", memramsz, PAGING64_MAX_RAMSZ);
		memramsz = PAGING64_MAX_RAMSZ;
	}
	for (sit = 0; sit < PAGING_MAX_MMSWP; sit++) {
		if (memswpsz[sit] > PAGING64_MAX_SWPSZ) {
			printf("SWAP%d size %lu exceeds %llu bytes, clamped\n",
			       sit, memswpsz[sit], PAGING64_MAX_SWPSZ);
			memswpsz[sit] = PAGING64_MAX_SWPSZ;
		}
	}
#endif

	if (init_memphy(&mram, memramsz, rdmflag) != 0) {
		printf("Cannot initialize RAM of %lu bytes\n", memramsz);
		exit(1);
	}

	/* init_memphy() fully resets each device (size 0 = valid empty device) */
	for (sit = 0; sit < PAGING_MAX_MMSWP; sit++) {
		if (init_memphy(&mswp[sit], memswpsz[sit], rdmflag) != 0) {
			printf("Cannot initialize SWAP%d of %lu bytes\n",
			       sit, memswpsz[sit]);
			exit(1);
		}
		mswp_ptr[sit] = &mswp[sit];
	}

#ifdef USE_DMA
	/* The DMA reads and writes the OS RAM directly through the system bus */
	if (mram.maxsz <= 0 ||
	    memory_bank_attach(mram.storage, (size_t)mram.maxsz) == MEMORY_BANK_INVALID) {
		printf("Cannot attach the RAM (%lu bytes) to the DMA bus\n", memramsz);
		return 1;
	}
	if (dma_service_start() != 0)
		return 1;
#endif

	/* In paging mode, system memory is passed to each PCB via the loader */
	struct mmpaging_ld_args *mm_ld_args =
		(struct mmpaging_ld_args *)calloc(1, sizeof(struct mmpaging_ld_args));

	mm_ld_args->timer_id = ld_event;
	mm_ld_args->mram = &mram;
	mm_ld_args->mswp = mswp_ptr;
	mm_ld_args->active_mswp = &mswp[0];
	mm_ld_args->active_mswp_id = 0;
#endif

	/*Khởi tạo Process Registry cho Kernel*/
	os.registry_lock = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init((pthread_mutex_t *)os.registry_lock, NULL);
    for (int j = 0; j < MAX_PROCESS_REGISTRY; j++) {
        os.proc_registry[j] = NULL;
    }

	/* Init scheduler */
	init_scheduler();

	/* Run loader and CPUs */
#ifdef MM_PAGING
	pthread_create(&ld, NULL, ld_routine, (void *)mm_ld_args);
#else
	pthread_create(&ld, NULL, ld_routine, (void *)ld_event);
#endif
	for (i = 0; i < num_cpus; i++) {
		pthread_create(&cpu[i], NULL, cpu_routine, (void *)&args[i]);
	}

	/* Wait for CPUs and loader to finish */
	for (i = 0; i < num_cpus; i++) {
		pthread_join(cpu[i], NULL);
	}
	pthread_join(ld, NULL);

	/* Every process has ended, and a process only ends after its DMA
	 * requests completed, so the DMA has nothing left to do. */
	dma_service_stop();
#ifdef USE_DMA
	memory_bank_cleanup();
#endif

	/* Stop timer */
	stop_timer();

	free(cpu);
	free(args);
#ifdef MM_PAGING
	/* RAM and swap contents: every thread is done with them */
	free(mram.storage);
	for (sit = 0; sit < PAGING_MAX_MMSWP; sit++)
		free(mswp[sit].storage);
	free(mm_ld_args);
#endif
	return 0;
}
