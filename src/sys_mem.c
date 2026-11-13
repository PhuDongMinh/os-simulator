/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "os-mm.h"
#include "syscall.h"
#include "libmem.h"
#include "queue.h"
#include <stdlib.h>

#ifdef MM64
#include "mm64.h"
#else
#include "mm.h"
#endif

// typedef char BYTE;
struct pcb_t *get_pcb_by_pid(struct krnl_t *krnl, uint32_t pid)
{
        if (krnl == NULL) return NULL;
        
        struct pcb_t *target = NULL;

        pthread_mutex_lock((pthread_mutex_t *)krnl->registry_lock);
        for (int i = 0; i < MAX_PROCESS_REGISTRY; i++)
        {
                if (krnl->proc_registry[i] != NULL && krnl->proc_registry[i]->pid == pid)
                {
                        target = krnl->proc_registry[i];
                        break;
                }
        }
        pthread_mutex_unlock((pthread_mutex_t *)krnl->registry_lock);

        return target;
}

int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
        int memop = regs->a1;
        BYTE value;
        /*
         * TODO THIS DUMMY CREATE EMPTY PROC TO AVOID COMPILER NOTIFY
         *      need to be eliminated
         */

        //    struct pcb_t *caller = malloc(sizeof(struct pcb_t));
        //    caller->krnl = malloc(sizeof(struct krnl_t));

        /*
         * @bksysnet: Please note in the dual spacing design
         *            syscall implementations are in kernel space.
         */

        /* TODO: Traverse proclist to terminate the proc
         *       stcmp to check the process match proc_name
         */

        /* TODO Maching and marking the process */
        /* user process are not allowed to access directly pcb in kernel space of syscall */
        //....

        // Xoa doan cap phat PCB dummy
        struct pcb_t *caller = get_pcb_by_pid(krnl, pid);
        // Tu choi truy cap neu khong tim thay tien trinh hop le
        if (caller == NULL)
        {
                printf("[Kernel Error] sys_memmap failed: Unauthorized or invalid PID %u\n", pid);
                return -1;
        }

        printf("[SYSCALL] sys_memmap found caller pid=%d\n", caller->pid);

        // Xu ly cac tac vu
        int stat = 0;
        switch (memop)
        {
        case SYSMEM_MAP_OP:
                /* Reserved process case*/
                printf("[SYSCALL] sys_memmap MAP pid=%d start=%lu size=%lu\n", caller->pid, regs->a2, regs->a3);
                stat = vmap_pgd_memset(caller, regs->a2, regs->a3);
                break;
        case SYSMEM_INC_OP:
                printf("[SYSCALL] sys_memmap INC pid=%d a2=%lu a3=%lu\n", caller->pid, regs->a2, regs->a3);
                stat = inc_vma_limit(caller, regs->a2, regs->a3);
                break;
        case SYSMEM_SWP_OP:
                printf("[SYSCALL] sys_memmap SWAP pid=%d pgn=%lu swp=%lu\n", caller->pid, regs->a2, regs->a3);
                stat = __mm_swap_page(caller, regs->a2, regs->a3);
                break;
        case SYSMEM_IO_READ:
                stat = MEMPHY_read(krnl->mram, regs->a2, &value);
                regs->a3 = value;  //Tra du lieu doc duoc qua reg a3
                printf("[SYSCALL] sys_memmap IO_READ pid=%d addr=%lu value=%u\n", caller->pid, regs->a2, value);
                break;
        case SYSMEM_IO_WRITE:
                stat = MEMPHY_write(krnl->mram, regs->a2, regs->a3);
                printf("[SYSCALL] sys_memmap IO_WRITE pid=%d addr=%lu value=%lu\n", caller->pid, regs->a2, regs->a3);
                break;
        default:
                printf("[SYSCALL] sys_memmap UNKNOWN pid=%d memop=%d a2=%lu a3=%lu\n", caller->pid, memop, regs->a2, regs->a3);
                stat = -1;
                break;
        }

        return stat;
}