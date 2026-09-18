---
title: "Classes"
description: "How antic gives a class its table and descriptor, dispatches a call, runs the destruct chain, copies an object, checks a cast and devirtualises in release mode."
summary: "The compiler's side of the object model. The base as field 0, the table and the descriptor as read-only data, the indirect call and `super`, `dup` and the `destruct` chain, checked casts, static atomics and devirtualisation."
date: 2026-09-18T12:00:00+02:00
lastmod: 2026-09-18T12:00:00+02:00
draft: true
weight: 240
tags: [compilers, programming-languages]
keywords: [virtual dispatch, vtable, class descriptor, single inheritance, checked cast, devirtualisation, destructor chain, object layout]
---

## Previously

[Chapter 23, Libraries for C]({{% relref "/programming/writing-a-compiler/23-libraries-for-c" %}}), writes a C header and a library from
the exported items of a module. It gives C the view of a class: the nested layout, the
table type and one prototype per public function. This chapter is the compiler's side
of that view.

## The cost of a class

A struct is the data C declares and nothing else. A class adds one word to every
object, a handful of read-only globals per class, and one indirect call where the
concrete class is not known. Each interface it implements adds one word to the object
and a table of its own. Everything else it offers is compile-time work.

[Chapter 2, The Anti language]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}), specifies the model. This chapter
follows one class through the compiler, from the field the checker adds to the
instruction the back end writes.

## The base as field 0

A class names what it inherits on the first line of its body, and the checker resolves
that name before it builds any field list. The base then becomes the first field of the class,
named `super`.

```text
type anti.rt.Object = struct { table: ptr }
type classes.Shape = struct { super: anti.rt.Object, x: f32, y: f32 }
type classes.Circle = struct { super: classes.Shape, r: f32 }
```

The name is a keyword, so no declared field collides with it, and `self.super` reads
as ordinary field access. Nothing else about the layout is new. The rules of
[chapter 18]({{% relref "/programming/writing-a-compiler/18-structs-and-arrays" %}}) place field 0 at offset 0. They round the size
of the base up before the next field starts, which is what C does for a nested
struct.

A class without `inherits` takes `anti.rt.Object`, the root, which the compiler
declares itself. Its descriptor lives in the runtime, so two modules of one program
share one and `p is *Object` compares the same address wherever it is written. No compilation may define the module `anti.rt`, so `types_object` in
`src/types.c` builds it. It carries one field, the table pointer, which no program
names and every object holds at offset 0.

Every class of a chain therefore starts at the same address as the object. An
inherited field is reached at the offset it has in the class that declares it, with no
step per level, and `offset_of Shape.x` is correct on a `Circle`.

## Interfaces as sub-objects

`implements ser: Serializable,` places a sub-object of the abstract class
`Serializable` inside the class, at the field `ser`. The sub-object is an ordinary
field of the interface's type. It therefore begins with the table pointer of the
interface and holds the interface's own fields after it.

```text
type classes.Square = struct { super: classes.Shape, ink: classes.Ink, side: i64 }
```

A pointer to that field is therefore a pointer to the interface, and `&c` converts to
`*Serializable` by adding the offset of the field. That is the one conversion in the
language that changes an address, and `converts_to_interface` in `src/sema.c` finds
the field. An interface that two sub-objects both reach has no single answer, and the
checker refuses it.

## The table

The table of a class is read-only data with the descriptor at entry 0 and one entry
per public function of its chain.

```text
global classes.Circle.table [11]ptr { @classes.Circle.descriptor,
  @anti_rt_Object_type_name, @anti_rt_Object_to_text,
  @anti_rt_Object_equals, @anti_rt_Object_hash,
  @anti_rt_Object_serialize, @anti_rt_Object_destruct,
  @anti_rt_Object_copy, @classes.Shape.move, @classes.Shape.span,
  @classes.Circle.area }
```

The function `table_of` in `src/lower.c` walks the chain from the root down and
appends each public function. A function that repeats a name replaces the entry rather
than adding one. The entries of a base therefore keep their indices in every class
below it. That is why a call through a base pointer finds the right body. The index is
fixed at the class that first declared the name.

The seven functions of the root come first in every table, because the root is the
first class of every chain. Their bodies live in `rt/object.c`, which the next section
covers.

A table holds addresses of functions, so an IR constant needs a kind for one.
`IR_CONST_FUNC` names a function by index, as `IR_CONST_ADDR` names a global. A
relocation records which of the two its target is. Without that kind, the pass that
removes unused functions would drop a body that only a table reaches. It would then
renumber the rest without moving the addresses inside the table.

Every class a module declares builds its table, whether or not a literal of that
module constructs one. A library that declares a class it never builds still carries
the data its consumers need. An abstract class has no complete value and therefore no
table.

## Interface tables and thunks

Each interface sub-object has a table of its own, which repeats the interface's chain
in the same order. Its entries are thunks.

```text
fn classes.Square.ink.colour.thunk(%0: ptr) -> i64 {
b0:
    %1 = sub i64 0, offset_of classes.Square.ink
    %2 = ptradd %0, %1
    %3 = call i64 @classes.Square.colour(%2)
    ret i64 %3
}
```

A thunk takes `self` as a pointer to the sub-object, subtracts the offset of that
sub-object and calls the function of the class with the object. Every function is
therefore written once and serves the direct call and the interface call alike.

Where the class fills nothing, the interface's own body takes the entry, and no thunk
stands between them. That body already takes a pointer to the sub-object, which is
what the interface declared.

The thunk is an ordinary IR function, so no back end pattern is about it. `--dev` and
release mode build the same one.

## The descriptor

Entry 0 of the table points at the descriptor, which is what the runtime reads when
the compiler cannot know the concrete class.

```text
global classes.Circle.descriptor anti.rt.Descriptor {
  @classes.19, i64 6, @classes.Shape.descriptor, size_of classes.Circle,
  i64 2, @classes.Circle.ancestors, i64 1, @classes.Circle.fields, ptr 0,
  i64 0, i64 10, @classes.Circle.functions }
```

The descriptor names the class and points at the descriptor of its base. Then come
the size of the class as a symbolic value, which the back end folds, and the depth of
the class in its chain. Then come its ancestors, its field list and the `destruct`
body the class declares. Last come the offset of the sub-object and the function list
of the chain.

The `destruct` entry is the body of this class and not the one it inherits, because
`delete` runs one body per level. The table holds the last one, which is a different
question with a different answer.

The offset is zero in the descriptor of a class. An interface table points at a copy
of the same record. Its offset is the distance from the start of the object to the
sub-object. A pointer into the middle of an object therefore leads back to the whole
of it.
The function `anti_rt_object_of` in the runtime is that one subtraction. Every
`delete`, `destroy` and `dup`, and every checked cast and identity comparison, begins
with it.

### Ancestors

The ancestors of a class are its descriptors from the root down, one per level.

```text
global classes.Circle.ancestors [3]ptr { @anti_rt_Object_descriptor,
  @classes.Shape.descriptor, @classes.Circle.descriptor }
```

The entry at a level is the same address in every class below that level. That is what
makes `p is *T` a comparison rather than a walk. The entry at `T`'s depth is `T`'s
descriptor for every class below `T`, and for no other class.

### The field list

Each class lists the fields it declares, with the name, the offset, the kind and the
`own` bit of each. The parent descriptor holds the rest of the chain. A field of class
type, and a pointer to one, carry the descriptor of that class, so a walk of the list
reaches the whole object.

### The function list

Beside the fields, each descriptor lists the public functions of the whole chain, in
table order. A record holds the name, the entry of the table and the count of
parameters.

```text
global classes.Circle.functions [10]anti.rt.Function {
  { @classes.20, i64 9, i64 1, i64 1 }, ... }
```

The list is what `anti.reflect` of the standard library reads. The function `describe` gives the
descriptor of an object and `parent` walks up the chain. Then `field` and `function`
give one record each, and `get` and `set` reach a field by its index. The runtime does the
walking, because a descriptor is C data with C layout, and Anti reads the results as
plain values.

`--no-reflect` drops the field list and the function list and keeps the rest of the
descriptor. `is` and `as` still work, because they read the depth and the ancestors. A
program that never calls `to_text`, `equals`, `hash` or `serialize`, and whose classes
own nothing, pays nothing for reflection it does not use.

## The root in the runtime

`anti.rt.Object` declares seven public functions: `type_name`, `to_text`, `equals`,
`hash`, `serialize`, `destruct`, which is empty, and `copy`, which is the function behind
`dup`. The compiler declares the class, so the bodies live in C.

```c
struct anti_text anti_rt_Object_type_name(struct anti_object *self)
{
    const struct anti_descriptor *d = anti_rt_descriptor(self);
    struct anti_text text;

    text.ptr = d == NULL ? (const unsigned char *)"" : d->name;
    text.len = d == NULL ? 0 : d->name_length;
    return text;
}
```

The defaults walk the field list of the chain. The function `equals` compares the
bytes of every field of every level, and `hash` runs FNV-1a over the same bytes. The
function `copy` takes the size from the descriptor. They are slow by design. A class that needs speed replaces
the one it cares about with a `concrete fn` of the same name, and that replacement is
one entry in one table.

The checker builds one item per root function, so `v.type_name()` resolves like any
inherited call. A call on a pointer reads the entry of the table, and a call on a
value goes straight to the symbol of the runtime.

## Dispatch

A call on a class pointer may meet an object of a class below the static type, so it
reads the entry of the table.

```text
    %1 = load ptr %0
    %2 = mul i64 8, size_of ptr
    %3 = ptradd %1, %2
    %4 = load ptr %3
    %5 = call i64 %4 via @main.fn.0(%0)
```

The table is the first word of the object, the index is a constant, and the offset
stays symbolic because the IR holds no sizes. On x86_64 that is two loads and a call
through a register.

Four kinds of call are direct instead, because each names one body. A call on a value
knows its concrete class. A call of a private function reads a function with no entry.
A call of a `final fn` and a call on a `final class` have no class below them.

`self.super.f(args)` is direct as well. The base part of the object is a value, its
class is the base, and the entry is found at compile time.

## Devirtualisation

Release mode compiles the whole program in one call, so the checker sees every class
of the program. A public function that no class below the static type replaces then
has one body, and its call is direct.

The rule has two halves and both fall out of one scan. A public function nothing
replaces is direct wherever it is called. A class nothing inherits makes every call
through its pointer direct, because no class below it can replace anything.

The listings `devirt.ir` and `devirt.dev.ir` hold the same program in both modes.
Dev mode compiles one module, cannot see the rest of the program, and keeps the table.

One class stays on the table whatever the scan finds. A pointer to a class that another
class implements as an interface points into the middle of an object. The receiver of
a call through it needs an offset that only the thunk knows, so the call keeps its
load.

The choice happens in the checker and not in the optimizer, because the optimizer
works on IR that holds no classes. Once a call is a load and an indirect call, nothing
in the IR says which entries the load can reach.

## Construction

A literal of a class names the fields of the whole chain, in any order, and never
writes the base as a nested value.

```anti
let c = Circle { r: 2.0, x: 1.0 };
let heap = alloc Circle { r: 3.0 };
```

The checker flattens the chain into one list and checks the literal against it. A
literal outside the class names the public fields alone, and the rest take their
defaults.

Lowering stores the address of the table into the first word of the object. The table
of every interface sub-object follows, each at the offset of its field. It writes each named
field at the offset of the class that declares it, then the defaults of every level
the literal left out. Last it runs `construct`, base first down the chain, so a base
sees its own fields before the class below adds to them.

`alloc T { ... }` calls `malloc` with the size of the literal's type, writes the
literal into the result and runs the same chain.

`Circle(2.0)` and `alloc Circle(2.0)` write the defaults and then run the `construct`
that takes arguments. That one may fail, so it gives a `*Error` back and the branch
after it is the error branch of chapter 7. On a failure the heap form frees the object
before the handler runs, so nothing half-built survives.

For an export class the compiler also writes `anti_<Class>_init`, which does the same
to an object a C program allocated. Chapter 23 covers the C side of it.

## Casts

`p is *T` compares the ancestor of the object at `T`'s depth with `T`'s descriptor,
after comparing the depth, because an object shallower than `T` has no entry there.
Two comparisons, one branch each.

`p as *T` is the same test with a call of the runtime on the failing path. That call
prints the name of the class and ends the program. `p as? *T` writes null there
instead. A conversion to a base is the same address and needs no check at all, and one
to an interface adds the offset of the sub-object.

A cast that leaves a sub-object moves the pointer the other way. The test passes as it
does for a base, because the descriptor of an interface table carries the depth and
the ancestors of the concrete class. The result then moves back by the offset that
descriptor holds.

`==` and `!=` on two class pointers compare the identity of the objects. Each side
moves back to the start of its object first. A pointer to one interface of an object
and a pointer to another are therefore equal. The runtime does the move, because it also
answers for a null pointer.

## Ownership and the end of an object

`own` before a field of pointer or slice type says the object owns the memory behind
it. It is refused on `str`, which is immutable and shared, and on an inline class or
struct field, which the object owns already.

`dup(p)`, `delete(p)` and `destroy(p)` each become one call of the runtime with the
pointer alone. The runtime reads the descriptor, so the concrete class decides what
happens whatever the static type says.

```c
void anti_rt_destroy(void *object)
{
    const struct anti_descriptor *d;
    const struct anti_descriptor *level;
    int64_t i;

    object = anti_rt_object_of(object);
    d = anti_rt_descriptor(object);

    if (d == NULL) {
        return;
    }
    for (level = d; level != NULL; level = level->parent) {
        if (level->destruct != NULL) {
            level->destruct(object);
        }
```

The destruct body of the concrete class runs first and the root's last, so a class tears
down what it added before its base does. The `own` fields of every level are freed
after every body has run, so a body still reads what it owns. `delete` is `destroy`
and then one `free`.

A direct call of `destruct` is refused, and the message names `delete` and `destroy`.

A local of a class whose chain declares `destruct` or owns memory is torn down at the
end of its block. Lowering adds it to the exit actions of the block, beside the
deferred statements. It therefore runs on every way out, in reverse order of
declaration.
A heap object is never torn down by itself, and `delete` is the only way to free one.

## Static and atomic fields

`static atomic count: int = 0;` declares a field of the class and not of the object.
It becomes a global of the module that declares the class, with the declared value
written into it.

A static field is the one kind of global the program writes, so it goes to the
writable section of the object format. The sections that hold literals and relocated
constants are both protected, and a write to either faults.

An atomic field is read and written by calls alone, so that every access is one
operation of the memory model. Each call reaches the runtime with the address, the
symbolic width of the value and the operands. A narrower value extends to 64 bits on
the way in and truncates on the way out, so one runtime symbol serves every width.

## One instance

`singleton class Config { }` gets a generated `Config.get()`. The checker declares it
before any body names it, and lowering writes the body.

```text
fn one_instance.Config.get() -> ptr {
b0:
    %0 = addr @one_instance.Config.instance
    %1 = call ptr @anti_rt_atomic_load(%0, size_of ptr)
```

The instance lives in an atomic global of the module, which `get` reads. On the first
call it allocates an object, writes the table pointers and the defaults, and runs
`construct`. It then puts the result in the global with a compare and a swap. A thread
that loses the race frees what it made and reads the winner, so the program has one
instance.

A field of a singleton is read-only after creation, which `construct` alone may set.
`mutable` marks a field the program writes anywhere. The pass of chapter 10 refuses a
worker that reaches one.

## Bound functions

`let f = c.area;` is a value of two words: the object and the entry of its table.

```text
    %9 = load ptr %0
    %10 = mul i64 8, size_of ptr
    %11 = ptradd %9, %10
    %12 = load ptr %11
    store ptr %0, %2
    store ptr %12, %2, offset_of entry
```

A call loads both and calls the entry with the object as its first argument, so the
concrete class still decides which body runs. A `final` function and a `final` class
store the address of the body instead, because no class below them replaces it.

The type of a bound function is the signature without `self`, and it is a distinct
type from the plain function pointer of that signature. The two have different
layouts, and the checker keeps them apart.

## Tests

Every part of the object model runs end to end, and the example of the specification runs all of it at once. The two modes of dispatch are pinned as IR listings, and a library and a C program each use a class across a boundary. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 25, Where to take Anti next]({{% relref "/programming/writing-a-compiler/25-where-to-take-anti-next" %}}), closes the book. It
gives the shape of the ideas the compiler leaves open, from SSA to the optimisations
that follow it. It says where the language, its libraries and its tools live.
