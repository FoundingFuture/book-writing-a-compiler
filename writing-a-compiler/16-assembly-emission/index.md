---
title: "Assembly emission per operating system"
description: "How antic writes assembly files for ELF, Mach-O and COFF, with exported and hidden symbols, constructors and the licence notice, and links executables."
summary: "Directives and sections, the leading `_` on Mach-O symbols, `rip`-relative and `:lo12:` versus `@PAGEOFF` relocations, COFF specifics. Global and hidden symbols for dev mode and export, constructor sections and the `anti_licenses` notice. The driver's llvm-mc and lld command lines for each target, with the platform linker as a fallback."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:59:18+02:00
draft: false
weight: 160
tags: [compilers, assembly]
keywords: [assembly directives, symbol visibility, relocations, dev mode objects, constructor sections, licence notice, module-definition file, linker command line]
---

## Previously

[Chapter 15, The ARM64 back end]({{% relref "/programming/writing-a-compiler/15-arm64-back-end" %}}), completes the integer patterns for ARM64. It covers immediates and their encoding limits, condition flags, addressing modes and `adrp`, and calls under AAPCS64 and Apple's variant.

## The full pipeline

Chapter 3 compiled one program shape, `fn main() -> int { return N; }`, for macos-arm64 through a direct path. This chapter removes that path. The driver sends every program through lowering, the optimizer, instruction selection, register allocation and the emitter of `src/emit.c`. The assembler llvm-mc then assembles the file, and the platform linker links it with the runtime library.

```c
/* Lower and optimize the program and run the back end for the target:
   instruction selection, register allocation and emission. The dumps
   print the machine code instead, before allocation for --dump-select.
   Returns 0 with the assembly, 2 after a dump and 1 after an error. */
static int back_end(const struct options *o, struct module *tree,
                    const char *module, struct ir_module *program,
                    struct diagnostics *diags, struct text *assembly,
                    struct extras *extras)
{
    struct mach_function **functions;
    struct text out = {0};
    char error[200];
    bool dump = o->dump_select || o->dump_alloc;
    bool ok;
    int status = 1;
    size_t i;

    if (tree != NULL &&
        !lower_checked(o->input, tree, module, program, diags)) {
        return 1;
    }
    if (o->dev) {
        ir_optimize_module(program, module);
    } else {
        ir_optimize(program, module);
    }
    if (!dump && !o->assembly_only && !o->dev && o->lib == LIB_NONE &&
        !has_main(program, module)) {
        fprintf(stderr, "antic: %s: the program has no function `main`\n",
                o->input);
        return 1;
    }
    functions = calloc(program->function_count + 1, sizeof *functions);
    if (functions == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    ok = select_module(o->target, program, functions, error, sizeof error);
    for (i = 0; ok && !(o->dump_select && !o->dump_alloc) &&
                i < program->function_count;
         i++) {
        if (functions[i] != NULL) {
            ok = regalloc_function(o->target, functions[i], error,
                                   sizeof error);
        }
    }
    if (ok && dump) {
        for (i = 0; i < program->function_count; i++) {
            if (functions[i] != NULL) {
                mach_print(&out, target_desc(o->target), program,
                           functions[i]);
            }
        }
        fputs(text_cstr(&out), stdout);
        status = 2;
    } else if (ok) {
        ok = o->dev ? emit_module(assembly, o->target, program, functions,
                                  module, error, sizeof error)
                    : emit_program(assembly, o->target, program, functions,
                                   module, error, sizeof error);
        if (ok && o->dev && !has_main(program, module)) {
            status = 3;
        }
        if (ok && o->lib == LIB_SHARED) {
            emit_constructor(assembly, o->target, "anti_rt_init");
        }
        if (ok && extras->notice.length > 0 && status != 3 &&
            !o->assembly_only && o->lib != LIB_STATIC) {
            emit_licenses(assembly, o->target, extras->notice.data,
                          extras->notice.length);
        }
        for (i = 0; ok && i < program->function_count; i++) {
            const struct ir_function *f = program->functions[i];
            if (f->exported && !f->is_extern) {
                text_appendf(&extras->exports, "%s\n", f->name);
            }
        }
        status = !ok ? 1 : status == 3 ? 3 : 0;
    }
    if (!ok) {
        fprintf(stderr, "antic: %s\n", error);
    }
    for (i = 0; i < program->function_count; i++) {
        if (functions[i] != NULL) {
            mach_function_free(functions[i]);
            free(functions[i]);
        }
    }
    free(functions);
    text_free(&out);
    return status;
}
```

The dumps of chapters 12 to 15 stop before emission. Without a dump option, `emit_program` writes the assembly text of the whole program. A program without `main` stops with `the program has no function` and the name, because the runtime calls `main`. The option `-S` writes the assembly file of such a module without that check. The branches for `--dev`, for a shared library and for the licence notice follow in the sections on dev mode, constructors and the licence notice.

## Symbol names

The dumps print IR names such as `main.scale` and block labels such as `b1`. An assembly file needs the names of the object format. The printers of both back ends take a `struct names` with the target and the symbol of the current function. The dumps pass `NULL`.

```c
/* How printed machine code names symbols and block labels. The dumps pass
   no names and print IR names and the labels b0, b1. The emitter passes
   its target and the symbol of the function whose blocks it prints. */
struct names {
    enum target target;
    const char *function;
};
```

Three kinds of names occur. An Anti function takes the symbol form of chapter 9 from `mangle`. A C function and a runtime helper such as `__chkstk` take the symbol that a C compiler writes, with a leading `_` on Mach-O, as chapter 11 lists. A block label joins the function symbol and the block number.

```c
void c_symbol(struct text *out, enum target t, const char *name)
{
    text_appendf(out, "%s%s", infos[t].format == FORMAT_MACHO ? "_" : "",
                 name);
}

/* DESIGN: llvm-mc leaves a label that starts with L out of a Mach-O symbol
   table. It leaves one that starts with .L out of ELF and COFF tables. */
void block_label(struct text *out, enum target t, const char *function,
                 size_t block)
{
    text_appendf(out, "%s%s.b%zu",
                 infos[t].format == FORMAT_MACHO ? "L" : ".L", function, block);
}
```

The function `mach_function_symbol` picks the symbol of a function. An IR function without a module is a C function. The flag `exported` marks an `export fn`, which chapter 2 defines, and it takes the C symbol of its name as well.

```c
void mach_function_symbol(struct text *out, enum target t,
                          const struct ir_function *f)
{
    if (f->module == NULL || f->exported) {
        c_symbol(out, t, f->name);
    } else {
        mangle(out, t, f->module, f->name);
    }
}
```

The object files of llvm-mc 23.1.1 on the development Mac show the effect of the label prefixes. For macos-arm64, `nm` of Apple LLVM 21.0.0 lists `_anti.rt.main`, `_letters.main`, `_putchar` and `ltmp0` in the object of `letters.anti`, the program shown below. The symbol `ltmp0` comes from llvm-mc, and no block label appears. The ELF and COFF objects list no label that starts with `.L`. A label in the symbol table would appear in the output of `nm` beside the functions.

## Directives

An assembly file of antic starts with `.text`, the section of machine code. A Mach-O file first names the minimum macOS version with `.build_version macos, 11, 0`, as chapter 3 does. Every ARM64 function starts with `.p2align 2`, a 4-byte alignment for 4-byte instructions. An x86_64 function has no alignment directive.

An ELF file ends with an empty section `.note.GNU-stack`. The GNU ld manual states that an input file without that section may require an executable stack, depending on the target. It calls this "often a problem for hand crafted assembler files"[^1]. The section without the executable flag marks the stack as not executable[^1].

The function `emit` writes the file. Its parameter `one_module` selects dev mode, and the two entry points `emit_program` and `emit_module` set it. The global data that the first loop checks and `emit_data` writes belongs to chapter 19.

```c
static bool emit(struct text *out, enum target t, const struct ir_module *m,
                 struct mach_function **functions, const char *module,
                 bool one_module, char *error, size_t error_size)
{
    const struct target_info *info = target_info(t);
    size_t i;
    size_t j;

    /* An address takes the eight bytes at its offset, so it lies inside
       the data that holds it. A library file read from disk is the one
       source of a global that does not. */
    for (i = 0; i < m->global_count; i++) {
        const struct ir_global *g = m->globals[i];
        for (j = 0; j < g->reloc_count; j++) {
            if (g->relocs[j].offset + 8 > g->size) {
                snprintf(error, error_size,
                         "the address at %" PRIu64 " of `%s.%s` ends past "
                         "its %" PRIu64 " bytes", g->relocs[j].offset,
                         g->module, g->name, g->size);
                return false;
            }
        }
    }
    if (info->format == FORMAT_MACHO) {
        text_appendf(out, "    .build_version macos, %d, %d\n",
                     MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
    }
    text_append(out, "    .text\n");
    emit_entry(out, t, m, module);
    for (i = 0; i < m->function_count; i++) {
        if (functions[i] != NULL) {
            emit_function(out, t, m, functions[i], one_module);
        }
    }
    if (m->global_count > 0) {
        emit_data(out, t, m);
    }
    /* Without this note GNU ld may mark the stack executable. */
    if (info->format == FORMAT_ELF) {
        text_append(out, "    .section .note.GNU-stack,\"\",@progbits\n");
    }
    return true;
}

bool emit_program(struct text *out, enum target t, const struct ir_module *m,
                  struct mach_function **functions, const char *module,
                  char *error, size_t error_size)
{
    return emit(out, t, m, functions, module, false, error, error_size);
}

bool emit_module(struct text *out, enum target t, const struct ir_module *m,
                 struct mach_function **functions, const char *module,
                 char *error, size_t error_size)
{
    return emit(out, t, m, functions, module, true, error, error_size);
}
```

## Global symbols

The runtime calls the program through the global symbol `anti.rt.main`, the function `main` of the module path `anti.rt`, which chapter 11 describes. The function `emit_entry` defines it as a second name for the main module's `main` with `.set`. The module path `anti.rt` belongs to the runtime, and the driver refuses to compile a module with that path.

```c
/* The runtime reaches main through a second global name, RUNTIME_ENTRY
   of RUNTIME_MODULE, which .set defines. */
static void emit_entry(struct text *out, enum target t,
                       const struct ir_module *m, const char *module)
{
    struct text entry = {0};
    struct text main_symbol = {0};
    size_t i;

    for (i = 0; i < m->function_count; i++) {
        const struct ir_function *f = m->functions[i];
        if (!f->is_extern && f->module != NULL &&
            strcmp(f->module, module) == 0 && strcmp(f->name, "main") == 0) {
            mangle(&entry, t, RUNTIME_MODULE, RUNTIME_ENTRY);
            mangle(&main_symbol, t, module, "main");
            text_appendf(out, "    .globl %s\n", text_cstr(&entry));
            text_appendf(out, "    .set %s, %s\n", text_cstr(&entry),
                         text_cstr(&main_symbol));
        }
    }
    text_free(&entry);
    text_free(&main_symbol);
}
```

In a whole program every other Anti function is a local symbol, because one assembly file holds all of them. The function `emit_function` makes a symbol global with `.globl` in two cases. An `export fn` is global in every mode. In dev mode every function of the module is global, and every function except an `export fn` is hidden, as the section on dev mode objects explains. For a function with Windows unwind data it writes `.seh_proc` and `.seh_endproc`, which the section on COFF specifics describes.

```c
static void emit_function(struct text *out, enum target t,
                          const struct ir_module *m,
                          const struct mach_function *f, bool module)
{
    const struct target_desc *desc = target_desc(t);
    struct text symbol = {0};
    struct names names;
    size_t b;
    size_t i;

    mach_function_symbol(&symbol, t, f->ir);
    names.target = t;
    names.function = text_cstr(&symbol);
    if (f->ir->exported || module) {
        text_appendf(out, "    .globl %s\n", text_cstr(&symbol));
    }
    /* DESIGN: in an object of one module, every function is global for the
       other modules and hidden. A shared library then exports only the
       export fns. COFF has no hidden symbols, and the .def file of a DLL
       names its exports. */
    if (module && !f->ir->exported &&
        target_info(t)->format != FORMAT_COFF) {
        text_appendf(out, "    %s %s\n",
                     target_info(t)->format == FORMAT_MACHO ? ".private_extern"
                                                           : ".hidden",
                     text_cstr(&symbol));
    }
    /* An ARM64 instruction is 4 bytes, and a function starts on one. */
    if (target_info(t)->arch == ARCH_ARM64) {
        text_append(out, "    .p2align 2\n");
    }
    text_appendf(out, "%s:\n", text_cstr(&symbol));
    /* A function with Windows unwind data. llvm-mc writes its pdata and
       xdata from the .seh_ lines. */
    if (f->unwind) {
        text_appendf(out, "    .seh_proc %s\n", text_cstr(&symbol));
    }
    for (b = 0; b < f->block_count; b++) {
        block_label(out, t, text_cstr(&symbol), b);
        text_append(out, ":\n");
        for (i = 0; i < f->blocks[b].count; i++) {
            text_append(out, "    ");
            desc->print(out, m, &f->blocks[b].insts[i], &names);
            text_append(out, "\n");
        }
    }
    if (f->unwind) {
        text_append(out, "    .seh_endproc\n");
    }
    text_free(&symbol);
}
```

## Assembly files per object format

The test `emit_main.macos-arm64` writes the file of `main.anti` from chapter 1 with `antic -S` and compares it with `tests/dump/main.macos-arm64.s`.

```text
    .build_version macos, 11, 0
    .text
    .globl _anti.rt.main
    .set _anti.rt.main, _main.main
    .p2align 2
_main.scale:
L_main.scale.b0:
    mov x9, #6
    mul x0, x0, x9
    ret
    .p2align 2
_main.main:
L_main.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #7
    bl _main.scale
    ldp x29, x30, [sp], #16
    ret
```

The same program for linux-x86_64 ends with the GNU-stack section and uses the ELF names.

```text
    .text
    .globl anti.rt.main
    .set anti.rt.main, main.main
main.scale:
.Lmain.scale.b0:
    imulq $6, %rdi, %rax
    ret
main.main:
.Lmain.main.b0:
    pushq %rbp
    movq %rsp, %rbp
    movq $7, %rdi
    call main.scale
    popq %rbp
    ret
    .section .note.GNU-stack,"",@progbits
```

For windows-x86_64 every Anti symbol takes the `_A` form, and a function with a frame carries the unwind directives of the section on COFF specifics. The runtime entry is `_A4anti2rt_main`, with the segments `anti` and `rt` of its module path.

```text
    .text
    .globl _A4anti2rt_main
    .set _A4anti2rt_main, _A4main_main
_A4main_scale:
.L_A4main_scale.b0:
    imulq $6, %rcx, %rax
    ret
_A4main_main:
    .seh_proc _A4main_main
.L_A4main_main.b0:
    pushq %rbp
    .seh_pushreg %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    .seh_stackalloc 32
    .seh_endprologue
    movq $7, %rcx
    call _A4main_scale
    addq $32, %rsp
    popq %rbp
    ret
    .seh_endproc
```

The program `tests/programs/letters.anti` prints three letters in a loop. Its file for linux-x86_64 shows the jumps to block labels and the call of a C function by its C name.

```anti
extern fn putchar(c: i32) -> i32;

fn main() -> int
{
    let i = 0;
    while i < 3 do {
        putchar(65 + i as i32);
        i += 1;
    }
    return i;
}
```

```text
    .text
    .globl anti.rt.main
    .set anti.rt.main, letters.main
letters.main:
.Lletters.main.b0:
    pushq %rbp
    movq %rsp, %rbp
    subq $16, %rsp
    movq %rbx, 8(%rsp)
    movq $0, %rbx
.Lletters.main.b1:
    cmpq $3, %rbx
    jge .Lletters.main.b3
.Lletters.main.b2:
    movl %ebx, %eax
    movl %eax, %edi
    addl $65, %edi
    call putchar
    addq $1, %rbx
    jmp .Lletters.main.b1
.Lletters.main.b3:
    movq %rbx, %rax
    movq 8(%rsp), %rbx
    movq %rbp, %rsp
    popq %rbp
    ret
    .section .note.GNU-stack,"",@progbits
```

## Relocations

A reference to a symbol whose address the assembler does not know becomes a relocation, which the linker resolves. The spelling in the assembly file decides its type. The assembler llvm-mc 23.1.1 wrote these relocations for the files that antic writes for `letters.anti` and for `strings.anti` of chapter 19, on all six targets. The command `llvm-objdump -r` of Apple LLVM 21.0.0 printed the types.

| Reference | ELF | Mach-O | COFF |
|---|---|---|---|
| `call putchar` on x86_64 | `R_X86_64_PLT32` | `X86_64_RELOC_BRANCH` | `IMAGE_REL_AMD64_REL32` |
| `bl putchar` on ARM64 | `R_AARCH64_CALL26` | `ARM64_RELOC_BRANCH26` | `IMAGE_REL_ARM64_BRANCH26` |
| `leaq g(%rip), %rax` | `R_X86_64_PC32` | `X86_64_RELOC_SIGNED` | `IMAGE_REL_AMD64_REL32` |
| Page of a symbol on ARM64 | `R_AARCH64_ADR_PREL_PG_HI21` | `ARM64_RELOC_PAGE21` | `IMAGE_REL_ARM64_PAGEBASE_REL21` |
| Low 12 bits on ARM64 | `R_AARCH64_ADD_ABS_LO12_NC` | `ARM64_RELOC_PAGEOFF12` | `IMAGE_REL_ARM64_PAGEOFFSET_12A` |

A call or an address is relative to the program counter or to its page, and the low 12 bits complete a page. The code therefore stays position-independent, as chapter 11 requires. On x86_64 one spelling, `sym(%rip)`, serves all three formats. On ARM64 the Mach-O files need `sym@PAGE` and `sym@PAGEOFF`. For `arm64-apple-macos`, llvm-mc rejects the ELF form. It reports `ADR/ADRP relocations must be GOT relative` for `adrp x0, sym` and `unknown AArch64 fixup kind` for `:lo12:sym`. The ARM64 printer picks the spelling from the object format. The two branches for the GOT between them belong to chapter 20.

```c
        } else if (inst->op == A64_ADD &&
                   (o->kind == MACH_FUNC || o->kind == MACH_GLOBAL)) {
            /* The low 12 bits: sym@PAGEOFF on Mach-O, :lo12:sym on ELF,
               COFF and in the dumps. */
            text_append(out, macho ? "" : ":lo12:");
            print_operand(out, m, names, o);
            text_append(out, macho ? "@PAGEOFF" : "");
        } else if (inst->op == A64_LDRGOT && i == 1) {
            /* ldr x0, [x0, :got_lo12:sym] or [x0, sym@GOTPAGEOFF]. */
            text_append(out, "[");
            print_operand(out, m, names, o);
            text_append(out, macho ? ", " : ", :got_lo12:");
            print_operand(out, m, names, &inst->operands[2]);
            text_append(out, macho ? "@GOTPAGEOFF]" : "]");
            break;
        } else if (inst->op == A64_ADRP && i == 1 && o->got) {
            text_append(out, macho ? "" : ":got:");
            print_operand(out, m, names, o);
            text_append(out, macho ? "@GOTPAGE" : "");
        } else if (inst->op == A64_ADRP && i == 1) {
            print_operand(out, m, names, o);
            text_append(out, macho ? "@PAGE" : "");
```

The unit test `page_offsets` in `tests/unit/test_emit.c` checks `adrp x0, _main.helper@PAGE` and `add x0, x0, _main.helper@PAGEOFF`. The string literals of chapter 19 take the same form in `tests/dump/strings.macos-arm64.s`.

## COFF specifics

A COFF symbol of antic is a C identifier. The form of chapter 9 is `_A`, each segment of the module path after its length, `_` and the name, and it holds only letters, digits and `_`. The runtime file `rt/start.c` reaches the program with an assembler label on ELF and Mach-O, because `anti.rt.main` contains dots. MSVC compiles no assembler labels, so on Windows the file declares the COFF symbol `_A4anti2rt_main` directly.

```c
/* A symbol with a dot is not a C identifier, so the declaration names it
   with an assembler label. Mach-O adds '_' to C symbols, ELF does not.
   The COFF symbol is an identifier, and MSVC has no assembler labels. */
#if defined(_WIN32)
extern int64_t _A4anti2rt_main(struct anti_slice args, struct anti_slice env);
#define anti_main _A4anti2rt_main
#else
#if defined(__APPLE__)
#define ANTI_ENTRY_SYMBOL "_anti.rt.main"
#else
#define ANTI_ENTRY_SYMBOL "anti.rt.main"
#endif
extern int64_t anti_main(struct anti_slice args, struct anti_slice env)
    __asm__(ANTI_ENTRY_SYMBOL);
#endif
```

The entry receives the slices `args` and `env` of chapter 19, whose layout `struct anti_slice` describes. The unit test `runtime_entry` in `tests/unit/test_link.c` reads `rt/start.c` and checks that it spells the entry in the form that `mangle` writes for each object format.

### Unwind data

Windows finds the caller of a function through the sections `.pdata` and `.xdata` of the object file. A function that allocates stack space or calls another function needs an entry there[^27]. Without an entry the unwinder treats the function as a leaf and reads the return address at `rsp`[^27]. In a function with a frame that address is wrong. An exception that a C library raises then cannot unwind through an Anti frame, and a debugger shows a wrong stack. The assembler llvm-mc writes both sections from `.seh_` directives in the assembly file. Register allocation sets the flag `unwind` of a function with a frame on a COFF target. The function `emit_function` then writes `.seh_proc` after its label and `.seh_endproc` after its last block. The prologue and the epilogues add each directive after the instruction it describes.

On x64 each unwind code records one operation of the prologue: a push, a stack allocation, a frame register or a save with `mov`[^27]. The offset of a save counts from the bottom of the fixed allocation. With a frame register in the unwind data, it counts from the value of `rsp` when that register was set[^27]. The prologue of chapter 13 sets `rbp` before the allocation, and the saves lie below it, where no unsigned offset reaches. The Windows prologue therefore names no frame register. Anti has no `alloca`, so `rsp` alone locates the frame in the body. A dynamic stack allocation would need a frame register in the unwind data and the `lea` form of the epilogue.

```c
/* rbp points to the saved rbp above the frame. Callee-saved registers
   take the top of the frame, slots its bottom.
   DESIGN: the Windows unwind data names no frame register. With one, each
   save offset would count from rbp, above the saves, and a save offset is
   unsigned. Anti has no alloca, so rsp alone locates the frame. If Anti
   gets dynamic stack allocation, the Windows prologue must set a frame
   register and the epilogue must switch to lea. */
static void prologue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[2];
    size_t i;

    if (!frame->needed) {
        return;
    }
    ops[0] = mach_preg(RBP, 64);
    append(b, X64_PUSH, 1, ops);
    unwind(b, frame, X64_SEH_PUSHREG, 1, ops);
    ops[1] = mach_preg(RSP, 64);
    append(b, X64_MOV, 2, ops);
    if (frame->size > 0) {
        allocate_frame(b, frame);
        ops[0] = mach_imm((int64_t)frame->size);
        unwind(b, frame, X64_SEH_STACKALLOC, 1, ops);
    }
    for (i = 0; i < frame->saved_count; i++) {
        save_register(b, frame->saved[i], frame->saved_offset[i], false);
        ops[0] = mach_preg(frame->saved[i], 64);
        ops[1] = mach_imm(frame->saved_offset[i]);
        unwind(b, frame,
               frame->saved[i] >= XMM0 ? X64_SEH_SAVEXMM : X64_SEH_SAVEREG,
               2, ops);
    }
    unwind(b, frame, X64_SEH_ENDPROLOGUE, 0, ops);
}
```

The unwind code `UWOP_SAVE_XMM128` holds the offset of an `xmm` save divided by 16, so every `xmm` save must start at a multiple of 16[^27]. The layout of the frame meets that constraint in the function `save_end` of `src/regalloc.c`. Below an odd number of 8-byte saves, `xmm6` moves down by 8 bytes.

```c
/* The bytes from the top of the frame to the end of the save of register
   preg. The saves above it take the first used bytes. A 16-byte save
   starts at a multiple of 16, which UWOP_SAVE_XMM128 of the Windows x64
   unwind data requires. */
static uint64_t save_end(const struct alloc *a, uint8_t preg, uint64_t used)
{
    uint64_t size = is_fp_register(a, preg) ? a->abi->fp_save_size : 8;

    return align_up(used, size) + size;
}
```

The page on the x64 epilogue allows `add rsp` or `lea rsp` from the frame pointer register, followed by pops and `ret`[^11]. The unwind data names no frame pointer register, so the Windows epilogue frees the frame with `add`. The listing of `main.anti` for windows-x86_64 above ends with `addq $32, %rsp`, `popq %rbp` and `ret`.

The test `unwind_windows-x86_64` compiles `tests/dump/unwind.anti`, assembles it with llvm-mc and decodes the object with `llvm-readobj --unwind` of LLVM 23.1.1. The function `spin` holds a 5000-byte array, keeps six integer registers and a float across its calls and returns in two places.

```anti
// A frame of more than a page with callee-saved integer and float
// registers and two returns, for the Windows unwind data of chapter 16.
extern fn g(x: int) -> int;
extern fn h(x: f64) -> f64;

fn spin(a: int, b: int, c: int) -> int
{
    let big: [5000]byte = [0; 5000];
    let x = g(a) + b;
    if x < 0 {
        return c;
    }
    let y = g(x) + c;
    let d = h(a as f64) + (b as f64);
    let e = h(d) + d;
    big[a & 1023] = y as byte;
    return x + y + (e as int) + (big[b & 1023] as int);
}

fn main() -> int
{
    return spin(1, 2, 3);
}
```

The decoded unwind data lists the codes in the reverse order of the prologue. `PUSH_NONVOL` records `pushq %rbp`, `ALLOC_LARGE` the 5104 bytes after `__chkstk`, and each `SAVE_NONVOL` a save with its offset from `rsp`. The float save sits at `0x13B0`, the offset 5040, a multiple of 16.

```text
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
UnwindInformation [
  RuntimeFunction {
    StartAddress: _A6unwind_spin (0x0)
    EndAddress: _A6unwind_spin +0x171 (0x4)
    UnwindInfoAddress: .xdata (0x8)
    UnwindInfo {
      Version: 1
      Flags [ (0x0)
      ]
      PrologSize: 73
      FrameRegister: -
      FrameOffset: -
      UnwindCodeCount: 17
      UnwindCodes [
        0x49: SAVE_XMM128 reg=XMM6, offset=0x13B0
        0x41: SAVE_NONVOL reg=R14, offset=0x13C0
        0x39: SAVE_NONVOL reg=R13, offset=0x13C8
        0x31: SAVE_NONVOL reg=R12, offset=0x13D0
        0x29: SAVE_NONVOL reg=RDI, offset=0x13D8
        0x21: SAVE_NONVOL reg=RSI, offset=0x13E0
        0x19: SAVE_NONVOL reg=RBX, offset=0x13E8
        0x11: ALLOC_LARGE size=5104
        0x01: PUSH_NONVOL reg=RBP
      ]
    }
  }
  RuntimeFunction {
    StartAddress: _A4anti2rt_main (0xC)
    EndAddress: _A6unwind_main +0x28 (0x10)
    UnwindInfoAddress: .xdata +0x28 (0x14)
    UnwindInfo {
      Version: 1
      Flags [ (0x0)
      ]
      PrologSize: 8
      FrameRegister: -
      FrameOffset: -
      UnwindCodeCount: 2
      UnwindCodes [
        0x08: ALLOC_SMALL size=32
        0x01: PUSH_NONVOL reg=RBP
      ]
    }
  }
]
```

On Windows ARM64 each instruction of a prologue or an epilogue maps to exactly one unwind code[^28]. The codes `save_reg` and `save_freg` hold an offset from `sp` of at most 504 bytes[^28]. The prologue of chapter 13 saves the callee-saved registers at the top of the whole frame, so a frame above 512 bytes needs larger offsets. With the flag `unwind` the same prologue allocates the save area first and saves into it with small offsets. It then points `x29` at the frame record with `add` and allocates the rest of the frame in the body. Microsoft allows changes of `sp` outside the prologue when `x29` keeps the original `sp`[^28]. The frame ends with the layout of chapter 13.

The epilogue mirrors the prologue between `.seh_startepilogue` and `.seh_endepilogue`. It frees the body part of the frame, restores the saves, frees the save area and loads the frame record.

```c
/* With unwind data the epilogue mirrors the prologue. It frees the body
   part of the frame and restores the saves. It then frees the save area
   and loads the frame record, each step with its directive. */
static void epilogue(struct mach_block *b, const struct frame *frame)
{
    struct mach_operand ops[3];
    uint64_t area = save_area(frame);
    size_t i;

    if (!frame->needed) {
        return;
    }
    if (frame->unwind) {
        unwind(b, frame, A64_SEH_STARTEPILOGUE, 0, ops);
        if (frame->size > area) {
            free_stack(b, frame, frame->size - area);
        }
        for (i = 0; i < frame->saved_count; i++) {
            int64_t offset = frame->saved_offset[i] -
                             (int64_t)(frame->size - area);
            load_spill(b, frame->saved[i], offset);
            unwind_save(b, frame, frame->saved[i], offset);
        }
        if (area > 0) {
            free_stack(b, frame, area);
        }
        ops[0] = mach_preg(X29, 64);
        ops[1] = mach_preg(X30, 64);
        ops[2] = stack(16, INDEX_POST);
        append(b, A64_LDP, 3, ops);
        ops[0] = mach_imm(16);
        unwind(b, frame, A64_SEH_SAVE_FPLR_X, 1, ops);
        unwind(b, frame, A64_SEH_ENDEPILOGUE, 0, ops);
        return;
    }
    for (i = 0; i < frame->saved_count; i++) {
        load_spill(b, frame->saved[i], frame->saved_offset[i]);
    }
    if (frame->size > 0) {
        ops[0] = mach_preg(SP, 64);
        ops[1] = mach_preg(X29, 64);
        append(b, A64_MOV, 2, ops);
    }
    ops[0] = mach_preg(X29, 64);
    ops[1] = mach_preg(X30, 64);
    ops[2] = stack(16, INDEX_POST);
    append(b, A64_LDP, 3, ops);
}
```

The function `free_stack` moves `sp` up. A size that no 12-bit immediate holds goes through `x16`, and each instruction of that load gets the code `nop`, which keeps one code per instruction.

```c
/* Move sp up by bytes. Each instruction gets its unwind directive: a nop
   for the load of x16 and the allocation for the add. */
static void free_stack(struct mach_block *b, const struct frame *frame,
                       uint64_t bytes)
{
    struct mach_operand ops[4];
    struct mach_block load;
    size_t i;

    ops[0] = mach_preg(SP, 64);
    ops[1] = mach_preg(SP, 64);
    if (fits_imm12((int64_t)bytes)) {
        append_imm12(b, A64_ADD, 2, ops, (int64_t)bytes);
    } else {
        memset(&load, 0, sizeof load);
        load_into(&load, mach_preg(X16, 64), bytes);
        for (i = 0; i < load.count; i++) {
            *mach_append(b) = load.insts[i];
            unwind(b, frame, A64_SEH_NOP, 0, ops);
        }
        free(load.insts);
        ops[2] = mach_preg(X16, 64);
        append(b, A64_ADD, 3, ops);
    }
    ops[0] = mach_imm((int64_t)bytes);
    unwind(b, frame, A64_SEH_STACKALLOC, 1, ops);
}
```

The test `unwind_windows-arm64` decodes the same program for windows-arm64. The prologue codes read from the body outward. Undone, `add fp` sets `sp` from `x29`, the six saves restore their registers, `sub sp` frees the save area and the `stp` loads the frame record. The first epilogue starts with the `nop` of the load of `x16` and frees the 5008 bytes of the body part.

```text
  RuntimeFunction {
    Function: _A6unwind_spin (0x0)
    ExceptionRecord: .xdata (0x0)
    ExceptionData {
      FunctionLength: 292
      Version: 0
      ExceptionData: No
      EpiloguePacked: No
      EpilogueScopes: 2
      ByteCodeLength: 36
      Prologue [
        0xe206              ; add fp, sp, #48
        0xdc00              ; str d8, [sp, #0]
        0xd101              ; str x23, [sp, #8]
        0xd0c2              ; str x22, [sp, #16]
        0xd083              ; str x21, [sp, #24]
        0xd044              ; str x20, [sp, #32]
        0xd005              ; str x19, [sp, #40]
        0x03                ; sub sp, #48
        0x81                ; stp x29, x30, [sp, #-16]!
        0xe4                ; end
      ]
      EpilogueScopes [
        EpilogueScope {
          StartOffset: 30
          EpilogueStartIndex: 17
          Opcodes [
            0xe3                ; nop
            0xc139              ; add sp, #5008
            0xd005              ; ldr x19, [sp, #40]
            0xd044              ; ldr x20, [sp, #32]
            0xd083              ; ldr x21, [sp, #24]
            0xd0c2              ; ldr x22, [sp, #16]
            0xd101              ; ldr x23, [sp, #8]
            0xdc00              ; ldr d8, [sp, #0]
            0x03                ; add sp, #48
            0x81                ; ldp x29, x30, [sp], #16
            0xe4                ; end
          ]
        }
```

The unit tests `probe` in `tests/unit/test_x86_64.c` and `tests/unit/test_arm64.c` pin the prologue and the epilogue of an 8192-byte frame with an integer and a float save. The asm tests assemble every program with its directives for both Windows triples. No Windows program runs on the development Mac, so no exception has unwound through an Anti frame yet.

## Export symbols

An `export fn` has the C symbol of its name and `.globl` in every mode, so that C code calls it by that name. A call from Anti code uses the same symbol. The removal of unused functions of chapter 10 keeps every `export fn`, because C code may call it. This program is the one that the unit test in `tests/unit/test_emit.c` compiles for macos-arm64.

```anti
export fn twice(x: int) -> int
{
    return 2 * x;
}

fn main() -> int
{
    return twice(21);
}
```

The command `antic -S --target macos-arm64` writes this file for it. The symbol `_twice` is global, and `_main.main` stays a local symbol.

```text
    .build_version macos, 11, 0
    .text
    .globl _anti.rt.main
    .set _anti.rt.main, _main.main
    .globl _twice
    .p2align 2
_twice:
L_twice.b0:
    lsl x0, x0, #1
    ret
    .p2align 2
_main.main:
L_main.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #21
    bl _twice
    ldp x29, x30, [sp], #16
    ret
```

## Dev mode objects

The option `--dev` compiles one module into its own object file. The functions of imported modules stay external symbols, which the objects of those modules define. The driver optimizes the module alone with `ir_optimize_module` of chapter 10, so no call crosses into the code of another module. Chapter 24 describes how the build tool caches these objects.

The function `emit_module` makes every function of the module global, so that the objects of other modules reach it. It marks each one except an `export fn` hidden, with `.hidden` on ELF and `.private_extern` on Mach-O. The GNU assembler gives such a symbol a visibility under which it is "not visible to other components"[^12]. The ELF specification requires the link editor to remove a hidden symbol, or to make it local, when it builds an executable or a shared object[^13]. Apple's linker calls a private external symbol "visibility=hidden"[^2]. A shared library linked from such objects exports only its `export fn` symbols.

The emitter writes no visibility directive for COFF. A DLL exports the functions in its exports table, and "any other functions in the DLL are private to the DLL"[^14]. A module-definition file fills that table, as the section on module-definition files shows.

The module `com.example.scale` in `tests/modules` defines one function.

```anti
pub const SCALE: int = 6;
pub fn scale(x: int) -> int
{
    return x * SCALE;
}
```

The command `antic --dev -S --target macos-arm64 -I tests/modules` writes its object file for macos-arm64.

```text
    .build_version macos, 11, 0
    .text
    .globl _com.example.scale.scale
    .private_extern _com.example.scale.scale
    .p2align 2
_com.example.scale.scale:
L_com.example.scale.scale.b0:
    mov x9, #6
    mul x0, x0, x9
    ret
```

For windows-x86_64 the function is global without a visibility directive.

```text
    .text
    .globl _A3com7example5scale_scale
_A3com7example5scale_scale:
.L_A3com7example5scale_scale.b0:
    imulq $6, %rcx, %rax
    ret
```

The main module calls `twice` of the module `com.example.twice`, which calls `scale`.

```anti
import com.example.twice;

fn main() -> int
{
    return twice.twice(7) / 2;
}
```

In dev mode for linux-x86_64 its file defines the runtime entry and a hidden `main.main`. The call names the symbol `com.example.twice.twice`, which the object of that module defines.

```text
    .text
    .globl anti.rt.main
    .set anti.rt.main, main.main
    .globl main.main
    .hidden main.main
main.main:
.Lmain.main.b0:
    pushq %rbp
    movq %rsp, %rbp
    movq $7, %rdi
    call com.example.twice.twice
    movq $2, %rcx
    cqto
    idivq %rcx
    popq %rbp
    ret
    .section .note.GNU-stack,"",@progbits
```

A module without `main` stops at its object file, because `back_end` returns the status 3 for it. That compilation needs no `--runtime` and writes no licence notice. A module with `main` is linked. The object files named on the command line of antic follow its own object, as the section on linker command lines shows. The script `tests/run_dev.cmake` builds the program of `tests/modules` this way and expects the exit status 42.

A module of a dependency may exist only as a library file. With `--dev` and no source file, antic takes the first library file on its command line as the input. The function `compile_library_file` in `src/driver.c` loads that file with the library files it imports and calls `back_end` without a syntax tree, so lowering is skipped. A library file holds the IR of lowering before any optimisation, as chapter 9 describes, so the assembly equals the assembly from the source. The object of a library file stops at the object, since a main module is a source file. The script then compiles both library modules from their library files and compares the assembly with the files from source. It links and runs that program as well.

```cmake
# Build a program of three modules in dev mode: each library module into
# its own object, then the main module, linked with those objects. Run the
# result and check its exit status. Then build the library modules again
# from their library files, which gives the same assembly, and link and
# run that program too. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory
#   MODULES   the search root of the sources
#   LIBS      the search root of the library files
#   WORK      a directory for the objects and the executable

file(MAKE_DIRECTORY "${WORK}")
foreach(module scale twice)
    execute_process(
        COMMAND "${ANTIC}" --dev --llvm-mc "${LLVM_MC}" -I "${MODULES}"
                -I "${LIBS}" -o "${WORK}/${module}"
                "${MODULES}/com/example/${module}.anti"
        RESULT_VARIABLE status
        ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "antic --dev ${module}.anti failed\n${err}")
    endif()
endforeach()
execute_process(
    COMMAND "${ANTIC}" --dev --llvm-mc "${LLVM_MC}" --runtime "${RUNTIME}"
            -I "${LIBS}" -o "${WORK}/main" "${MODULES}/main.anti"
            "${WORK}/scale.o" "${WORK}/twice.o"
    RESULT_VARIABLE status
    ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic --dev main.anti failed\n${err}")
endif()
execute_process(COMMAND "${WORK}/main" RESULT_VARIABLE status)
if(NOT status EQUAL 42)
    message(FATAL_ERROR "the program exits with ${status}, expected 42")
endif()

foreach(module scale twice)
    execute_process(
        COMMAND "${ANTIC}" --dev --llvm-mc "${LLVM_MC}" -I "${LIBS}"
                -o "${WORK}/${module}_antl"
                "${LIBS}/com/example/${module}.antl"
        RESULT_VARIABLE status
        ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "antic --dev ${module}.antl failed\n${err}")
    endif()
    file(READ "${WORK}/${module}.s" from_source)
    file(READ "${WORK}/${module}_antl.s" from_library)
    if(NOT from_source STREQUAL from_library)
        message(FATAL_ERROR "${module}_antl.s differs from ${module}.s")
    endif()
endforeach()
execute_process(
    COMMAND "${ANTIC}" --dev --llvm-mc "${LLVM_MC}" --runtime "${RUNTIME}"
            -I "${LIBS}" -o "${WORK}/main_antl" "${MODULES}/main.anti"
            "${WORK}/scale_antl.o" "${WORK}/twice_antl.o"
    RESULT_VARIABLE status
    ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic --dev main.anti with library objects failed\n${err}")
endif()
execute_process(COMMAND "${WORK}/main_antl" RESULT_VARIABLE status)
if(NOT status EQUAL 42)
    message(FATAL_ERROR "the program from library objects exits with ${status}, expected 42")
endif()
```

The test `dev_modules` runs the script on the development Mac. The unit tests in `tests/unit/test_emit.c` pin the hidden functions for linux-x86_64 and macos-arm64.

## Constructor sections

A shared library for C, which `antic --lib shared` writes, has no C `main` of the runtime. Chapter 25, Libraries for C, describes those libraries. The runtime still needs its initialisation, which the C `main` of `rt/start.c` performs first in an executable.

```c
int main(int argc, char **argv)
{
    size_t count = 0;

    anti_rt_init();
    while (environ != NULL && environ[count] != NULL) {
        count++;
    }
    return (int)anti_main(strings(argv, (size_t)argc),
                          strings(environ, count));
}
```

The file `rt/init.c` defines `anti_rt_init`, and `anti_rt_ready` reports whether it ran.

```c
#include "rt.h"

static int ready;

void anti_rt_init(void)
{
    ready = 1;
}

int anti_rt_ready(void)
{
    return ready;
}
```

A shared library calls `anti_rt_init` from a constructor, a function whose address the loader finds in a section of the library and calls at load time. The function `emit_constructor` places that address in the section of each object format.

```c
/* DESIGN: a shared library initialises the runtime in a constructor. The
   loader calls every function whose address lies in .init_array on ELF,
   __mod_init_func on Mach-O and .CRT$XCU on COFF. */
void emit_constructor(struct text *out, enum target t, const char *function)
{
    static const char *const sections[] = {
        [FORMAT_ELF] = ".init_array,\"aw\"",
        [FORMAT_MACHO] = "__DATA,__mod_init_func,mod_init_funcs",
        [FORMAT_COFF] = ".CRT$XCU,\"dr\"",
    };
    struct text symbol = {0};

    c_symbol(&symbol, t, function);
    text_appendf(out, "    .section %s\n    .p2align 3\n    .quad %s\n",
                 sections[target_info(t)->format], text_cstr(&symbol));
    text_free(&symbol);
}
```

The ELF section `.init_array` "holds an array of function pointers that contributes to a single initialization array for the executable or shared object"[^15]. The dynamic linker runs the initialization functions after it has built the process image and performed the relocations[^16]. The Mach-O section type `S_MOD_INIT_FUNC_POINTERS` marks a "section with only function pointers for initialization"[^17]. The Microsoft C++ compiler places its dynamic initializers in `.CRT$XCU`. The CRT calls every pointer between `__xc_a` in `.CRT$XCA` and `__xc_z` in `.CRT$XCZ`, because the linker orders these sections alphabetically[^18]. In a DLL the entry point `_DllMainCRTStartup` calls the constructors for static data on process attach[^19].

The library module `com.example.doubling` exports one function.

```anti
export fn twice(x: int) -> int
{
    return 2 * x;
}
```

The command `antic --lib shared -S`, with the search root of the module after `-I`, writes the file with the constructor at its end. The output of `-S` holds no licence notice, which only a link adds. For linux-x86_64 the constructor follows the GNU-stack section.

```text
    .text
    .globl twice
twice:
.Ltwice.b0:
    movq %rdi, %rax
    shlq $1, %rax
    ret
    .section .note.GNU-stack,"",@progbits
    .section .init_array,"aw"
    .p2align 3
    .quad anti_rt_init
```

```text
    .build_version macos, 11, 0
    .text
    .globl _twice
    .p2align 2
_twice:
L_twice.b0:
    lsl x0, x0, #1
    ret
    .section __DATA,__mod_init_func,mod_init_funcs
    .p2align 3
    .quad _anti_rt_init
```

```text
    .text
    .globl twice
    .p2align 2
twice:
.Ltwice.b0:
    lsl x0, x0, #1
    ret
    .section .CRT$XCU,"dr"
    .p2align 3
    .quad anti_rt_init
```

The objects that llvm-mc 23.1.1 assembles from these files show the section types. The ELF section header of `.init_array` holds the type 14, `SHT_INIT_ARRAY`, and the flags `SHF_ALLOC` and `SHF_WRITE`, the attributes that the ELF specification lists[^15]. `otool -lv` prints the type `S_MOD_INIT_FUNC_POINTERS` for `__mod_init_func`. The test `clib_loader` loads a shared library with `dlopen` on the development Mac. Its function `geo_ready` returns 1, because the constructor has run.

## Licence notice

The read-only data object `anti_licenses` lists the packages of every executable and shared library that antic links. The object holds plain text between two marker lines, by which a tool finds it in any binary. Chapter 24 describes the command of the build tool that prints it. A static library holds no notice.

```c
/* The begin and end markers of the licence notice, by which a tool finds
   anti_licenses in any Anti binary. */
#define NOTICE_BEGIN "ANTI_LICENSES_BEGIN\n"
#define NOTICE_END "ANTI_LICENSES_END\n"
```

The function `build_notice` in `src/driver.c` lists the packages in order. The runtime comes first as the package `anti.rt`, with the version of antic and the licence 0BSD. Each package of the loaded library files follows once, and the package of the compiled module comes last. The runtime's licence text comes from `licenses/anti_rt.txt` in the runtime directory, which the CMake build copies from `rt/LICENSE`.

```c
/* The licence notice of a linked binary: the runtime, every package of
   the loaded libraries once, and the package of the compiled module. */
static bool build_notice(const struct options *o, const struct interface *own,
                         const struct interface *const *libraries,
                         size_t count, struct arena *arena, struct text *out)
{
    const struct package **list = calloc(count + 3, sizeof *list);
    struct package runtime;
    struct text path = {0};
    struct text text = {0};
    size_t n = 0;
    size_t i;
    size_t j;

    if (list == NULL) {
        fputs("antic: out of memory\n", stderr);
        exit(70);
    }
    memset(&runtime, 0, sizeof runtime);
    runtime.name = RUNTIME_MODULE;
    runtime.version = ANTIC_VERSION;
    runtime.license = "0BSD";
    runtime.license_text = "";
    text_appendf(&path, "%s/licenses/anti_rt.txt",
                 o->runtime != NULL ? o->runtime : ".");
    if (o->runtime != NULL && read_source(text_cstr(&path), &text)) {
        char *copy = arena_alloc(arena, text.length + 1);
        memcpy(copy, text_cstr(&text), text.length);
        runtime.license_text = copy;
    }
    list[n++] = &runtime;
    for (i = 0; i < count; i++) {
        for (j = 0; j < n; j++) {
            if (strcmp(list[j]->name, libraries[i]->package.name) == 0) {
                break;
            }
        }
        if (j == n) {
            list[n++] = &libraries[i]->package;
        }
    }
    list[n++] = &own->package;
    notice_text(out, list, n);
    free((void *)list);
    text_free(&path);
    text_free(&text);
    return true;
}
```

The package of the compiled module takes its fields from the options `--package-name`, `--package-version`, `--license`, `--license-text` and `--attribution`. Chapter 9 describes these fields in the header of a library file. The function `notice_text` writes a line per package with its name, version and licence, followed by its attribution lines. A package name that came before is left out, so the modules of one package give one line. Each distinct licence text follows once, after the names of the packages that it covers.

```c
/* Whether a package before index i has the name of package i. */
static bool repeated(const struct package *const *packages, size_t i)
{
    size_t j;

    for (j = 0; j < i; j++) {
        if (strcmp(or_empty(packages[j]->name),
                   or_empty(packages[i]->name)) == 0) {
            return true;
        }
    }
    return false;
}

void notice_text(struct text *out, const struct package *const *packages,
                 size_t count)
{
    size_t i;
    size_t j;
    size_t k;

    text_append(out, NOTICE_BEGIN);
    for (i = 0; i < count; i++) {
        const struct package *p = packages[i];
        if (repeated(packages, i)) {
            continue;
        }
        text_appendf(out, "package %s %s %s\n", or_empty(p->name),
                     or_empty(p->version), or_empty(p->license));
        for (k = 0; k < p->attribution_count; k++) {
            text_appendf(out, "attribution %s\n", p->attribution[k]);
        }
    }
    for (i = 0; i < count; i++) {
        const char *text = or_empty(packages[i]->license_text);
        if (text[0] == '\0' || repeated(packages, i)) {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(or_empty(packages[j]->license_text), text) == 0) {
                break;
            }
        }
        if (j < i) {
            continue;
        }
        text_append(out, "text for");
        for (j = i; j < count; j++) {
            if (strcmp(or_empty(packages[j]->license_text), text) == 0 &&
                !repeated(packages, j)) {
                text_appendf(out, " %s", or_empty(packages[j]->name));
            }
        }
        text_appendf(out, "\n%s", text);
        if (text[strlen(text) - 1] != '\n') {
            text_append(out, "\n");
        }
    }
    text_append(out, NOTICE_END);
}
```

The function `emit_licenses` writes the notice as bytes with a NUL after them. It uses the read-only data section of chapter 19 and a global C symbol.

```c
void emit_licenses(struct text *out, enum target t, const char *bytes,
                   size_t length)
{
    struct text symbol = {0};
    size_t k;

    c_symbol(&symbol, t, "anti_licenses");
    text_appendf(out, "    .section %s\n    .globl %s\n%s:\n",
                 data_sections[target_info(t)->format], text_cstr(&symbol),
                 text_cstr(&symbol));
    for (k = 0; k <= length; k++) {
        unsigned char c = k < length ? (unsigned char)bytes[k] : 0;
        text_appendf(out, "%s0x%02x", k % 16 == 0 ? "    .byte " : ", ", c);
        if (k % 16 == 15 || k == length) {
            text_append(out, "\n");
        }
    }
    text_free(&symbol);
}
```

The driver writes `anti_licenses` for a linked program, for a shared library and for a dev mode module with `main`. It writes none for the output of `-S`, for a static library and for a dev mode module without `main`. The script `tests/run_licenses.cmake` links `tests/programs/return42.anti` with a package name, a version and a licence, and matches the package lines in the executable. The test `program_licenses` also finds the bytes of the whole notice from `tests/dump/licenses.notice` in the executable.

```cmake
# Link a program and find the licence notice anti_licenses in it by its
# markers. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory
#   SOURCE    the .anti file
#   WORK      a directory for the executable
#   WANTED    optional list of patterns for the marker and package lines
#   EXPECTED  optional file with the whole notice that the program holds

file(MAKE_DIRECTORY "${WORK}")
execute_process(
    COMMAND "${ANTIC}" --llvm-mc "${LLVM_MC}" --runtime "${RUNTIME}"
            --package-name com.example.hello --package-version 2.0.0
            --license MIT -o "${WORK}/licensed" "${SOURCE}"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic failed\n${err}")
endif()
file(STRINGS "${WORK}/licensed" lines REGEX "ANTI_LICENSES|^package ")
set(wanted "ANTI_LICENSES_BEGIN;package anti.rt [0-9.]+ 0BSD;package com.example.hello 2.0.0 MIT")
if(DEFINED WANTED)
    string(REPLACE "|" ";" wanted "${WANTED}")
endif()
string(JOIN ";" got ${lines})
if(NOT got MATCHES "^${wanted}")
    message(FATAL_ERROR "the notice in the program is\n${got}")
endif()
if(DEFINED EXPECTED)
    file(READ "${WORK}/licensed" program HEX)
    file(READ "${EXPECTED}" notice HEX)
    string(FIND "${program}" "${notice}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "the program does not hold the notice of ${EXPECTED}")
    endif()
endif()
```

The executable of that command on the development Mac holds this notice, 800 bytes and a NUL.

```text
ANTI_LICENSES_BEGIN
package anti.rt 0.1.0 0BSD
package com.example.hello 2.0.0 MIT
text for anti.rt
BSD Zero Clause License

Copyright (c) 2026 Eddie Niese / Founding Future

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
ANTI_LICENSES_END
```

## Module-definition files

The Microsoft linker builds the exports table of a DLL from a module-definition file with the suffix `.def`. Its `LIBRARY` statement names the DLL[^20]. Each line of its `EXPORTS` section names a function or a variable to export, and the keyword `DATA` marks a variable[^21].

The function `build_c_library` in `src/driver.c` writes `<name>.def` beside a Windows DLL. The file lists every `export fn` of the program and `anti_licenses DATA`. The function `back_end` collects the names of the export fns while it emits, and the link command passes the file with `/DEF:`.

```c
        if (ok && info->os == OS_WINDOWS) {
            struct text content = {0};
            const char *p = text_cstr(&extras->exports);
            text_appendf(&def, "%s%s%s", text_cstr(&dir), name, DEF_SUFFIX);
            text_appendf(&content, "LIBRARY %s\nEXPORTS\n", name);
            while (*p != '\0') {
                size_t n = strcspn(p, "\n");
                text_appendf(&content, "    %.*s\n", (int)n, p);
                p += n + (p[n] == '\n');
            }
            text_append(&content, "    anti_licenses DATA\n");
            ok = write_file(text_cstr(&def), &content);
            s.def_file = text_cstr(&def);
            text_free(&content);
        }
```

The rest of `build_c_library`, with the shared library link and the static archive, belongs to chapter 25. The unit test `libraries` in `tests/unit/test_link.c` pins the command line `link.exe /NOLOGO /DLL /MACHINE:ARM64 /OUT:geo.dll /DEF:geo.def` with its inputs. The compiler antic links a DLL only on a Windows host, so no test on the development Mac writes a `.def` file.

## Assembler and linker command lines

The function `assemble` in `src/driver.c` runs llvm-mc on the assembly file with the triple of the target from chapter 11. Without `--llvm-mc` the command is `llvm-mc` from the search path.

```c
/* Run llvm-mc on the assembly file for the target. */
static bool assemble(const struct options *o, const char *assembly,
                     const char *object)
{
    struct text triple = {0};
    struct text found = {0};
    const char *argv[] = {NULL, NULL, "-filetype=obj", "-o", object, assembly,
                          NULL};
    int run;

    argv[0] = o->llvm_mc != NULL ? o->llvm_mc
                                 : archive_tool(o, "llvm-mc", &found);
    text_appendf(&triple, "-triple=%s", target_info(o->target)->triple);
    argv[1] = text_cstr(&triple);
    run = process_run(argv);
    text_free(&triple);
    text_free(&found);
    if (run != 0) {
        fprintf(stderr, "antic: llvm-mc failed\n");
        return false;
    }
    return true;
}
```

The linker of every target is lld of the pinned LLVM release. It runs as `ld.lld` for ELF, as `ld64.lld` for Mach-O and as `lld-link` for COFF, the programs that chapter 3 installs. The option `--linker platform` selects the linker of the host's own toolchain instead, which chapter 3 uses for its first executable. The function `link_command` in `src/linker.c` builds the command line of each operating system from the files and from facts about the target. The driver runs it after llvm-mc.

A link needs the C library of the target and its start files. With lld these come from the runtime archive, which chapter 23, The runtime archive, describes. Its directory `sysroot/<target>/` holds them for each target, so lld links a program for any target on any host. The platform linker takes the C library of the host and links only for the operating system it runs on.

```c
/* DESIGN: lld links for every target from any host, with the sysroot of
   the runtime archive. The platform linker links only for the operating
   system it runs on, where the C library of the target is installed. */
static bool can_link(const struct options *o)
{
    enum target host;

    if (o->linker == LINKER_LLD) {
        return true;
    }
    if (!target_host(&host) ||
        target_info(host)->os != target_info(o->target)->os) {
        fprintf(stderr, "antic: linking for %s with the platform linker needs "
                        "a host with the same operating system. -S writes "
                        "the assembly without linking.\n",
                target_name(o->target));
        return false;
    }
    return true;
}
```

The function `link_facts` collects what a command line needs. For lld it names the directory `bin/` of the runtime archive, which holds the lld programs, and the sysroot of the target. A Linux or macOS link without a sysroot stops with a message that names `tools/get-sysroot.cmake`. A Windows link without a sysroot leaves the library directories to lld-link, which reads the environment variable `LIB` as `link.exe` does[^22]. An MSVC environment sets that variable. For the platform linker, `link_facts` asks `xcrun` for the macOS SDK and searches the glibc start files of a Linux host.

```c
/* The facts of a link for the target. lld takes the sysroot and the lld
   programs of the runtime archive, and the SDK version of a macOS
   sysroot. The platform linker takes the macOS SDK from xcrun and the
   glibc start files of the host. */
static bool link_facts(const struct options *o, struct link_inputs *in,
                       struct link_facts *f)
{
    enum target t = o->target;
    enum target_os os = target_info(t)->os;
    static const char *const flavours[] = {
        [OS_LINUX] = "ld.lld", [OS_MACOS] = "ld64.lld",
        [OS_WINDOWS] = "lld-link",
    };
    struct text marker = {0};
    bool present;

    memset(f, 0, sizeof *f);
    in->linker = o->linker;
    if (o->linker == LINKER_PLATFORM) {
        if (os == OS_MACOS) {
            if (!xcrun("--show-sdk-path", &f->sdk_path) ||
                !xcrun("--show-sdk-version", &f->sdk_version)) {
                return false;
            }
            in->sdk_path = text_cstr(&f->sdk_path);
            in->sdk_version = text_cstr(&f->sdk_version);
        } else if (os == OS_LINUX) {
            if (!find_crt_dir(t, &f->crt_dir)) {
                return false;
            }
            in->crt_dir = text_cstr(&f->crt_dir);
        }
        return true;
    }
    text_appendf(&f->lld_dir, "%s/%s", o->runtime, RUNTIME_BIN_DIR);
    text_appendf(&marker, "%s/%s", text_cstr(&f->lld_dir), flavours[os]);
    in->lld_dir = file_exists(text_cstr(&marker)) ? text_cstr(&f->lld_dir)
                                                  : NULL;
    text_free(&marker);
    text_appendf(&f->sysroot, "%s/%s/%s", o->runtime, RUNTIME_SYSROOT_DIR,
                 target_name(t));
    text_appendf(&marker, "%s/%s", text_cstr(&f->sysroot),
                 os == OS_LINUX   ? "usr/lib/libc.a"
                 : os == OS_MACOS ? SYSROOT_SDK_VERSION
                 : target_info(t)->arch == ARCH_ARM64
                     ? "crt/lib/aarch64/msvcrt.lib"
                     : "crt/lib/x86_64/msvcrt.lib");
    present = file_exists(text_cstr(&marker)) &&
              (os != OS_MACOS || read_bytes(text_cstr(&marker), &f->sdk_version));
    text_free(&marker);
    /* DESIGN: without a Windows sysroot lld-link reads the library
       directories of LIB, which an MSVC environment sets. */
    if (!present && os != OS_WINDOWS) {
        fprintf(stderr, "antic: linking for %s with lld needs the sysroot "
                        "%s, which tools/get-sysroot.cmake installs\n",
                target_name(t), text_cstr(&f->sysroot));
        return false;
    }
    in->sysroot = present ? text_cstr(&f->sysroot) : NULL;
    while (f->sdk_version.length > 0 &&
           (f->sdk_version.data[f->sdk_version.length - 1] == '\n' ||
            f->sdk_version.data[f->sdk_version.length - 1] == '\r')) {
        f->sdk_version.data[--f->sdk_version.length] = '\0';
    }
    in->sdk_version = text_cstr(&f->sdk_version);
    return true;
}
```

The function `program` gives each command line its linker. For lld it prefixes the directory of the runtime archive when that directory holds the program, and otherwise the name alone runs from the search path.

```c
/* The program of the linker: flavour of lld in its directory, or the
   platform linker. */
static const char *program(struct link_command *c, const struct link_inputs *in,
                           const char *flavour, const char *platform)
{
    struct text *path;

    if (in->linker == LINKER_PLATFORM) {
        return platform;
    }
    if (in->lld_dir == NULL) {
        return flavour;
    }
    path = next(c);
    text_appendf(path, "%s/%s", in->lld_dir, flavour);
    return text_cstr(path);
}
```

Every command line names the program's object and then the object files and archives from the command line of antic, before the runtime library. A dev mode link passes the objects of the other modules this way.

```c
static void add_inputs(struct link_command *c, const struct link_inputs *in)
{
    size_t i;

    add(c, in->object);
    for (i = 0; i < in->extra_count; i++) {
        add(c, in->extra[i]);
    }
}
```

### macOS

The Apple linker takes the architecture with `-arch`, the platform, minimum version and SDK version with `-platform_version`, and a prefix for all library search paths with `-syslibroot`[^2]. The option `-lSystem` links libSystem, the C library of chapter 11. The linker ld64.lld takes the same options. Its sysroot holds the `.tbd` stubs of libSystem, text files that list the symbols of each library without its code. The platform `ld` takes the SDK that `xcrun` names.

```c
/* The start of a Mach-O link: -arch, the versions and libSystem's root.
   ld64 of Apple takes the SDK that xcrun names, and ld64.lld the stubs of
   the sysroot. */
static void macos_start(struct link_command *c, enum target t,
                        const struct link_inputs *in, bool dylib)
{
    const char *linker = program(c, in, "ld64.lld", "ld");
    struct text *version = next(c);

    text_appendf(version, "%d.%d", MACOS_MIN_MAJOR, MACOS_MIN_MINOR);
    add(c, linker);
    if (dylib) {
        add(c, "-dylib");
    }
    add(c, "-S");
    add(c, "-arch");
    add(c, target_info(t)->arch == ARCH_ARM64 ? "arm64" : "x86_64");
    add(c, "-platform_version");
    add(c, "macos");
    add(c, text_cstr(version));
    add(c, in->sdk_version);
    add(c, "-syslibroot");
    add(c, in->linker == LINKER_LLD ? in->sysroot : in->sdk_path);
}
```

```c
static void macos(struct link_command *c, enum target t,
                  const struct link_inputs *in)
{
    struct text *library = next(c);

    link_runtime_library(library, in->runtime, t);
    macos_start(c, t, in, false);
    add(c, "-o");
    add(c, in->executable);
    add_inputs(c, in);
    add(c, text_cstr(library));
    add(c, "-lSystem");
}
```

The script `tools/get-sysroot.cmake` copies the stubs from the newest SDK of the Command Line Tools for Xcode that the pinned ld64.lld reads. On the development Mac the Command Line Tools hold MacOSX27.0.sdk and MacOSX26.5.sdk. The ld64.lld of LLVM 23.1.1 refuses the stub file of MacOSX27.0.sdk with the message `unknown target` for `arm64e.x1-macos`. The script therefore takes MacOSX26.5.sdk and writes `26.5` into the file `sdk-version`, which `link_facts` reads for `-platform_version`.

The test `program_return42` links with ld64.lld on the development Mac, and `otool -hv` prints the flags `NOUNDEFS DYLDLINK TWOLEVEL PIE` for its executable. The test `program_platform_linker` links the same program with `ld` of the Command Line Tools and runs it.

### Linux

The linker ld.lld links a Linux program statically against musl, a C library under the MIT licence that the sysroot holds[^23]. The option `-static` does not link against shared libraries, `-pie` creates a position independent executable, and `--no-dynamic-linker` inhibits the output of an `.interp` section[^24]. The option `--strip-debug` strips the debugging information[^24]. That information comes from the musl of the sysroot, and antic writes none of its own. Linking `tests/programs/letters.anti` for linux-arm64 writes 98,392 bytes without the option and 21,288 bytes with it. The start file `rcrt1.o` of musl is the start file for static PIE[^25]. The executable names no program interpreter and needs no C library on the Linux system that runs it.

```c
/* DESIGN: ld.lld links a Linux program statically against the musl of
   the sysroot. The executable is position-independent and has no dynamic
   linker, so it runs on any Linux kernel. It drops the debug sections,
   which come from the musl of Alpine and weigh more than the program.
   antic writes no debug information of its own, so nothing of the
   program is lost, and the symbol table stays. */
static void linux_lld(struct link_command *c, enum target t,
                      const struct link_inputs *in)
{
    static const char *const before[] = {"rcrt1.o", "crti.o"};
    static const char *const after[] = {"libc.a", "libclang_rt.builtins.a",
                                        "crtn.o"};
    const char *linker = program(c, in, "ld.lld", "ld");
    struct text *library = next(c);
    size_t i;

    link_runtime_library(library, in->runtime, t);
    add(c, linker);
    add(c, "-static");
    add(c, "-pie");
    add(c, "--no-dynamic-linker");
    add(c, "--strip-debug");
    add(c, "-o");
    add(c, in->executable);
    for (i = 0; i < 2; i++) {
        struct text *file = next(c);
        text_appendf(file, "%s/usr/lib/%s", in->sysroot, before[i]);
        add(c, text_cstr(file));
    }
    add_inputs(c, in);
    add(c, text_cstr(library));
    for (i = 0; i < 3; i++) {
        struct text *file = next(c);
        text_appendf(file, "%s/usr/lib/%s", in->sysroot, after[i]);
        add(c, text_cstr(file));
    }
}
```

The archive `libclang_rt.builtins.a` of compiler-rt supplies the functions that the C compiler calls for arithmetic without its own instructions. On ARM64, `long double` has 128 bits. The objects of musl that a call of `printf` brings in call `__addtf3`, `__multf3` and seven more such functions. A link of `tests/programs/float_math.anti` for linux-arm64 without the archive reports these nine symbols as undefined.

The platform linker links against the glibc of the host. GNU ld takes `-pie` for a position-independent executable and `--dynamic-linker` for the program interpreter[^1]. The interpreters are the paths of chapter 11. The glibc start files come in three objects. The C library builds `Scrt1.o` from the object `start.os` when it builds shared libraries, and `crti.o` and `crtn.o` "are always the first and last file in the link"[^3]. GCC links a position-independent executable with `Scrt1.o`[^10]. The option `-lc` links the C library from the directories of `-L`[^1].

```c
/* GNU ld: a position-independent executable with the glibc start
   files, crti.o first and crtn.o last, and the C library. */
static void linux_ld(struct link_command *c, enum target t,
                     const struct link_inputs *in)
{
    struct text *interpreter = next(c);
    struct text *start = next(c);
    struct text *crti = next(c);
    struct text *library = next(c);
    struct text *search = next(c);
    struct text *crtn = next(c);

    text_appendf(interpreter, "--dynamic-linker=%s",
                 target_info(t)->arch == ARCH_ARM64
                     ? "/lib/ld-linux-aarch64.so.1"
                     : "/lib64/ld-linux-x86-64.so.2");
    text_appendf(start, "%s/Scrt1.o", in->crt_dir);
    text_appendf(crti, "%s/crti.o", in->crt_dir);
    link_runtime_library(library, in->runtime, t);
    text_appendf(search, "-L%s", in->crt_dir);
    text_appendf(crtn, "%s/crtn.o", in->crt_dir);
    add(c, "ld");
    add(c, "-pie");
    add(c, "--strip-debug");
    add(c, text_cstr(interpreter));
    add(c, "-o");
    add(c, in->executable);
    add(c, text_cstr(start));
    add(c, text_cstr(crti));
    add_inputs(c, in);
    add(c, text_cstr(library));
    add(c, text_cstr(search));
    add(c, "-lc");
    add(c, text_cstr(crtn));
}
```

GCC also links its own `crtbeginS.o` and `crtendS.o`, which support the construction of C++ file-scope objects before `main`[^10]. The runtime is C, and antic leaves both out.

Distributions install the start files in different directories. Debian and Ubuntu use the multiarch directory `/usr/lib/$(DEB_HOST_MULTIARCH)`[^4], whose tuples are `x86_64-linux-gnu` and `aarch64-linux-gnu`[^5]. The Filesystem Hierarchy Standard allows `/lib64` for systems with several binary formats[^6]. The driver takes the first directory that holds `Scrt1.o`.

```c
const char *const *link_crt_dirs(enum target t)
{
    /* DESIGN: Debian and Ubuntu install glibc in the multiarch directory,
       other distributions in /usr/lib64 or /usr/lib. */
    static const char *const x86_64[] = {
        "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", NULL
    };
    static const char *const arm64[] = {
        "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", NULL
    };
    return target_info(t)->arch == ARCH_ARM64 ? arm64 : x86_64;
}
```

### Windows

The Microsoft linker takes `/SUBSYSTEM:CONSOLE` for a character-mode application[^7] and `/MACHINE` with `X64` or `ARM64` for the target platform[^8]. A link without a compiler option that selects the C runtime uses the static libraries `libcmt.lib`, `libvcruntime.lib` and `libucrt.lib`[^9]. The command line of antic names them, because its objects hold no default libraries. The program lld-link takes the options of `link.exe`.

The Windows sysroot comes from xwin, a tool that downloads the Microsoft C runtime and the Windows SDK[^26]. It lays them out for a link on another system. Its directories `crt/lib/<arch>`, `sdk/lib/um/<arch>` and `sdk/lib/ucrt/<arch>` hold the libraries[^26], and lld-link receives them with `/LIBPATH`. The option `--accept-license` of xwin accepts the licence without a prompt[^26]. The script `tools/get-sysroot.cmake` runs xwin only when its caller passes that option.

```c
/* The library directories of lld-link: the CRT and the SDK that xwin
   writes into the sysroot. Without a sysroot lld-link reads LIB, as
   link.exe does. */
static void windows_libpaths(struct link_command *c, enum target t,
                             const struct link_inputs *in)
{
    static const char *const dirs[] = {"crt/lib", "sdk/lib/um",
                                       "sdk/lib/ucrt"};
    const char *arch = target_info(t)->arch == ARCH_ARM64 ? "aarch64"
                                                          : "x86_64";
    size_t i;

    if (in->linker != LINKER_LLD || in->sysroot == NULL) {
        return;
    }
    for (i = 0; i < 3; i++) {
        struct text *dir = next(c);
        text_appendf(dir, "/LIBPATH:%s/%s/%s", in->sysroot, dirs[i], arch);
        add(c, text_cstr(dir));
    }
}
```

```c
/* DESIGN: link.exe or lld-link with the C runtime of the machine. The
   Universal CRT is a part of Windows since Windows 10, so ucrt.lib links
   against what the machine already holds. Only vcruntime, the support
   code of the compiler, comes in statically, because that one ships with
   Visual Studio rather than with Windows. The program then needs no
   redistributable, and it weighs 23,040 bytes instead of 91,136. */
static void windows(struct link_command *c, enum target t,
                    const struct link_inputs *in)
{
    const char *linker = program(c, in, "lld-link", "link.exe");
    struct text *output = next(c);
    struct text *library = next(c);

    text_appendf(output, "/OUT:%s", in->executable);
    link_runtime_library(library, in->runtime, t);
    add(c, linker);
    add(c, "/NOLOGO");
    add(c, "/debug:none");
    add(c, "/SUBSYSTEM:CONSOLE");
    add(c, target_info(t)->arch == ARCH_ARM64 ? "/MACHINE:ARM64"
                                              : "/MACHINE:X64");
    add(c, text_cstr(output));
    windows_libpaths(c, t, in);
    add_inputs(c, in);
    add(c, text_cstr(library));
    add(c, "msvcrt.lib");
    add(c, "libvcruntime.lib");
    add(c, "ucrt.lib");
    /* DESIGN: printf and its family are inline in the headers of the
       UCRT, and ucrt.lib exports none of them. A program that calls
       one through `extern fn` needs the definitions of the older
       form, which this library holds. */
    add(c, "legacy_stdio_definitions.lib");
}
```

The runtime library is `anti_rt.lib` on Windows, the name that MSVC gives a static library, and `libanti_rt.a` elsewhere. The function `link_runtime_library` builds its path below the runtime directory.

```c
void link_runtime_library(struct text *out, const char *runtime, enum target t)
{
    /* DESIGN: MSVC names a static library name.lib, and the other
       toolchains libname.a. */
    text_appendf(out, "%s/%s/%s/%s", runtime, RUNTIME_LIB_DIR, target_name(t),
                 target_info(t)->format == FORMAT_COFF ? "anti_rt.lib"
                                                       : "libanti_rt.a");
}
```

### Cross-target links

One ctest test per target, `cross_link_<target>`, links `tests/programs/return42.anti` on the host and runs `llvm-objdump -h` on the executable. The file format that llvm-objdump prints names the object format and the architecture. On the development Mac four of the six tests link.

| Target | File format |
|---|---|
| linux-x86_64 | `elf64-x86-64` |
| linux-arm64 | `elf64-littleaarch64` |
| macos-x86_64 | `mach-o 64-bit x86-64` |
| macos-arm64 | `mach-o arm64` |

The runtime archive on the development Mac has no Windows sysroot, so the tests for windows-x86_64 and windows-arm64 report themselves as skipped. Only the macos-arm64 executables run there. The Linux and Windows links are pinned by unit tests and by these format checks, and chapter 21 describes the runs on Linux and Windows machines.

## Programs that run

Every `.anti` file in `tests/programs` is compiled, linked and run on the development Mac, a macos-arm64 host. Each one runs for macos-x86_64 as well, in the test `program_<name>_macos-x86_64`, because Rosetta runs such a program on that host. Chapter 3 had two of them, and this chapter adds ten.

| Program | Checks |
|---|---|
| `scale.anti` | The program of chapter 1, exit code 42 |
| `letters.anti` | A loop that calls `putchar`, output `ABC` |
| `loop_branch.anti` | A loop and an `if` chain |
| `twice.anti` | A value kept across two calls in a callee-saved register |
| `cell.anti` | An address-taken local changed through a pointer |
| `spill.anti` | Twelve values across a call, with spills |
| `compare_bits.anti` | Comparisons as values, logical immediates, a 64-bit constant |
| `narrow_ops.anti` | Division, shifts, immediates and 8-bit and 16-bit comparisons |
| `stack_args.anti` | Eleven parameters, three on the stack, and a variadic `printf` |
| `big_frame.anti` | A frame of more than 4095 bytes with 520 address-taken locals |

The expected exit code of `narrow_ops.anti` comes from a C program with the same operations, compiled with Apple clang 21.0.0 and run on the development Mac. Its exit code is 155, the value of the Anti program. The file `stack_args.expected` holds the output `100461 42`.

## Tests

The unit tests in `tests/unit/test_emit.c` check the assembly files of both example programs for five targets and the Mach-O page offsets. They also check an export fn and the hidden functions of dev mode on ELF and Mach-O. Further tests there cover the constructor and `anti_licenses` for three object formats and the notice text of three packages. The file `tests/unit/test_link.c` checks the command lines of all six targets for lld and for the platform linker, with and without extra inputs. It also checks the start file directories and the runtime entry in `rt/start.c`. It also pins the shared library, archive and relocatable command lines of chapter 25.

The emit tests pin the assembly files of `main.anti` for six targets and of `letters.anti` for two. The asm tests assemble the output of `antic -S` for all six triples, and the tests `asm_clib_<target>` assemble a shared library with its constructor. The tests `dev_modules` and `program_licenses` run the two scripts above. The tests `unwind_windows-x86_64` and `unwind_windows-arm64` decode the Windows unwind data of `tests/dump/unwind.anti`. The tests `error_no_main` and `error_foreign_link` check the messages of the driver, the second one for the platform linker. The tests `cross_link_<target>` and `program_platform_linker` link with lld for every target and with the platform linker of the host. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 17, Floating point]({{% relref "/programming/writing-a-compiler/17-floating-point" %}}), adds `float` as a second register class with SSE and NEON registers. It covers conversions, comparisons, the ABI rules for passing floats and the changes in selection and allocation.

## References

[^1]: GNU Binutils, *The GNU linker*, section "Command-line Options", options `-pie`, `--dynamic-linker`, `-l`, `-L` and `--warn-execstack`, https://sourceware.org/binutils/docs/ld/Options.html

[^2]: Apple, *ld(1)*, options `-arch`, `-platform_version`, `-syslibroot` and `-keep_private_externs`, https://keith.github.io/xcode-man-pages/ld.1.html

[^3]: GNU C Library, source file `csu/Makefile`, the rules for `crti.o`, `crtn.o` and `Scrt1.o`, https://sourceware.org/git/?p=glibc.git

[^4]: Debian Wiki, *Multiarch/Implementation*, https://wiki.debian.org/Multiarch/Implementation

[^5]: Debian Wiki, *Multiarch/Tuples*, https://wiki.debian.org/Multiarch/Tuples

[^6]: Linux Foundation, *Filesystem Hierarchy Standard 3.0*, section 3.10, https://refspecs.linuxfoundation.org/FHS_3.0/fhs/ch03s10.html

[^7]: Microsoft, */SUBSYSTEM (Specify Subsystem)*, https://learn.microsoft.com/en-us/cpp/build/reference/subsystem-specify-subsystem?view=msvc-170

[^8]: Microsoft, */MACHINE (Specify Target Platform)*, https://learn.microsoft.com/en-us/cpp/build/reference/machine-specify-target-platform?view=msvc-170

[^9]: Microsoft, *C runtime (CRT) and C++ standard library (STL) .lib files*. Section "C runtime (CRT) .lib files", https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-library-features?view=msvc-170

[^10]: GNU Compiler Collection, source file `gcc/config/gnu-user.h`, definition of `GNU_USER_TARGET_STARTFILE_SPEC`, https://github.com/gcc-mirror/gcc/blob/master/gcc/config/gnu-user.h

[^11]: Microsoft, *x64 prolog and epilog*, https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog?view=msvc-170

[^12]: GNU Binutils, *Using as*, section 7.46 ".hidden names", https://sourceware.org/binutils/docs/as/Hidden.html

[^13]: *System V Application Binary Interface*, draft of 24 April 2001, chapter 4, section "Symbol Table", https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.symtab.html

[^14]: Microsoft, *Exporting from a DLL*, https://learn.microsoft.com/en-us/cpp/build/exporting-from-a-dll?view=msvc-170

[^15]: *System V Application Binary Interface*, draft of 24 April 2001, chapter 4, section "Special Sections", https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.sheader.html

[^16]: *System V Application Binary Interface*, draft of 24 April 2001, chapter 5, section "Initialization and Termination Functions", https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.dynamic.html

[^17]: Apple, source file `EXTERNAL_HEADERS/mach-o/loader.h` of xnu, definition of `S_MOD_INIT_FUNC_POINTERS`, https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h

[^18]: Microsoft, *CRT initialization*, sections on the global initializers, https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-initialization?view=msvc-170

[^19]: Microsoft, *DLLs and Visual C++ runtime library behavior*, section "Default DLL entry point _DllMainCRTStartup", https://learn.microsoft.com/en-us/cpp/build/run-time-library-behavior?view=msvc-170

[^20]: Microsoft, *LIBRARY*, https://learn.microsoft.com/en-us/cpp/build/reference/library?view=msvc-170

[^21]: Microsoft, *EXPORTS*, https://learn.microsoft.com/en-us/cpp/build/reference/exports?view=msvc-170

[^22]: LLVM Project, source file `lld/COFF/Driver.cpp` of release 23.1.1, function `LinkerDriver::addLibSearchPaths`, which reads the environment variable `LIB`, https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/lld/COFF/Driver.cpp

[^23]: musl libc, file `COPYRIGHT` of release 1.2.6, https://musl.libc.org/releases/musl-1.2.6.tar.gz

[^24]: LLVM Project, *ld.lld(1)* of release 23.1.1, options `--static`, `--pie`, `--no-dynamic-linker` and `--strip-debug`, https://github.com/llvm/llvm-project/blob/llvmorg-23.1.1/lld/docs/ld.lld.1

[^25]: musl libc, file `WHATSNEW` of release 1.2.6, entry "rcrt1.o start file for static PIE", https://musl.libc.org/releases/musl-1.2.6.tar.gz

[^26]: Jake Shadle, *xwin*, file `README.md`, sections "Usage" and "xwin splat", https://github.com/Jake-Shadle/xwin

[^27]: Microsoft, *x64 exception handling*, sections "struct RUNTIME_FUNCTION", "Unwind operation code" and "Unwind helpers for MASM", https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64?view=msvc-170

[^28]: Microsoft, *ARM64 exception handling*, sections "Assumptions", ".pdata records" and "Unwind codes", https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling?view=msvc-170
