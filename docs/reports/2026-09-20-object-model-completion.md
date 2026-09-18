# Object model completion

The five steps of `docs/work-order-completion.md`. All five are done. 836 tests
pass on the development Mac and none is skipped. The build has zero warnings,
`check_docs.py` and `check-web-content` report no error on the files this work
touched, and the site builds with drafts and without.

## New provisional decisions

Each is in `docs/decisions.md` with its reason, in order of appearance.

- `by -k` walks the values of `by k` in reverse and starts at the largest of
  them. The specification's rule and its procedure disagree, and the rule wins.
- A `use` field and an `implements` sub-object carry no visibility level. What
  they promote keeps its own.
- A failing function writes its result through its last parameter. A call one
  argument short leaves that place to the compiler.
- A static function of the error class builds an error rather than reporting
  one, so `Error.new` is a call like any other.
- The error a handler binds is deleted where the handler yields or falls off its
  end, which are the two exits the lowering sees.
- The pass that only reports walks the checked tree rather than the whole
  program's IR, because no point of the driver holds a whole program.
- `on_fatal` lives in `anti.error`, because the runtime owns `anti.rt`.
- `anti.reflect` reads descriptors and writes fields. It has no `call`, no `new`
  and no `Value`.
- A `return` of a local by name hands that local to the caller, and the local is
  not destroyed on the way out.
- A call through a class pointer is direct only for a `final fn`, a `final
  class`, a value receiver or a function with no table entry.
- The default `serialize` writes a field the field list cannot read as `null`. A
  `str` field is such a field.
- `Error.from_win32` is one function on every target, and gives code 0 away from
  Windows.
- An error of `anti.args` points its message at a builder the parser owns.

## What failed on the way

Six defects, each found by a program the work order asked for.

- The register allocator dropped a move whose operands resolved to one scratch
  register, and with it the store that wrote the destination's stack slot. The
  slot kept what was there before, and `anti.collection` crashed on its last
  `delete` with a pointer of `0x24`.
- A `return` of a local ran the scope-end destruction of that local. A static
  function that built a value and returned it then freed the memory it had
  given away. `anti.args` crashed at the first option it declared.
- A module that named a class of another wrote that class's table and descriptor
  a second time, and the linker refused the program.
- A call was made direct when no class the checker could see replaced the
  function. The checker reads one module, and a class that replaces it is
  declared in a module that imports this one. `anti.json` called the default
  `serialize` where the program had replaced it.
- `build_into` lost the aggregate copy of an ordinary call, and `lower_place`
  assumed a field for any operand of an operator. The specification's own
  example found both.
- The default `serialize` wrote the class name to file descriptor 1 and dropped
  the builder it was handed.

## Host only

Everything here ran on the development Mac, macos-arm64, with macos-x86_64 under
Rosetta. The other four targets assemble and link in the suite and run nowhere.
No machine here has an x86_64 processor or a Linux or Windows ARM64 one that the
suite reaches. `docs/vm-setup.md` holds the machines that exist.

## Not done

- Release-mode devirtualisation across the whole program. The checker reads one
  module, so the scan needs a pass the driver does not have.
- `anti.reflect` has no `call`, no `new` and no `Value`. `Object.deserialize`
  therefore builds no object. Both need a registry of every class of the
  program, which the link step would have to write.
- The `[module]` thresholds of the `ANTI_LOGGER` file. A program holds one
  logger with one name. A table of levels by module path needs a logger per
  module, which the module does not offer yet.
- `f"..."` interpolation, which `docs/decisions.md` defers to a section of
  chapter 26. The specification's example uses it in one line, so the pinned
  program and the chapter 2 listing write `"circle"` where the example writes
  `f"circle {self.r}"`. Every other line of the example is the program.
- The native libraries in `libs/`, which nothing builds.
- Atomic operations are calls of the runtime rather than inline instruction
  sequences, as the report before this one left them.

## The consistency pass

One line per chapter, against `docs/anti-object-model.md`.

- 1, parts of a compiler: nothing to change.
- 2, the Anti language: the objects section rewritten to the whole model, the
  specification's example as the listing, and the Heederik footnote.
- 3, setup: the runtime source list follows the build.
- 4, lexer: the keyword count, the seven contextual words and the new tokens.
- 5, parser: the class body, the statement keywords and the `alloc` forms.
- 6, semantic analysis: `inherits` in the body, four levels, replacement, the
  table layout, interfaces, operators, the handled error and ten messages.
- 7, intermediate representation: the error branch as IR.
- 8, lowering: tables, thunks, descriptors, `construct`, scope-end destruction,
  `super` and the error forms.
- 9, modules and library files: version 22, the field and function records, and
  every number of the hex listing measured again.
- 10, optimizer: devirtualisation as it stands, and the pass that only reports.
- 11, targets and ABIs: nothing to change.
- 12, instruction selection: nothing to change.
- 13, register allocation: nothing to change.
- 14, x86_64 back end: thunks, and the atomic list.
- 15, ARM64 back end: thunks, and the atomic list.
- 16, assembly per operating system: nothing to change.
- 17, floating point: nothing to change.
- 18, structs and arrays: `pub` on fields, the header's `/* private */`, and
  interface sub-objects in the layout.
- 19, strings: the listing follows the compiler.
- 20, function pointers: the listing follows the compiler.
- 21, testing six targets: nothing to change.
- 22, threads: `pub n`, the literal outside the class, the singleton check and
  `delete` in a worker.
- 23, libraries for C: the table type per interface, the two worked C views, the
  symbols and the third line of the program.
- 24, classes: the whole object model, interfaces and the singleton. It keeps
  `draft: true`.
- 25, where next: nothing to change.

## Questions

- The `[module]` thresholds of the logger file need a logger per module. Should
  `anti.log` offer `log.named("http")`, which gives a logger of that name with
  the sink of the configured one, or should the thresholds go?
- Release-mode devirtualisation and `anti.reflect.new` both want a registry that
  the link step writes. Is a whole-program pass over the IR of every module the
  next work, or does it wait?
