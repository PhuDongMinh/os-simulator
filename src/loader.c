/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * Loader: read a program file from disk and build a PCB.
 *
 * Program file format (line based):
 *     [priority] [N = number of instructions]
 *     instruction 0
 *     ...
 *     instruction N-1
 * Blank lines are skipped, '#' starts a comment, opcodes are case
 * insensitive, arguments are decimal (or hexadecimal with a 0x prefix).
 */
#include "loader.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LINE_LEN  512
#define NUM_REGS  10          /* size of pcb_t.regs[] */
#define MAX_INSTS 1000000     /* sanity limit for N */
#define MAX_ARGS  4           /* inst_t has arg_0 .. arg_3 */

static uint32_t avail_pid = 1;

/* One row per opcode: its name and how many arguments it accepts. */
struct op_spec {
	const char *name;
	enum ins_opcode_t opcode;
	int min_args;
	int max_args;
};

static const struct op_spec op_table[] = {
	{ "calc",           CALC,           0, 0 },
	{ "alloc",          ALLOC,          2, 2 },  /* alloc [size] [reg]  */
	{ "free",           FREE,           1, 1 },  /* free [reg]          */
	{ "read",           READ,           3, 3 },  /* read [src] [off] [dst] */
	{ "write",          WRITE,          3, 3 },  /* write [data] [dst] [off] */
	{ "kmalloc",        KMALLOC,        2, 2 },  /* kmalloc [size] [reg] */
	{ "copy_from_user", COPY_FROM_USER, 4, 4 },
	{ "copy_to_user",   COPY_TO_USER,   4, 4 },
	{ "dma_memcpy",     DMA_MEMCPY,     4, 4 },
	{ "syscall",        SYSCALL,        1, 4 },  /* syscall [nr] [args..] */
};
#define NUM_OPS ((int)(sizeof(op_table) / sizeof(op_table[0])))

static void die(const char *path, int line, const char *msg)
	__attribute__((noreturn));

static void die(const char *path, int line, const char *msg)
{
	if (line > 0)
		printf("Loader error: %s:%d: %s\n", path, line, msg);
	else
		printf("Loader error: %s: %s\n", path, msg);
	exit(1);
}

/*
 * Read the next meaningful line: strips comments, skips blank lines.
 * Returns a pointer to the first non-blank character, or NULL at EOF.
 */
static char *next_line(FILE *f, char *buf, size_t len, int *lineno,
		       const char *path)
{
	while (fgets(buf, len, f) != NULL) {
		(*lineno)++;
		if (strchr(buf, '\n') == NULL && !feof(f))
			die(path, *lineno, "line too long");

		char *hash = strchr(buf, '#');
		if (hash != NULL)
			*hash = '\0';

		char *p = buf;
		while (isspace((unsigned char)*p))
			p++;
		if (*p != '\0')
			return p;
	}
	return NULL;
}

/*
 * Parse the arguments in "s" into args[]. Returns the number of arguments,
 * or -1 on error with *err describing the problem.
 */
static int parse_args(const char *s, arg_t *args, int min_args, int max_args,
		      const char **err)
{
	int n = 0;
	const char *p = s;

	while (1) {
		while (isspace((unsigned char)*p))
			p++;
		if (*p == '\0')
			break;

		if (n == max_args) {
			*err = "too many arguments";
			return -1;
		}
		if (*p == '-') {
			*err = "negative argument is not allowed";
			return -1;
		}

		char *end;
		int base = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? 16 : 10;

		errno = 0;
		unsigned long long v = strtoull(p, &end, base);
		if (end == p || (*end != '\0' && !isspace((unsigned char)*end))) {
			*err = "argument is not a number";
			return -1;
		}
		if (errno == ERANGE || v > (unsigned long long)((arg_t)-1)) {
			*err = "argument is out of range";
			return -1;
		}
		args[n++] = (arg_t)v;
		p = end;
	}

	if (n < min_args) {
		*err = "too few arguments";
		return -1;
	}
	return n;
}

static const struct op_spec *find_op(const char *name)
{
	int i;

	for (i = 0; i < NUM_OPS; i++)
		if (strcasecmp(name, op_table[i].name) == 0)
			return &op_table[i];
	return NULL;
}

/* Register operands must be in [0, NUM_REGS), or run() would index
 * proc->regs[] out of bounds. Returns an error message or NULL. */
static const char *check_regs(const struct inst_t *ins)
{
	switch (ins->opcode) {
	case ALLOC:
	case KMALLOC:
		if (ins->arg_1 >= NUM_REGS)
			return "register index must be 0..9";
		break;
	case FREE:
		if (ins->arg_0 >= NUM_REGS)
			return "register index must be 0..9";
		break;
	case READ:   /* read [src reg] [offset] [dst reg] */
		if (ins->arg_0 >= NUM_REGS || ins->arg_2 >= NUM_REGS)
			return "register index must be 0..9";
		break;
	case WRITE:  /* write [data] [dst reg] [offset] */
		if (ins->arg_1 >= NUM_REGS)
			return "register index must be 0..9";
		break;
	default:
		break;
	}
	return NULL;
}

struct pcb_t *load(const char *path)
{
	FILE *file;
	char buf[LINE_LEN];
	char *line;
	int lineno = 0;
	const char *err = NULL;

	if ((file = fopen(path, "r")) == NULL) {
		printf("Cannot find process description at '%s'\n", path);
		exit(1);
	}

	/* ---- Header: [priority] [N] ---- */
	arg_t hdr[2];

	line = next_line(file, buf, sizeof(buf), &lineno, path);
	if (line == NULL)
		die(path, 0, "empty file, expected '[priority] [N]'");
	if (parse_args(line, hdr, 2, 2, &err) < 0)
		die(path, lineno, err);
	if (hdr[0] > UINT32_MAX || hdr[1] > MAX_INSTS)
		die(path, lineno, "priority or number of instructions too large");

	/* ---- Create the PCB (calloc: no garbage in regs, pointers, ...) ---- */
	struct pcb_t *proc = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));

	proc->pid = avail_pid++;
	proc->priority = (uint32_t)hdr[0];
#ifdef MLQ_SCHED
	proc->prio = proc->priority; /* default, overwritten by the config file */
#endif
	proc->bp = PAGE_SIZE;
	proc->pc = 0;
	proc->page_table =
		(struct page_table_t *)calloc(1, sizeof(struct page_table_t));
	snprintf(proc->path, sizeof(proc->path), "%s", path);

	proc->code = (struct code_seg_t *)calloc(1, sizeof(struct code_seg_t));
	proc->code->size = (uint32_t)hdr[1];
	/* at least 1 element so that text is never NULL */
	proc->code->text = (struct inst_t *)calloc(
		proc->code->size ? proc->code->size : 1, sizeof(struct inst_t));

	/* ---- Instructions ---- */
	uint32_t i;

	for (i = 0; i < proc->code->size; i++) {
		struct inst_t *ins = &proc->code->text[i];
		char name[32];
		size_t n = 0;

		line = next_line(file, buf, sizeof(buf), &lineno, path);
		if (line == NULL) {
			printf("Loader error: %s: expected %u instructions, "
			       "found only %u\n", path, proc->code->size, i);
			exit(1);
		}

		/* opcode = first token */
		while (line[n] != '\0' && !isspace((unsigned char)line[n]))
			n++;
		if (n >= sizeof(name))
			die(path, lineno, "unknown opcode");
		memcpy(name, line, n);
		name[n] = '\0';

		const struct op_spec *spec = find_op(name);

		if (spec == NULL) {
			printf("Loader error: %s:%d: unknown opcode '%s'\n",
			       path, lineno, name);
			exit(1);
		}
		ins->opcode = spec->opcode;

		/* arguments = rest of the line */
		arg_t args[MAX_ARGS] = { 0 };

		if (parse_args(line + n, args, spec->min_args, spec->max_args,
			       &err) < 0) {
			printf("Loader error: %s:%d: '%s': %s\n",
			       path, lineno, spec->name, err);
			exit(1);
		}
		ins->arg_0 = args[0];
		ins->arg_1 = args[1];
		ins->arg_2 = args[2];
		ins->arg_3 = args[3];

		if ((err = check_regs(ins)) != NULL) {
			printf("Loader error: %s:%d: '%s': %s\n",
			       path, lineno, spec->name, err);
			exit(1);
		}
	}

	fclose(file);
	return proc;
}
