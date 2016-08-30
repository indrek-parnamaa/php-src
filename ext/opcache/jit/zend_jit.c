/*
   +----------------------------------------------------------------------+
   | Zend JIT                                                             |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2016 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

#include <ZendAccelerator.h>
#include "Zend/zend_execute.h"
#include "zend_smart_str.h"
#include "jit/zend_jit.h"

#ifdef HAVE_JIT

#include "Optimizer/zend_func_info.h"
#include "Optimizer/zend_ssa.h"
#include "Optimizer/zend_inference.h"
#include "Optimizer/zend_dump.h"


#define ZEND_JIT_LEVEL_NONE   0     /* no JIT */
#define ZEND_JIT_LEVEL_MINI   1     /* minimal JIT (subroutine threading) */
#define ZEND_JIT_LEVEL_INLINE 2     /* selective inline threading */
#define ZEND_JIT_LEVEL_FULL   3     /* optimized JIT based on Type-Inference */

#define ZEND_JIT_LEVEL  ZEND_JIT_LEVEL_FULL

//#define CONTEXT_THREDED_JIT

#define JIT_PREFIX      "JIT$"
#define JIT_STUB_PREFIX "JIT$$"

// TODO: define DASM_M_GROW and DASM_M_FREE to use CG(arena) ???

#include "dynasm/dasm_proto.h"

typedef struct _zend_jit_stub {
	const char *name;
	int (*stub)(dasm_State **Dst);
} zend_jit_stub;

#define JIT_STUB(name) \
	{JIT_STUB_PREFIX #name, zend_jit_ ## name ## _stub}

#include "dynasm/dasm_x86.h"
#include "jit/zend_jit_x86.c"
#include "jit/zend_jit_disasm_x86.c"
#include "jit/zend_jit_gdb.c"
#include "jit/zend_jit_perf_dump.c"
#ifdef HAVE_OPROFILE
# include "jit/zend_jit_oprofile.c"
#endif

#if _WIN32
# include <Windows.h>
#else
# include <sys/mman.h>
# if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#   define MAP_ANONYMOUS MAP_ANON
# endif
#endif

#define DASM_ALIGNMENT 16

static void *dasm_buf = NULL;
static void *dasm_ptr = NULL;
static void *dasm_end = NULL;

static zend_string *zend_jit_func_name(zend_op_array *op_array)
{
	smart_str buf = {0};

	if (op_array->function_name) {
		if (op_array->scope) {
			smart_str_appends(&buf, JIT_PREFIX);
			smart_str_appendl(&buf, ZSTR_VAL(op_array->scope->name), ZSTR_LEN(op_array->scope->name));
			smart_str_appends(&buf, "::");
			smart_str_appendl(&buf, ZSTR_VAL(op_array->function_name), ZSTR_LEN(op_array->function_name));
			smart_str_0(&buf);
			return buf.s;
		} else {
			smart_str_appends(&buf, JIT_PREFIX);
			smart_str_appendl(&buf, ZSTR_VAL(op_array->function_name), ZSTR_LEN(op_array->function_name));
			smart_str_0(&buf);
			return buf.s;
		}
	} else if (op_array->filename) {
		smart_str_appends(&buf, JIT_PREFIX);
		smart_str_appendl(&buf, ZSTR_VAL(op_array->filename), ZSTR_LEN(op_array->filename));
		smart_str_0(&buf);
		return buf.s;
	} else {
		return NULL;
	}
}

static void *dasm_link_and_encode(dasm_State    **dasm_state,
                                  zend_op_array  *op_array,
                                  zend_cfg       *cfg,
                                  const char     *name)
{
	size_t size;
	int ret;
	void *entry;
#if defined(HAVE_DISASM) || defined(HAVE_GDB) || defined(HAVE_OPROFILE) || defined(HAVE_PERFTOOLS)
	zend_string *str = NULL;
#endif

	if (dasm_link(dasm_state, &size) != DASM_S_OK) {
		// TODO: dasm_link() failed ???
		return NULL;
	}

	if ((void*)((char*)dasm_ptr + size) > dasm_end) {
		// TODO: jit_buffer_size overflow ???
		return NULL;
	}

	ret = dasm_encode(dasm_state, dasm_ptr);

	if (ret != DASM_S_OK) {
		// TODO: dasm_encode() failed ???
		return NULL;
	}

	entry = dasm_ptr;
	dasm_ptr = (void*)((char*)dasm_ptr + ZEND_MM_ALIGNED_SIZE_EX(size, DASM_ALIGNMENT));

	if (op_array && cfg) {
		int b;

		for (b = 0; b < cfg->blocks_count; b++) {
#ifdef CONTEXT_THREDED_JIT
			if (cfg->blocks[b].flags & ZEND_BB_START) {
#else
			if (cfg->blocks[b].flags & (ZEND_BB_START|ZEND_BB_ENTRY|ZEND_BB_RECV_ENTRY)) {
#endif
				zend_op *opline = op_array->opcodes + cfg->blocks[b].start;

				opline->handler = (void*)(((char*)entry) +
					dasm_getpclabel(dasm_state, cfg->blocks_count + b));
			}
		}
	}

#if defined(HAVE_DISASM) || defined(HAVE_GDB) || defined(HAVE_OPROFILE) || defined(HAVE_PERFTOOLS)
    if (!name) {
		if (ZCG(accel_directives).jit_debug & (ZEND_JIT_DEBUG_ASM|ZEND_JIT_DEBUG_GDB|ZEND_JIT_DEBUG_OPROFILE|ZEND_JIT_DEBUG_PERF)) {
			str = zend_jit_func_name(op_array);
			if (str) {
				name = ZSTR_VAL(str);
			}
		}
	}
#endif

#ifdef HAVE_DISASM
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_ASM) {
		zend_jit_disasm_add_symbol(name, (uintptr_t)entry, size);
		zend_jit_disasm(
			name,
			(op_array && op_array->filename) ? ZSTR_VAL(op_array->filename) : NULL,
			op_array,
			cfg,
			entry,
			size);
	}
#endif

#ifdef HAVE_GDB
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_GDB) {
		if (name) {
			zend_jit_gdb_register(
					name,
					op_array,
					entry,
					size);
		}
	}
#endif

#ifdef HAVE_OPROFILE
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_OPROFILE) {
		zend_jit_oprofile_register(
			name,
			entry,
			size);
	}
#endif

#ifdef HAVE_PERFTOOLS
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_PERF) {
		if (name) {
			zend_jit_perf_dump(
				name,
				entry,
				size);
		}
	}
#endif

#if defined(HAVE_DISASM) || defined(HAVE_GDB) || defined(HAVE_OPROFILE) || defined(HAVE_PERFTOOLS)
	if (str) {
		zend_string_release(str);
	}
#endif

	return entry;
}

static size_t jit_page_size(void)
{
#ifdef _WIN32
	SYSTEM_INFO system_info;
	GetSystemInfo(&system_info);
	return system_info.dwPageSize;
#else
	return getpagesize();
#endif
}

static void *jit_alloc(size_t size, int shared)
{
#ifdef _WIN32
	return VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
#ifdef MAP_HUGETLB
	zend_ulong huge_page_size = 2 * 1024 * 1024;
#endif

	void *p;
	int prot;

# ifdef HAVE_MPROTECT
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_GDB) {
		prot = PROT_EXEC | PROT_READ | PROT_WRITE;
	} else {
		prot = PROT_NONE;
	}
# else
	prot = PROT_EXEC | PROT_READ | PROT_WRITE;
# endif

# ifdef MAP_HUGETLB
	if (size >= huge_page_size) {
		p = mmap(NULL, size, prot,
				(shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
		if (p != MAP_FAILED) {
			return (void*)p;
		}
	}
# endif
	p = mmap(NULL, size, prot,
			(shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
		return NULL;
	}

    return (void*)p;
#endif
}

static void jit_free(void *p, size_t size)
{
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
#else
	munmap(p, size);
#endif
}

static int zend_may_throw(zend_op *opline, zend_op_array *op_array, zend_ssa *ssa)
{
	uint32_t t1 = OP1_INFO();
	uint32_t t2 = OP2_INFO();

    if (opline->op1_type == IS_CV) {
		if (t1 & MAY_BE_UNDEF) {
			switch (opline->opcode) {
				case ZEND_UNSET_VAR:
				case ZEND_ISSET_ISEMPTY_VAR:
					if (opline->extended_value & ZEND_QUICK_SET) {
						break;
					}
				case ZEND_ISSET_ISEMPTY_DIM_OBJ:
				case ZEND_ISSET_ISEMPTY_PROP_OBJ:
				case ZEND_ASSIGN:
				case ZEND_ASSIGN_REF:
				case ZEND_BIND_GLOBAL:
				case ZEND_FETCH_DIM_IS:
				case ZEND_FETCH_OBJ_IS:
				case ZEND_SEND_REF:
					break;
				default:
					/* undefined variable warning */
					return 1;
			}
		}
    } else if (opline->op1_type & (IS_TMP_VAR|IS_VAR)) {
		if (t1 & (MAY_BE_OBJECT|MAY_BE_ARRAY_OF_OBJECT|MAY_BE_RESOURCE)) {
			switch (opline->opcode) {
				case ZEND_CASE:
				case ZEND_FE_FETCH_R:
				case ZEND_FE_FETCH_RW:
				case ZEND_FETCH_LIST:
				case ZEND_QM_ASSIGN:
				case ZEND_SEND_VAL:
				case ZEND_SEND_VAL_EX:
				case ZEND_SEND_VAR:
				case ZEND_SEND_VAR_EX:
				case ZEND_SEND_VAR_NO_REF:
				case ZEND_SEND_VAR_NO_REF_EX:
				case ZEND_SEND_REF:
				case ZEND_SEPARATE:
					break;
				default:
					/* destructor may be called */
					return 1;
			}
		}
    }

    if (opline->op2_type == IS_CV) {
		if (t2 & MAY_BE_UNDEF) {
			switch (opline->opcode) {
				case ZEND_ASSIGN_REF:
					break;
				default:
					/* undefined variable warning */
					return 1;
			}
		}
	} else if (opline->op2_type & (IS_TMP_VAR|IS_VAR)) {
		if (t2 & (MAY_BE_OBJECT|MAY_BE_ARRAY_OF_OBJECT|MAY_BE_RESOURCE)) {
			switch (opline->opcode) {
				case ZEND_ASSIGN:
					break;
				default:
					/* destructor may be called */
					return 1;
			}
		}
    }

	switch (opline->opcode) {
		case ZEND_NOP:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_QM_ASSIGN:
		case ZEND_JMP:
		case ZEND_CHECK_VAR:
		case ZEND_MAKE_REF:
		case ZEND_SEND_VAR:
		case ZEND_BEGIN_SILENCE:
		case ZEND_END_SILENCE:
		case ZEND_SEND_VAL:
		case ZEND_SEND_REF:
		case ZEND_FREE:
		case ZEND_SEPARATE:
			return 0;
		case ZEND_BIND_GLOBAL:
			if ((opline+1)->opcode == ZEND_BIND_GLOBAL) {
				return zend_may_throw(opline + 1, op_array, ssa);
			}
			return 0;
		case ZEND_ADD:
			if ((t1 & MAY_BE_ANY) == MAY_BE_ARRAY
			 && (t2 & MAY_BE_ANY) == MAY_BE_ARRAY) {
				return 0;
			}
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_DIV:
		case ZEND_MOD:
			if (opline->op2_type != IS_CONST
			 || Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_NULL
			 || Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_FALSE
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_LONG
			  && Z_LVAL_P(RT_CONSTANT(op_array, opline->op2)) == 0)
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_DOUBLE
			  && Z_DVAL_P(RT_CONSTANT(op_array, opline->op2)) == 0.0)) {
				return 1;
			}
			/* break missing intentionally */
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_POW:
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_SL:
		case ZEND_SR:
			if (opline->op2_type != IS_CONST
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_LONG
			  && (zend_ulong)Z_LVAL_P(RT_CONSTANT(op_array, opline->op2)) >= SIZEOF_ZEND_LONG * 8)
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_DOUBLE
			  && (zend_ulong)Z_DVAL_P(RT_CONSTANT(op_array, opline->op2)) >= SIZEOF_ZEND_LONG * 8)) {
				return 1;
			}
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_CONCAT:
		case ZEND_FAST_CONCAT:
			return (t1 & (MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_BW_OR:
		case ZEND_BW_AND:
		case ZEND_BW_XOR:
			if ((t1 & MAY_BE_ANY) == MAY_BE_STRING
			 && (t2 & MAY_BE_ANY) == MAY_BE_STRING) {
				return 0;
			}
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_BW_NOT:
			return (t1 & (MAY_BE_NULL|MAY_BE_FALSE|MAY_BE_TRUE|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_BOOL_NOT:
		case ZEND_PRE_INC:
		case ZEND_POST_INC:
		case ZEND_PRE_DEC:
		case ZEND_POST_DEC:
		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZNZ:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
		case ZEND_BOOL:
			return (t1 & MAY_BE_OBJECT);
		case ZEND_BOOL_XOR:
			return (t1 & MAY_BE_OBJECT) || (t2 & MAY_BE_OBJECT);
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_CASE:
			if ((t1 & MAY_BE_ANY) == MAY_BE_NULL
			 || (t2 & MAY_BE_ANY) == MAY_BE_NULL) {
				return 0;
			}
			return (t1 & MAY_BE_OBJECT) || (t2 & MAY_BE_OBJECT);
		case ZEND_ASSIGN_ADD:
			if (opline->extended_value != 0) {
				return 1;
			}
			if ((t1 & MAY_BE_ANY) == MAY_BE_ARRAY
			 && (t2 & MAY_BE_ANY) == MAY_BE_ARRAY) {
				return 0;
			}
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_ASSIGN_DIV:
		case ZEND_ASSIGN_MOD:
			if (opline->extended_value != 0) {
				return 1;
			}
			if (opline->op2_type != IS_CONST
			 || Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_NULL
			 || Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_FALSE
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_LONG
			  && Z_LVAL_P(RT_CONSTANT(op_array, opline->op2)) == 0)
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_DOUBLE
			  && Z_DVAL_P(RT_CONSTANT(op_array, opline->op2)) == 0.0)) {
				return 1;
			}
			/* break missing intentionally */
		case ZEND_ASSIGN_SUB:
		case ZEND_ASSIGN_MUL:
		case ZEND_ASSIGN_POW:
			if (opline->extended_value != 0) {
				return 1;
			}
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_ASSIGN_SL:
		case ZEND_ASSIGN_SR:
			if (opline->extended_value != 0) {
				return 1;
			}
			if (opline->op2_type != IS_CONST
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_LONG
			  && (zend_ulong)Z_LVAL_P(RT_CONSTANT(op_array, opline->op2)) >= SIZEOF_ZEND_LONG * 8)
			 || (Z_TYPE_P(RT_CONSTANT(op_array, opline->op2)) == IS_DOUBLE
			  && (zend_ulong)Z_DVAL_P(RT_CONSTANT(op_array, opline->op2)) >= SIZEOF_ZEND_LONG * 8)) {
				return 1;
			}
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_ASSIGN_CONCAT:
			if (opline->extended_value != 0) {
				return 1;
			}
			return (t1 & (MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_ASSIGN_BW_OR:
		case ZEND_ASSIGN_BW_AND:
		case ZEND_ASSIGN_BW_XOR:
			if (opline->extended_value != 0) {
				return 1;
			}
			if ((t1 & MAY_BE_ANY) == MAY_BE_STRING
			 && (t2 & MAY_BE_ANY) == MAY_BE_STRING) {
				return 0;
			}
			return (t1 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT)) ||
				(t2 & (MAY_BE_STRING|MAY_BE_ARRAY|MAY_BE_OBJECT));
		case ZEND_ASSIGN:
			return (t1 & (MAY_BE_OBJECT|MAY_BE_ARRAY_OF_OBJECT|MAY_BE_RESOURCE));
		case ZEND_ROPE_INIT:
		case ZEND_ROPE_ADD:
			return t2 & (MAY_BE_ARRAY|MAY_BE_OBJECT);
		case ZEND_INIT_ARRAY:
		case ZEND_ADD_ARRAY_ELEMENT:
			return t2 & (MAY_BE_ARRAY|MAY_BE_OBJECT|MAY_BE_RESOURCE);
		case ZEND_STRLEN:
			return (t1 & MAY_BE_ANY) != MAY_BE_STRING;
		default:
			return 1;
	}
}

ZEND_API int zend_jit(zend_op_array *op_array, zend_script *script)
{
	uint32_t flags = 0;
	zend_ssa ssa;
	void *checkpoint;
	int b, i, end;
	zend_op *opline;
	dasm_State* dasm_state = NULL;
	void *handler;

	if (!dasm_buf) {
		return FAILURE;
	}

	checkpoint = zend_arena_checkpoint(CG(arena));

    /* Build SSA */
	memset(&ssa, 0, sizeof(zend_ssa));

	if (zend_build_cfg(&CG(arena), op_array, ZEND_CFG_STACKLESS | ZEND_CFG_RECV_ENTRY | ZEND_RT_CONSTANTS | ZEND_SSA_RC_INFERENCE, &ssa.cfg, &flags) != SUCCESS) {
		goto jit_failure;
	}

	if (zend_cfg_build_predecessors(&CG(arena), &ssa.cfg) != SUCCESS) {
		goto jit_failure;
	}

	/* Compute Dominators Tree */
	if (zend_cfg_compute_dominators_tree(op_array, &ssa.cfg) != SUCCESS) {
		goto jit_failure;
	}

	/* Identify reducible and irreducible loops */
	if (zend_cfg_identify_loops(op_array, &ssa.cfg, &flags) != SUCCESS) {
		goto jit_failure;
	}

	if ((ZEND_JIT_LEVEL >= ZEND_JIT_LEVEL_FULL)
	 && ssa.cfg.blocks
	 && op_array->last_try_catch == 0
	 && !(op_array->fn_flags & ZEND_ACC_GENERATOR)
	 && !(flags & ZEND_FUNC_INDIRECT_VAR_ACCESS)) {
		if (zend_build_ssa(&CG(arena), script, op_array, ZEND_RT_CONSTANTS | ZEND_SSA_RC_INFERENCE, &ssa, &flags) != SUCCESS) {
			goto jit_failure;
		}

		if (zend_ssa_compute_use_def_chains(&CG(arena), op_array, &ssa) != SUCCESS) {
			goto jit_failure;
		}

		if (zend_ssa_find_false_dependencies(op_array, &ssa) != SUCCESS) {
			goto jit_failure;
		}

		if (zend_ssa_inference(&CG(arena), op_array, script, &ssa) != SUCCESS) {
			goto jit_failure;
		}

		if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_SSA) {
			zend_dump_op_array(op_array, ZEND_DUMP_HIDE_UNREACHABLE|ZEND_DUMP_RC_INFERENCE|ZEND_DUMP_SSA|ZEND_DUMP_RT_CONSTANTS, "JIT", &ssa);
		}
	} else {
		ssa.rt_constants = 1;
	}

	/* mark hidden branch targets */
	for (b = 0; b < ssa.cfg.blocks_count; b++) {
		if (ssa.cfg.blocks[b].flags & ZEND_BB_REACHABLE &&
		    ssa.cfg.blocks[b].len > 1) {

			opline = op_array->opcodes + ssa.cfg.blocks[b].start + ssa.cfg.blocks[b].len - 1;
			if (opline->opcode == ZEND_DO_FCALL &&
			    (opline-1)->opcode == ZEND_NEW) {
				ssa.cfg.blocks[ssa.cfg.blocks[b].successors[0]].flags |= ZEND_BB_TARGET;
			}
		}
	}

	dasm_init(&dasm_state, DASM_MAXSECTION);
	dasm_setupglobal(&dasm_state, dasm_labels, zend_lb_MAX);
	dasm_setup(&dasm_state, dasm_actions);

	dasm_growpc(&dasm_state, ssa.cfg.blocks_count * 2);

	zend_jit_align_func(&dasm_state);
	for (b = 0; b < ssa.cfg.blocks_count; b++) {
		if ((ssa.cfg.blocks[b].flags & ZEND_BB_REACHABLE) == 0) {
			continue;
		}
#ifdef CONTEXT_THREDED_JIT
		if (ssa.cfg.blocks[b].flags & ZEND_BB_START) {
			zend_jit_label(&dasm_state, ssa.cfg.blocks_count + b);
			zend_jit_prologue(&dasm_state);
		}
#else
		if (ssa.cfg.blocks[b].flags & ZEND_BB_ENTRY) {
			if (ssa.cfg.blocks[b].flags & ZEND_BB_TARGET) {
				zend_jit_jmp(&dasm_state, b);
			}
			zend_jit_label(&dasm_state, ssa.cfg.blocks_count + b);
			zend_jit_prologue(&dasm_state);
		} else if (ssa.cfg.blocks[b].flags & (ZEND_BB_START|ZEND_BB_RECV_ENTRY)) {
			opline = op_array->opcodes + ssa.cfg.blocks[b].start;
			if (ssa.cfg.split_at_recv && opline->opcode == ZEND_RECV_INIT) {
				if (opline > op_array->opcodes &&
				    (opline-1)->opcode == ZEND_RECV_INIT) {
					/* repeatable opcode */
					continue;
				} else {
					for (i = 0; (opline+i)->opcode == ZEND_RECV_INIT; i++) {
					}
					zend_jit_jmp(&dasm_state, b + i);
					zend_jit_label(&dasm_state, ssa.cfg.blocks_count + b);
					for (i = 1; (opline+i)->opcode == ZEND_RECV_INIT; i++) {
						zend_jit_label(&dasm_state, ssa.cfg.blocks_count + b + i);
					}
					zend_jit_prologue(&dasm_state);
				}
			} else {
				if (ssa.cfg.blocks[b].flags & (ZEND_BB_TARGET|ZEND_BB_RECV_ENTRY)) {
					zend_jit_jmp(&dasm_state, b);
				}
				zend_jit_label(&dasm_state, ssa.cfg.blocks_count + b);
				zend_jit_prologue(&dasm_state);
			}
		}
#endif
		zend_jit_label(&dasm_state, b);
		if (ssa.cfg.blocks[b].flags & ZEND_BB_LOOP_HEADER) {
			if (!zend_jit_check_timeout(&dasm_state)) {
				goto jit_failure;
			}
		}
		if (!ssa.cfg.blocks[b].len) {
			continue;
		}
		end = ssa.cfg.blocks[b].start + ssa.cfg.blocks[b].len - 1;
		for (i = ssa.cfg.blocks[b].start; i <= end; i++) {
			opline = op_array->opcodes + i;
			switch (opline->opcode) {
				case ZEND_RECV_INIT:
					if (ssa.cfg.split_at_recv) {
						if (!zend_jit_handler(&dasm_state, opline, zend_may_throw(opline, op_array, &ssa))) {
							goto jit_failure;
						}
						break;
					}
					/* break missing intentionally */
				case ZEND_BIND_GLOBAL:
					if (opline->opcode != op_array->opcodes[i+1].opcode) {
						/* repeatable opcodes */
						if (!zend_jit_handler(&dasm_state, opline, zend_may_throw(opline, op_array, &ssa))) {
							goto jit_failure;
						}
					}
					break;
				case ZEND_NOP:
					{
						uint32_t skip = 1;
						while (i < end && (opline + skip)->opcode == ZEND_NOP) {
							i++;
							skip++;
						}
						if (!zend_jit_skip_handler(&dasm_state, skip)) {
							goto jit_failure;
						}
					}
					break;
				case ZEND_OP_DATA:
					break;
				case ZEND_JMP:
					if (!zend_jit_set_opline(&dasm_state, OP_JMP_ADDR(opline, opline->op1)) ||
					    !zend_jit_jmp(&dasm_state, ssa.cfg.blocks[b].successors[0])) {
						goto jit_failure;
					}
					break;
				case ZEND_CATCH:
				case ZEND_FAST_CALL:
				case ZEND_FAST_RET:
				case ZEND_GENERATOR_CREATE:
				case ZEND_GENERATOR_RETURN:
				case ZEND_RETURN_BY_REF:
				case ZEND_RETURN:
				case ZEND_EXIT:
				/* switch through trampoline */
				case ZEND_YIELD:
				case ZEND_YIELD_FROM:
					if (!zend_jit_tail_handler(&dasm_state, opline)) {
						goto jit_failure;
					}
					break;
				/* stackless execution */
				case ZEND_INCLUDE_OR_EVAL:
				case ZEND_DO_FCALL:
				case ZEND_DO_UCALL:
				case ZEND_DO_FCALL_BY_NAME:
					if (!zend_jit_call(&dasm_state, opline)) {
						goto jit_failure;
					}
					break;
				case ZEND_JMPZ:
				case ZEND_JMPNZ:
					if (i != 0) {
						if ((opline-1)->opcode == ZEND_IS_EQUAL ||
						    (opline-1)->opcode == ZEND_IS_NOT_EQUAL ||
						    (opline-1)->opcode == ZEND_IS_SMALLER ||
						    (opline-1)->opcode == ZEND_IS_SMALLER_OR_EQUAL ||
						    (opline-1)->opcode == ZEND_CASE) {

							uint32_t t1 = _ssa_op1_info(op_array, &ssa, (opline-1));
							uint32_t t2 = _ssa_op2_info(op_array, &ssa, (opline-1));

							if ((t1 & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE))) ||
							    (t2 & (MAY_BE_ANY-(MAY_BE_LONG|MAY_BE_DOUBLE)))) {
								/* might be smart branch */
								if (!zend_jit_smart_branch(&dasm_state, opline, (b + 1), ssa.cfg.blocks[b].successors[0])) {
									goto jit_failure;
								}
								/* break missing intentionally */
							} else {
								/* smart branch */
								if (!zend_jit_cond_jmp(&dasm_state, opline + 1, ssa.cfg.blocks[b].successors[0])) {
									goto jit_failure;
								}
								break;
							}
						} else if ((opline-1)->opcode == ZEND_IS_IDENTICAL ||
						           (opline-1)->opcode == ZEND_IS_NOT_IDENTICAL ||
						           (opline-1)->opcode == ZEND_ISSET_ISEMPTY_VAR ||
						           (opline-1)->opcode == ZEND_ISSET_ISEMPTY_STATIC_PROP ||
						           (opline-1)->opcode == ZEND_ISSET_ISEMPTY_DIM_OBJ ||
						           (opline-1)->opcode == ZEND_ISSET_ISEMPTY_PROP_OBJ ||
						           (opline-1)->opcode == ZEND_INSTANCEOF ||
						           (opline-1)->opcode == ZEND_TYPE_CHECK ||
						           (opline-1)->opcode == ZEND_DEFINED) {
						    /* smart branch */
							if (!zend_jit_cond_jmp(&dasm_state, opline + 1, ssa.cfg.blocks[b].successors[0])) {
								goto jit_failure;
							}
							break;
						}
					}
					/* break missing intentionally */
				case ZEND_JMPZ_EX:
				case ZEND_JMPNZ_EX:
				case ZEND_JMP_SET:
				case ZEND_COALESCE:
				case ZEND_FE_RESET_R:
				case ZEND_FE_RESET_RW:
				case ZEND_ASSERT_CHECK:
					if (!zend_jit_handler(&dasm_state, opline, zend_may_throw(opline, op_array, &ssa)) ||
					    !zend_jit_cond_jmp(&dasm_state, opline + 1, ssa.cfg.blocks[b].successors[0])) {
						goto jit_failure;
					}
					break;
				case ZEND_NEW:
					if (!zend_jit_new(&dasm_state, opline)) {
						goto jit_failure;
					}
					if (EXPECTED(opline->extended_value == 0 && (opline+1)->opcode == ZEND_DO_FCALL)) {
						i++;
					}
					break;
				case ZEND_JMPZNZ:
					if (!zend_jit_handler(&dasm_state, opline, zend_may_throw(opline, op_array, &ssa)) ||
					    !zend_jit_cond_jmp(&dasm_state, OP_JMP_ADDR(opline, opline->op2), ssa.cfg.blocks[b].successors[1]) ||
					    !zend_jit_jmp(&dasm_state, ssa.cfg.blocks[b].successors[0])) {
						goto jit_failure;
					}
					break;
				case ZEND_FE_FETCH_R:
				case ZEND_FE_FETCH_RW:
				case ZEND_DECLARE_ANON_CLASS:
				case ZEND_DECLARE_ANON_INHERITED_CLASS:
				if (!zend_jit_handler(&dasm_state, opline, zend_may_throw(opline, op_array, &ssa)) ||
					    !zend_jit_cond_jmp(&dasm_state, opline + 1, ssa.cfg.blocks[b].successors[0])) {
						goto jit_failure;
					}
					break;
				default:
					if (!zend_jit_handler(&dasm_state, opline, zend_may_throw(opline, op_array, &ssa))) {
						goto jit_failure;
					}
			}
		}
	}

	handler = dasm_link_and_encode(&dasm_state, op_array, &ssa.cfg, NULL);
	if (!handler) {
		goto jit_failure;
	}
	dasm_free(&dasm_state);

	zend_arena_release(&CG(arena), checkpoint);
	return SUCCESS;

jit_failure:
    if (dasm_state) {
		dasm_free(&dasm_state);
    }
	zend_arena_release(&CG(arena), checkpoint);
	return FAILURE;
}

ZEND_API void zend_jit_unprotect(void)
{
#ifdef HAVE_MPROTECT
	if (!(ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_GDB)) {
		mprotect(dasm_buf, ((char*)dasm_end) - ((char*)dasm_buf), PROT_READ | PROT_WRITE);
	}
#endif
}

ZEND_API void zend_jit_protect(void)
{
#ifdef HAVE_MPROTECT
	if (!(ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_GDB)) {
		mprotect(dasm_buf, ((char*)dasm_end) - ((char*)dasm_buf), PROT_READ | PROT_EXEC);
	}
#endif
}

static int zend_jit_make_stubs(void)
{
	dasm_State* dasm_state = NULL;
	uint32_t i;

	dasm_init(&dasm_state, DASM_MAXSECTION);
	dasm_setupglobal(&dasm_state, dasm_labels, zend_lb_MAX);

	for (i = 0; i < sizeof(zend_jit_stubs)/sizeof(zend_jit_stubs[0]); i++) {
		dasm_setup(&dasm_state, dasm_actions);
		if (!zend_jit_stubs[i].stub(&dasm_state)) {
			return 0;
		}
		if (!dasm_link_and_encode(&dasm_state, NULL, NULL, zend_jit_stubs[i].name)) {
			return 0;
		}
	}
	dasm_free(&dasm_state);
	return 1;
}

ZEND_API int zend_jit_startup(size_t size)
{
	size_t page_size = jit_page_size();
	int shared = 1;
	void *buf;
	int ret;

#ifdef HAVE_GDB
	zend_jit_gdb_init();
#endif

#ifdef HAVE_OPROFILE
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_OPROFILE) {
		shared = 0;
		if (!zend_jit_oprofile_startup()) {
			// TODO: error reporting and cleanup ???
			return FAILURE;
		}
	}
#endif

	/* Round up to the page size, which should be a power of two.  */
	page_size = jit_page_size();

	if (!page_size || (page_size & (page_size - 1))) {
		abort();
	}

	size = ZEND_MM_ALIGNED_SIZE_EX(size, page_size);

	buf = jit_alloc(size, shared);

	if (!buf) {
		return FAILURE;
	}

	dasm_buf = dasm_ptr = buf;
	dasm_end = (void*)(((char*)dasm_buf) + size);

#ifdef HAVE_DISASM
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_ASM) {
		if (!zend_jit_disasm_init()) {
			// TODO: error reporting and cleanup ???
			return FAILURE;
		}
	}
#endif

	zend_jit_unprotect();
	ret = zend_jit_make_stubs();
	zend_jit_protect();

	if (!ret) {
		// TODO: error reporting and cleanup ???
		return FAILURE;
	}

	return SUCCESS;
}

ZEND_API void zend_jit_shutdown(void)
{
#ifdef HAVE_OPROFILE
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_OPROFILE) {
		zend_jit_oprofile_shutdown();
	}
#endif

#ifdef HAVE_GDB
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_GDB) {
		zend_jit_gdb_unregister();
	}
#endif

#ifdef HAVE_DISASM
	if (ZCG(accel_directives).jit_debug & ZEND_JIT_DEBUG_ASM) {
		zend_jit_disasm_shutdown();
	}
#endif

	if (dasm_buf) {
		jit_free(dasm_buf, ((char*)dasm_end) - ((char*)dasm_buf));
	}
}

#else /* HAVE_JIT */

ZEND_API int zend_jit(zend_op_array *op_array, zend_script *script)
{
	return FAILURE;
}

ZEND_API void zend_jit_unprotect(void)
{
}

ZEND_API void zend_jit_protect(void)
{
}

ZEND_API int zend_jit_startup(size_t size)
{
	return FAILURE;
}

ZEND_API void zend_jit_shutdown(void)
{
}

#endif /* HAVE_JIT */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 */