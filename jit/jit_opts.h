/**********************************************************************

  jit_opts.h -

  $Author$

  Copyright (C) 2014 Masahiro Ide

 **********************************************************************/

#ifndef JIT_OPTS_H
#define JIT_OPTS_H

// When `USE_CGEN` is defined,  we use c-generator as backend of jit compiler.
// If `USE_LLVM` is defined, use llvm as backend.
#define USE_CGEN 1
//#define USE_LLVM 1

#define DUMP_STACK_MAP 2 /* 0:disable, 1:dump, 2:verbose */
//#define DUMP_LLVM_IR   0 /* 0:disable, 1:dump, 2:dump non-optimized llvm ir */
#define DUMP_INST      1 /* 0:disable, 1:dump */
#define DUMP_LIR       1 /* 0:disable, 1:dump */
#define DUMP_CALL_STACK_MAP 1 /* 0:disable, 1:dump */

#define GWJIT_DUMP_COMPILE_LOG 2 /* 0:disable, 1:dump, 2:verbose */
#define GWJIT_USE_PCH          1 /* 0:none,    1:use pre-compiled header */
#define GWIR_MAX_TRACE_LENGTH 1024 /* max length of instructions gwjit compile */
#define GWIR_TRACE_INIT_SIZE  16   /* initial size of trace */

#define GWIT_CGEN_OPT_LEVEL "0"
#define GWIT_CGEN_DBG_LEVEL "3"

/* Initial buffer size of lir memory allocator */
#define LIR_COMPILE_DATA_BUFF_SIZE (512)

#endif /* end of include guard */
