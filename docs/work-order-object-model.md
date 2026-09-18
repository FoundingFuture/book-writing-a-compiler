# Work order: object model, scope cut, finish the book

Read the whole file before doing anything. It has four parts. Part 1 is the object model, which replaces the "Object model" section of `docs/decisions.md` word for word. Part 2 holds the other decisions of 2026-09-18 and 2026-09-19, each to be recorded in `docs/decisions.md` under the section it names. Part 3 is the scope cut of the book. Part 4 is the work order and the rules.

You do not stop to ask. A question that needs a decision goes through the gap procedure. Search the designs and the log for the direction. Take the smallest option that keeps every struct C layout and the IR free of sizes. Record one `[provisional]` line with its reason. Continue. Questions appear in the report at the end and nowhere else.

# Part 1. Object model

Replace the "Object model" section of `docs/decisions.md`. The new text runs from the heading "Object model" up to the heading "End of the replacement section".

## Object model

Two kinds of aggregate. A `struct` is data that C declares, and nothing else. A `class` is an object with behaviour, a table pointer first and fields after it. Every byte of both is visible to C. Settled on 2026-09-19 and replaces every earlier version of this section.

### Struct

- A struct body holds fields, nothing else. No functions, no constants, no defaults, no `inherits`. It is exactly the bytes C declares. A bound struct is a struct, and an exported struct is a struct.
- The method-call sugar of the core stays. `v.f(args)` rewrites to `f(&v, args)` or `f(v, args)` for a free function `f` in the module that declares `v`'s type. The struct does not know the function exists.
- `enum Color { Red, Green, Blue }` declares a named integer type. `enum Mode: u8 { A = 1, B = 4 }` names the underlying type and gives explicit values. Without them the type is `c_int` and the values start at 0. An enum has C layout and converts with `as` to and from its underlying type. It compares with `== != < <= > >=`. `anti bind` emits a C enum as an Anti enum, and the header writes `enum`.
- Every struct that appears in a class or is exported gets a descriptor in read-only data. The struct itself does not reference it. Reflection over a class field of struct type reads it. C layout is untouched. `type_of(T)` gives any type's descriptor.

### Class declaration

- `class Circle inherits Shape { }` declares a class with a base. `class Shape { }` inherits `anti.rt.Object`, the root. A class has one base, named in the header and nowhere else. The base is nested whole at offset 0, trailing padding included. The class's own fields follow at `size_of(Base)`.
- The first field of every class is a pointer to its table. The Anti programmer never names it. The C header declares it. Fields follow in declaration order.
- `self.super` is the base part as a value. `self.super.f(args)` calls the base's entry for `f`, found in the base's table at compile time. It is a direct call.
- `abstract class Shape { }` is required for a class with any open function, own or inherited. A class with an open function and no `abstract` is refused at the class, naming the open functions. An abstract class is never a complete value. `let s: Shape`, `alloc(Shape, n)`, `alloc Shape { }`, a literal and a plain field of type `Shape` are refused. It appears as a base and behind a pointer, nothing else.
- An interface is an abstract class. It may carry fields, constants, private helpers and public functions with bodies. Every derived class shares those bodies unless it replaces them. There is no other interface form.
- `final class Circle` forbids inheritance. `final fn` forbids replacement. `final` is a contextual word before `class` or `fn`.
- A class body holds fields with optional defaults `x: f32 = 0.0`, constants `const N: T = e;` reached as `T.N`, static atomic fields, `use` fields, and functions.
- `use name: T` promotes the public functions and the fields of `T`, a struct or a class, onto the containing class. `v.f(args)` rewrites to `T.f(&v.name, args)`, and `v.x` to `v.name.x`. A `use` field may sit anywhere and a class may have more than one. Own names win. A name two `use` fields both provide is an error at the call, and the message names both. Promotion follows a chain of `use` fields. A promoted call is direct, and the containing class is not the promoted type.
- A class may not redeclare a field, function or constant that its chain already has, except a `concrete fn`. A struct may not inherit or `use` a class. `packed` and `align(N)` apply to classes as to structs and leave a base or `use` part as it is.

### Functions

- `self` is the receiver, of type `*T`, written as the first parameter. Fields are reached through `self.x`. A function without `self` is a static function, called as `T.f(args)`. Functions declared between the braces have the symbol `module.T.f`.
- `pub fn` is visible wherever the class is and has an entry in the table. `fn` without `pub` is visible only to the functions of the same class, on any instance, and has no entry. `abstract fn f(self) -> R;` is a `pub` function without a body.
- The table of a class holds one entry per `pub` function of its inheritance chain. Base entries come first in declaration order, own entries are appended. A derived class's table starts as a copy of its base's. A function declared with the name and signature of an inherited entry replaces it, and must carry `concrete`. A `concrete fn` that matches nothing is an error, and a match without `concrete` is an error. An abstract entry is null until a class fills it.
- `v.f(args)` resolves in the class first, then its chain, then its `use` fields, then the module. A field wins over a function. A call to a private function is always direct. A call through a pointer is direct when the function is `final`, the class is `final`, or release mode proves no class replaces it. Otherwise it loads the entry and calls it with the object as `self`.
- Release mode devirtualises. A `pub` function that no class replaces becomes a direct call everywhere. A class without subclasses makes every call through its pointer direct. Dev mode compiles one module and relies on `final`.
- `let f = c.area;` is a bound function, a value of two words, object and entry, with the function type of `area` without `self`. `f()` calls it. No captures.

### Literals and construction

- A class literal names the fields of the whole inheritance chain directly, in any order: `Circle { kind: Kind.Circle, x: 0.0, y: 0.0, r: 2.0 }`. Defaults from any level apply. The base is never written as a nested value. `use` fields are not flattened. They are ordinary fields with a name, because two `use` fields may hold the same field names.
- A literal sets the table pointer and writes every default. Lowering may copy a read-only prototype of the class and store only the named fields.
- `alloc Circle { r: 2.0 }` allocates one object on the heap, writes the literal into it, and returns `*Circle`. `alloc(T, n)` stays the raw form for any type.
- `[&c, &s]` has type `[2]*Shape` only when the context gives that type, as in `let shapes: [2]*Shape = [&c, &s];` or an argument of type `[]*Shape`. Without context each element keeps its own pointer type.
- `&d` converts to `*B` for every `B` in `d`'s chain, `*anti.rt.Object` included. The conversion is the same address and is the one implicit conversion of the language. Values never convert.

### Ownership, copy and destruction

- `own` before a field of type `*T`, `[]T` or `[]byte` says the object owns the memory behind it. `own` is a contextual word before a field name. It is refused on `str`, which is immutable and shared, and on a field of class or struct type, which is inline and owned by definition. Ownership is a tree by rule, stated and not checked.
- `dup(p)` allocates the object's concrete size from the descriptor and copies it. Value fields copy. `own` fields and inline class fields get fresh memory and a copy of their contents, recursively. Other pointer fields copy the address. `Object.copy` is the function behind it and a class may replace it with `concrete fn copy`. `dup` returns a pointer of the same static type as `p`.
- `drop` is special. `delete(p)` runs the concrete `drop` body, then each base's `drop` body up the chain. It frees the `own` fields at each level, then frees the object. `destroy(&c)` runs the same chain without the final free, for objects on the stack or inline in another object. A direct call `c.drop()` is refused. A class declares `concrete fn drop(self)` for anything the `own` rule cannot express. It never calls `self.super.drop()`, since the compiler chains it.
- The memory model stays C's. Nothing runs unless the program writes `delete` or `destroy`.

### Descriptor and root

- Entry 0 of every table points at the class descriptor, a read-only record. It holds the class name, the parent descriptor, size and alignment. It holds the depth in the chain and an array of ancestor descriptors. It holds the field list, with name, offset, type id and an `own` bit. A class or struct field also carries its descriptor. `--no-reflect` drops the field list and keeps the rest.
- `p is *T` is one comparison: the ancestor at `T`'s depth equals `T`'s descriptor. `p as *T` on a class pointer is checked the same way and traps on a mismatch. `p as? *T` gives `null` instead.
- In dev mode `delete`, `destroy`, `dup`, `is`, `as` and every dispatch check the table pointer for null and trap with the class name. Release mode keeps the raw load.
- `anti.rt.Object` declares six `pub` functions with default bodies over the descriptor. They are `type_name(self) -> str`, `to_text(self) -> str`, `equals(self, other: *Object) -> bool`, `hash(self) -> u64`, `serialize(self, out: *anti.text.Builder)`, and `drop(self)`, which is empty. `copy` is the seventh, behind `dup`. A static `Object.deserialize` is the counterpart of `serialize`. The default bodies walk the field list, follow `own` and inline fields, and treat other pointers as addresses. A class may replace any of them. Replacing `equals` without `hash`, or the reverse, is a warning. The defaults are slow by design, and the chapter says so.

### Static fields

- `static atomic count: int = 0;` in a class body declares a field of the class, not the object, reached as `Circle.count`. It is a global with the class's mangled symbol. A `static` field must be `atomic`. A plain `static x: int` is refused with a message that names the rule: mutable globals exist only as atomics, so `parallel` keeps its guarantee.
- `atomic` applies to integer types, `bool` and pointers. It applies to instance fields too. An atomic field has no `=` and no `+=`. It has `load()`, `store(v)`, `add(v)`, `sub(v)`, `and(v)`, `or(v)`, `swap(v)` and `compare_swap(expected, new) -> bool`. Reading it without `load()` is refused. Memory order is sequentially consistent, always.
- Lowering: `lock xadd`, `lock cmpxchg`, `xchg` and `mov` on x86_64. `ldadd`, `cas`, `swp`, `ldar` and `stlr` on ARM64 with LSE, or a load-store-exclusive loop without it.

### Threads

- The pointer-free test of `parallel` exempts the table pointer and `own` fields. A class value whose other fields are pointer-free is pointer-free, so `[]Circle` chunks like any array. `[]*Shape` is refused with a message that points at `dispatch`.
- `dispatch obj -> f(args)` submits one object to the pool for `worker fn f(o: *T, args...) -> R`. The other arguments follow the pointer-free rule. It returns a `Job` struct. The pool records the object's address in an in-flight map before running and removes it after. A `dispatch` of an object already in flight returns a null job. `join(job) -> R` blocks and returns the result. `join_all(jobs: []Job)` waits for a set. A dispatched worker may not `delete` its object. Same pool, same inline fallback, same `ANTI_THREADS` as `parallel`. The object header stays one word. The in-flight state is the pool's.

### C view

- The header of an exported class declares the full layout as nested C structs. `struct Shape { struct anti_Object base; float x; float y; }` and `struct Circle { struct Shape base; float r; }`, with `struct anti_Object { const struct anti_Object_vtable *vtable; }` innermost. `own` fields carry a comment.
- It declares the table type per class with the descriptor pointer first and one function pointer per `pub` function of the chain. It declares one `extern const` table per concrete class, `anti_Circle_vtable`, and each descriptor as `extern const`. An abstract class gets layout and table type, no table symbol and no `init`, with a comment saying why.
- It declares a prototype per `pub` function, `float Circle_area(struct Circle *self);`, and per static function. Private functions do not appear. Static atomic fields appear as `_Atomic` globals under the `anti_` prefix.
- Every generated helper is under the `anti_` prefix and never collides with a user function: `anti_Circle_init(c)` sets the table pointer through the chain and writes every default. `anti_Circle_delete(c)` runs the `drop` chain, frees `own` fields and the object. `anti_Circle_destroy(c)` runs the chain without the free. `anti_Shape_area(s)` is a `static inline` dispatch wrapper through the table, next to the direct `Shape_area`.
- The header compiles as C11 and as C++17. Names that are keywords in either get a trailing `_`, and no generated struct is named `class`.
- A bound function crosses as a struct of two pointers. A class is exportable when every `pub` function follows the export signature rule. `anti bind --header` writes all of the above from the `.antl`. A bound struct is never a class.

### Messages

- `` `reset` is private to `Counter` ``
- `` `Counter` has no function `rest` ``
- `` `Counter` has no field `rest` ``
- `` `Shape` has open functions `area` and `draw` and must be `abstract` ``
- `` `Shape` is abstract and has no complete value ``
- `` `Circle.area` replaces `Shape.area` and needs `concrete` ``
- `` `Circle.aera` is `concrete` and replaces nothing ``
- `` `Circle` cannot inherit `final` class `Dot` ``
- `` `move` is provided by both `hitbox` and `shape` ``
- `` `r` is already a field of `Shape` ``
- `` `count` is `static` and must be `atomic` ``
- `` `drop` is never called directly, use `delete` or `destroy` ``
- `` `Sprite` replaces `equals` without `hash` `` as a warning

### Keywords and words

- Keywords added: `class`, `self`, `super`, `abstract`, `concrete`, `enum`, `use`, `inherits`, `is`, `dup`, `delete`, `destroy`, `static`. `atomic`, `dispatch` and `join` were reserved already.
- Contextual words added: `final`, `own`. They join `packed` and `align`.

### Not in the language

Multiple inheritance, an `interface` keyword, `virtual`, `override`, constructors that run by themselves. Private fields, operator functions, closures with captures, an iterator protocol, a universal base for structs. The closing guide keeps a second table pointer as the sketch for orthogonal interfaces.

### Example

```anti
import anti.io;

const PI: f32 = 3.14159;

enum Kind: u8 { Circle, Square }

struct Rect
{
	x: f32,
	y: f32,
	w: f32,
	h: f32,
}

abstract class Drawable
{
	abstract fn draw(self);
}

abstract class Shape inherits Drawable
{
	kind: Kind,
	x: f32 = 0.0,
	y: f32 = 0.0,

	const MAX_SIDE: f32 = 1000.0;
	static atomic count: int = 0;

	abstract fn area(self) -> f32;

	pub fn move(self, dx: f32, dy: f32)
	{
		self.x = self.x + dx;
		self.y = self.y + dy;
	}

	pub fn describe(self) -> str
	{
		return self.type_name();
	}
}

final class Circle inherits Shape
{
	r: f32,

	pub fn new(r: f32) -> Circle
	{
		Shape.count.add(1);
		return Circle { kind: Kind.Circle, r: r };
	}

	concrete fn area(self) -> f32
	{
		return PI * self.r * self.r;
	}

	concrete fn draw(self)
	{
		io.println("circle");
	}
}

class Square inherits Shape
{
	side: f32,
	own label: []byte,

	concrete fn area(self) -> f32
	{
		return self.side * self.side;
	}

	concrete fn draw(self)
	{
		io.println("square");
	}

	concrete fn describe(self) -> str
	{
		self.clamp();
		return self.super.describe();
	}

	fn clamp(self)
	{
		if self.side > Shape.MAX_SIDE {
			self.side = Shape.MAX_SIDE;
		}
	}
}

class Sprite
{
	use rect: Rect,
	texture: int,
}

fn total(shapes: []*Shape) -> f32
{
	let sum = 0.0;
	let i = 0;
	while i < shapes.len do {
		sum = sum + shapes[i].area();
		shapes[i].draw();
		i = i + 1;
	}
	return sum;
}

fn main() -> int
{
	let c = Circle.new(2.0);
	let s = alloc Square { kind: Kind.Square, side: 3.0, label: b"sq" };
	defer delete(s);
	c.move(1.0, 1.0);
	let shapes: [2]*Shape = [&c, s];
	let t = total(shapes[0..2]);
	if shapes[1] is *Square {
		let q = shapes[1] as *Square;
		io.println(q.describe());
	}
	let copy = dup(s);
	let f = c.area;
	let a = f();
	assert(a > 12.0);
	delete(copy);
	let sp = Sprite { rect: Rect { x: 0.0, y: 0.0, w: 8.0, h: 8.0 }, texture: 7 };
	sp.x = 4.0;
	destroy(&c);
	return 0;
}
```

The example uses `defer` and `assert` from Part 2. It shows, in order: an enum with an underlying type, a struct that is only data, and an abstract class as an interface. Then a second abstract class that inherits it and leaves `draw` open. Then a shared function, a root call, defaults, a class constant, a static atomic. Then a `final` class, a heap object from `alloc` with a literal and an `own` field, a `concrete` replacement that calls `super`, a private helper, and a class that uses a struct's fields. Last, dispatch through `[]*Shape`, `is` and a checked `as`, a deep `dup`, a bound function, `delete` and `destroy`.

### Chapter changes

- Chapter 2: one section for the object model, in the order above. Its first sentence states the two kinds and what C sees of each.
- Chapter 4: the keywords and contextual words.
- Chapter 5: `class`, `abstract class`, `final`, `enum`, the class body, `alloc` with a literal, `is`, `dup`, `delete`, `destroy`, `static atomic`, bound functions.
- Chapter 6: the namespace per class, resolution order, visibility, table layout and replacement checking. Abstractness, the pointer conversion, the export rule, the thirteen messages.
- Chapter 7: atomic operations and `assert` as IR instructions.
- Chapter 8: tables, descriptors and prototypes as read-only globals. Literal lowering. The indirect call, `super`, `is`, `as`, `dup`, the `drop` chain, bound functions.
- Chapter 9: classes, tables and descriptors in the interface and the IR of a library file. The format version rises.
- Chapter 10: devirtualisation in release mode.
- Chapters 14 and 15: the atomic patterns.
- Chapter 18: the class layout, the nested base with its padding, defaults in literals.
- Chapter 22, threads: the exemptions, `dispatch`, `join`, the in-flight map.
- Chapter 23, libraries for C: the C view above, with the two examples of Part 3.
- The closing guide: the second table pointer sketch.

### Tests

- A struct body with a function, a default or a base is an error.
- A private call from outside, from a public function, and from a private function on another instance.
- Two classes with a function `f` in one module, called on each.
- Defaults: a literal that omits every defaulted field, and one that overrides a default. A literal that omits a field without a default is an error.
- Literals: a flattened literal naming fields from three levels. A nested base value in a literal as an error. A `use` field written nested. `alloc` with a literal.
- Class constants: `T.N` in a constant expression, `self.N` refused.
- Static atomic: a counter incremented from `parallel` chunks with the exact total, `static` without `atomic` refused, a read without `load()` refused.
- Enums: explicit values, the underlying type, `as` both ways, comparison, the header of an exported enum compiled by clang.
- `use`: promotion of a function and a field, own name winning, a clash between two `use` fields. `use` of a struct and of a class.
- Inheritance: layout checked by the ABI probe against a C struct with the base nested first. A chain of three, a base field name redeclared, a struct that inherits, inheriting a `final` class.
- Table layout: the table of a three-class chain decoded from the read-only section. Base entries first, replaced entries at the derived function, unreplaced at the base function, abstract entries null.
- Pointer conversion: `&d` to every base in the chain and to `*Object`, a value where a base value is expected, `as` back.
- Contracts: a missing `concrete fn`, a match without `concrete`, a `concrete fn` matching nothing, a deeper replacement, `super`. A class with an open function and no `abstract`. An open function inherited two levels up.
- Dispatch: a `[]*Shape` summed through `area`, the same sum on the concrete types, `draw` through `*Drawable`. A `final` function called directly through a base pointer. The release build calling a never-replaced function directly, checked in the assembly.
- Descriptor: `type_name` on every class, `is` for every pair in the chain, a checked `as` that traps, `as?` giving `null`, a null table pointer trapping in dev mode with the class name.
- Ownership: `dup` of an object with an `own` slice and an inline class field, both copied deep. `delete` running three `drop` bodies in order and freeing `own` fields, checked with the leak counter. `destroy` on a stack object. A direct `drop()` call refused.
- Root functions: `to_text` and `equals` over the example's classes, `hash` equal for equal objects, `serialize` round trip, the `equals` without `hash` warning.
- Bound functions: a bound `area` called after the object moved, and one passed to a C function through the header.
- Threads: `[]Circle` through `parallel`, `[]*Shape` refused, `dispatch` of one object twice with the second returning a null job, `join_all` over a set.
- The C view: a C program that allocates a `Circle`, calls `anti_Circle_init`, `Circle_area`, `anti_Shape_area` and `anti_Circle_delete`, and walks the descriptor to print field names. Compiled with `cc -std=c11` and `c++ -std=c++17` on every host.
- Every message above pinned by a test.

## End of the replacement section

# Part 2. Other decisions

Record each under the section named, dated 2026-09-19.

## Core language

- `assert(cond)` and `assert(cond, "message")` are a keyword. Lowering emits an IR instruction. In dev mode the back end emits a compare, a branch and a call to the runtime's failure routine. File, line and the expression text sit in read-only data. In release mode it emits nothing, the condition is not evaluated, and the strings are not in the binary. `--asserts` and `--no-asserts` override either mode. The decision is made in the back end, so assertions inside a `.antl` follow the build that compiles the program. The failure routine prints `main.anti:14: assertion failed: n > 0` to stderr and aborts. A condition that contains a call gets a warning in dev mode, because it does not run in release.
- `switch e { A => stmt, B => { block }, else => stmt }` on an enum or an integer, no fallthrough, `else` for the rest. A `switch` on an enum without `else` must cover every value, and the message names the missing ones. Lowering is a compare chain or a jump table.
- `for i in lo..hi { }` and `for x in slice { }` are sugar over `while`. The bound is evaluated once. `i` and `x` are read-only in the body. `x` is a copy of the element, and `for x in &slice` gives a pointer to it. `break` and `continue` work as in `while`.
- `defer stmt;` runs the statement at every exit of the enclosing block, in reverse order of declaration. Lowering inserts the statements before each `return`, `break` and `continue` that leaves the block and at the closing brace. No unwinding.
- Keywords added: `assert`, `switch`, `for`, `defer`. `for` leaves the reserved list and becomes a statement.
- No `volatile` and no `register`. The targets are user-space programs on six desktop operating systems, so there are no device registers, no signal handlers and no `setjmp`. Threads use `atomic`. The optimizer never removes a load or store through a pointer whose target it cannot see, which holds because it has no alias analysis. `anti bind` maps `volatile` in a C declaration to the plain type, with a comment in the binding. A scalar local whose address is never taken lives in a register already, so `register` has nothing to ask for.

## Optimizer and register allocation

- Store-to-load forwarding within a block. The optimizer walks each block. Per address expression it remembers the last value stored to it or loaded from it. A load from a remembered address becomes a copy of that value. Any call, any atomic operation and any store to a different address wipe the memory. The optimizer has no alias analysis. A call through a bound function or a table wipes it as any call does. The pass runs before copy propagation, so the copy it makes is removed. Chapter 10, last section, "Memory".
- Scalar replacement of aggregates. A struct or class local whose address never escapes is split into one temporary per field. An address escapes when `&` is taken of it or of a field. It escapes when it is passed as a pointer or stored into another object. The allocator then keeps the fields in registers. `self` is a parameter and escapes by nature. Chapter 10, same section.
- Spill weights. Every virtual register gets a cost. It is the number of its uses and definitions, each multiplied by ten for every loop it sits inside. Lowering records a loop depth per block. When the allocator runs out of registers it spills the candidate with the lowest cost. It no longer spills the interval that ends last. A constant never spills, it is re-emitted at its use. Chapter 13, section "Spill cost", with a test that shows a loop counter staying in a register.
- Each back-end chapter ends with a section "Performance". It lists the rules the back end applies and says that far more is possible outside the book. It names the Intel and AMD optimization manuals, Agner Fog's manuals and Arm's software optimization guides. It names the Apple Silicon CPU Optimization Guide by name only, since its licence decides what may be quoted. The rules stated for x86_64: 32-bit operations for narrow values, `lea` for add and shift, `test` before `jcc`, 16-byte alignment of loop heads. A remark that `div` costs twenty to ninety cycles. ARM64 uses `madd` and `msub`, keeps `adrp` and its `add` together, and does not extend a value a `w` write already zero-extended. `csel`, `ldp` and `stp` pairing, and `x / const` as a multiply are named as the next peephole rules and left to the reader.
- The closing guide's optimisation page lists the roadmap after the book in order. SSA with mem2reg, value numbering, loop invariant code motion and strength reduction, inlining, if-conversion, live-range splitting. It states that vectorisation is the gap that stays. A benchmark suite of ten programs with C twins compiled by clang at `-O2` is the first thing to build. The ratios are published per release on anti-lang.com.

## Threading

- The threading section gains the exemptions and `dispatch`, `join`, `join_all` from Part 1. The pool keeps an in-flight map keyed by object address. The object header stays one word.

## Standard library

These decisions are the language's, not the book's. They are recorded here and documented on anti-lang.com. Only the minimum in Part 3 is built now.

- A type is a struct unless it needs a table. Data that C might create, pass or index is a struct with free functions. A type is a class when it needs dispatch, ownership or a `drop`. A standard library function takes and returns structs and primitives where it can.
- Errors. `anti.rt.Error` is a class with `code: int`, `message: str` and `own cause: *Error`. A function that can fail returns `*Error`, `null` on success, and writes its results through out pointers. A function that cannot fail returns its value. A function whose only failure is "not present" may return `bool`. The caller owns a returned error and either deletes it or returns it upward. Libraries subclass `Error`, and callers use `is`. Functions: `Error.new(code, message)`, `Error.from_errno()`, `Error.from_win32()`, `e.text()` with the cause chain, `e.print()` to stderr, `e.fatal()` printing then exiting with `e.code` or 1, and `rt.check(e)` calling `fatal` on a non-null error.
- Logging, `anti.log`. Levels `trace debug info warn error fatal` as an enum. `Sink` is an abstract class with `write(self, line: str)`. `StderrSink`, `FileSink` and `CallbackSink` around a C function pointer ship with it. `Logger` holds a level, a sink, a format and a name. `class Log { static atomic current: *Logger }` is the global. `log.set(logger)` swaps it. The default is `StderrSink`, level `info`, text with a timestamp. Each line is one `write` call, so lines never interleave. `ANTI_LOGGER=<path>` names a TOML file read once before `main`. It holds `level`, `format`, `[[sink]]` entries with `kind`, `path` and `level`, and `[module]` thresholds by module path. A missing or invalid file prints one line to stderr and the program continues with the default. `log.set` in code takes precedence. `log.register(kind, factory)` adds a configurable sink kind. The runtime carries the TOML subset parser as a C function.
- Modules planned: `anti.time` with a monotonic clock, wall time, sleep and a `Duration` struct. `anti.random` as a seedable class. `anti.args` for declarative command-line parsing with generated `--help`. `anti.collection` with `List` and `Map` over `*Object` and `str` and integer keys. `anti.json` as the format of `serialize` and `deserialize`. `anti.toml`, reading only. Later: a `Signal` class of bound functions, `anti.process`, `anti.crypto`, `anti.debug`.
- Refused: an iterator protocol on the root, a general event loop, a universal base for structs.

## Names and publication

- `anti doc` emits plain semantic HTML with a fixed small set of class names and no styling. The stylesheet is separate and replaceable. The reference on anti-lang.com is generated from the runtime archive's `.antl` files at every release and never hand-edited.
- Every code sample on anti-lang.com lives in the repository under `examples/` and is compiled by `anti check` and `anti test` in the release job.

# Part 3. Scope of the book

The book on foundingfuture.com is about writing a compiler. It ends with a compiler that compiles small programs for six targets. It is not the reference for the language, the standard library, the build tool or the packages. Those live on anti-lang.com.

## Chapters

Chapters 1 to 21 stay. Part V becomes four chapters:

- 22. Threads. `worker`, `parallel`, `dispatch`, `join`, the pool, the platform layer.
- 23. Libraries for C. `export`, the header, static and shared libraries, the runtime in both forms, the C view of classes. Two worked examples. A C program that uses an Anti library with classes. An Anti class that wraps a C struct from a bound library.
- 24. Classes. Tables, descriptors, dispatch, the `drop` chain, `dup`, checked casts, devirtualisation, static atomics. This is the compiler's side of Part 1 and the best late chapter the book has.
- 25. Where to take Anti next. The closing guide, which now also points at anti-lang.com for everything the book does not cover.

The chapters "The runtime archive", "The build tool" and "Standard library" leave the book. Their text moves to `docs/site/` as design pages for anti-lang.com. `docs/table-of-contents.md` lists 25 chapters. `docs/decisions.md` keeps every decision. Decisions are the language's whether or not a chapter explains them.

Chapter 1 gains one paragraph: this compiler is the same one that builds the language. The language, its libraries and its tools live at anti-lang.com. The closing guide gains one page saying the same in detail.

## The standard library in the book

The book uses the smallest standard library that lets its programs print and exit. That is `anti.io` with `print`, `println`, `eprint`, `eprintln` and `exit`, and `anti.text` with `equal` and `from_c`. Nothing else appears in a chapter. Chapter 24 uses `anti.rt.Object` because the compiler emits its tables.

The book shows how those modules are made: a module in `std/` compiled to a `.antl` in the runtime archive, found through `--runtime`, calling a C function in `rt/`. It says in one sentence that the real standard library is larger and lives on anti-lang.com, and that this is not its reference.

Everything else in `std/` is built for the language, tested, and shipped in the runtime archive, and no chapter mentions it.

# Part 4. Work order

1. Push what is unpushed. Force-push the rebuilt series if it is still on a local branch. `main` is the series by rule.
2. Apply Part 1 to `docs/decisions.md`, Part 2 to its sections, Part 3 to `docs/table-of-contents.md` and `CLAUDE.md`. One commit.
3. Strip structs to fields only. Everything built for functions, defaults, abstract functions or `inherits` on structs moves to classes or is deleted. No struct-side variant remains.
4. Implement the language additions of Part 2: `assert`, `switch`, `for`, `defer`, with chapter text and tests.
4b. Implement store-to-load forwarding and scalar replacement in chapter 10, with tests that compare the IR before and after on a method body that reads a field twice.
4c. Implement spill weights and constant rematerialisation in chapter 13, with the loop counter test. Add the "Performance" section to chapters 14 and 15.
5. Implement Part 1 in the compiler, the runtime and the chapters it names, with every test in its list. Pin the example as a chapter 2 listing.
6. Move the three chapters out of the book into `docs/site/`. Renumber. Rewrite chapter 1's paragraph, the closing guide's page, and every "Previously" and "Next" affected.
7. Write chapter 22, then 23 with its two examples, then 24. Leave draft.
8. Build the minimum standard library of Part 3 as the book uses it. Build the rest of `std/` from the decisions as far as the runtime archive allows. With tests, without chapter text.
9. The test-mention rollout and the chapter 3 terminal output. Then the remaining items of the earlier finish task that still apply after the scope cut.
10. The report.

## Rules, unchanged

- Warnings are errors. All tests pass on the host before every commit. The chapter tags rebuild and pass. The docs-style checker reports zero findings on every touched file. The site builds with and without drafts.
- No workflow runs. Every workflow stays `workflow_dispatch` only.
- One commit per logical change. Push after every completed step above.
- Never a TODO, a placeholder or an unfinished sentence in chapter text.
- If a step fails after a reasonable number of attempts, isolate it and note it in the report. Move on, and return at the end.

## Report

`docs/reports/2026-09-19-object-model.md`: the new `[provisional]` entries with reasons, what failed and why, what is host-only, and what is not done with the reason. Under two pages. That is the only place a question may appear.
