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
#include "mm.h"
#ifdef MM64
#include "mm64.h"
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* The co-processor (DMA/Verilator) is not part of the loader-CPU flow yet.
 * Compile with -DUSE_DMA to enable it. */
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

/*
 * CPU thread. Rules for the timer:
 *  - every slot, this thread must call next_slot() exactly once
 *  - when it exits, it must call detach_event()
 */

 //lấy proc mới
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
				/*Gỡ đăng ký trước khi free*/
				pthread_mutex_lock((pthread_mutex_t *)proc->krnl->registry_lock);
				for (int j = 0; j < MAX_PROCESS_REGISTRY; j++) {
					if (proc->krnl->proc_registry[j] == proc) {
						proc->krnl->proc_registry[j] = NULL;
						break;
					}
				}
            	pthread_mutex_unlock((pthread_mutex_t *)proc->krnl->registry_lock);

				/* The process has finished its job */
				printf("\tCPU %d: Processed %2d has finished\n",
				       id, proc->pid);
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

		/* 3. Run one instruction in this slot */
		run(proc);
		time_left--;
		next_slot(timer_id);
	}
	detach_event(timer_id);
	pthread_exit(NULL);
}

#ifdef USE_DMA
static void *co_processor(void *arg)
{
	(void)arg;
	/* TODO: Initialize the internal Verilator RTL simulation. */
	if (rtl_dma_init_simulation() != 0) {
		printf("Cannot initialize RTL simulation\n");
		return (void *)1;
	}
	/* TODO: asynchronous DMA handling (callback/IRQ) */
	return NULL;
}
#endif

//khởi tạo OS (kernel)
static void *ld_routine(void *args)
{
#ifdef MM_PAGING
	struct memphy_struct *mram = ((struct mmpaging_ld_args *)args)->mram;
	struct memphy_struct **mswp = ((struct mmpaging_ld_args *)args)->mswp;
	struct memphy_struct *active_mswp = ((struct mmpaging_ld_args *)args)->active_mswp;
	struct timer_id_t *timer_id = ((struct mmpaging_ld_args *)args)->timer_id;
#else
	struct timer_id_t *timer_id = (struct timer_id_t *)args;
#endif
	int i = 0;

#ifdef MM_PAGING
	/* init kernel page table directory (calloc: no garbage PTE) */
#ifdef MM64
	os.krnl_pgd = (addr_t *)calloc(PAGING64_MAX_PGN, sizeof(addr_t));
	os.krnl_p4d = NULL;
	os.krnl_pud = NULL;
	os.krnl_pmd = NULL;
	os.krnl_pt = NULL;
#else
	os.krnl_pgd = (uint32_t *)calloc(PAGING_MAX_PGN, sizeof(uint32_t));
#endif
	os.mram = mram;
	os.mswp = mswp;
	os.active_mswp = active_mswp;
	os.mm = (struct mm_struct *)calloc(1, sizeof(struct mm_struct));
	init_mm(os.mm, NULL);
#endif
	printf("ld_routine\n");

	while (i < num_processes) {
		struct pcb_t *proc = load(ld_processes.path[i]);

		/* sched.c uses proc->krnl, so this must always be set */
		proc->krnl = &os;

		/*Đăng ký tiến trình mới vào Kernel Registry*/
		pthread_mutex_lock((pthread_mutex_t *)os.registry_lock);
		for (int j = 0; j < MAX_PROCESS_REGISTRY; j++) {
			if (os.proc_registry[j] == NULL) {
				os.proc_registry[j] = proc;
				break;
			}
		}
		pthread_mutex_unlock((pthread_mutex_t *)os.registry_lock);

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
		init_mm(proc->mm, proc);
#endif
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

//đọc file config để khởi tạo cấu hình
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
#ifdef USE_DMA
	pthread_t dma;
#endif

	/* Init timer: one event per thread (each CPU + loader) */
	for (i = 0; i < num_cpus; i++) {
		args[i].timer_id = attach_event();
		args[i].id = i;
	}
	struct timer_id_t *ld_event = attach_event();
	start_timer();

#ifdef MM_PAGING
	/* Init all MEMPHY: 1 MEMRAM and n MEMSWP */
	int rdmflag = 1; /* random access memory */

	struct memphy_struct mram;
	struct memphy_struct mswp[PAGING_MAX_MMSWP];

	init_memphy(&mram, memramsz, rdmflag);

	int sit;
	for (sit = 0; sit < PAGING_MAX_MMSWP; sit++)
		init_memphy(&mswp[sit], memswpsz[sit], rdmflag);

	/* In paging mode, system memory is passed to each PCB via the loader */
	struct mmpaging_ld_args *mm_ld_args =
		(struct mmpaging_ld_args *)calloc(1, sizeof(struct mmpaging_ld_args));

	mm_ld_args->timer_id = ld_event;
	mm_ld_args->mram = (struct memphy_struct *)&mram;
	mm_ld_args->mswp = (struct memphy_struct **)&mswp;
	mm_ld_args->active_mswp = (struct memphy_struct *)&mswp[0];
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

#ifdef USE_DMA
	/* Run co-processor */
	pthread_create(&dma, NULL, co_processor, NULL);
#endif

	/* Wait for CPUs and loader to finish */
	for (i = 0; i < num_cpus; i++) {
		pthread_join(cpu[i], NULL);
	}
	pthread_join(ld, NULL);
#ifdef USE_DMA
	pthread_join(dma, NULL);
#endif

	/* Stop timer */
	stop_timer();

	free(cpu);
	free(args);
#ifdef MM_PAGING
	free(mm_ld_args);
#endif
	return 0;
}
