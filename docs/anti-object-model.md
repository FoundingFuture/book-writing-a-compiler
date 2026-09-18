# The Anti object model

The complete description of structs, enums, classes and errors in Anti. `docs/decisions.md` refers to the object model document and repeats none of it. A work order refers to it for what to build. The book's chapters must agree with it, and a difference is a bug in the chapter.

Settled on 2026-09-19. Replaces every earlier description.

Contents:

- [Principles](#principles)
- [Struct](#struct)
- [Enum](#enum)
- [Class declaration](#class-declaration)
- [Fields](#fields)
- [Functions](#functions)
- [Visibility](#visibility)
- [Inheritance](#inheritance)
- [Interfaces](#interfaces)
- [Composition](#composition)
- [Literals and construction](#literals-and-construction)
- [Destruction](#destruction)
- [Ownership and copies](#ownership-and-copies)
- [Pointers and conversions](#pointers-and-conversions)
- [Tables and dispatch](#tables-and-dispatch)
- [Descriptor and reflection](#descriptor-and-reflection)
- [The root class](#the-root-class)
- [Operators](#operators)
- [Static fields and singletons](#static-fields-and-singletons)
- [Threads](#threads)
- [Errors](#errors)
- [The C view](#the-c-view)
- [Messages](#messages)
- [Keywords](#keywords)
- [Not in the language](#not-in-the-language)
- [Example](#example)
- [Consistency checklist for the book](#consistency-checklist-for-the-book)

## Principles

- A struct is data that C declares, and nothing else. Its bytes are exactly the bytes C sees.
- A class is an object with behaviour. Its first field is a pointer to a table of functions. Every byte of a class is declared in the generated C header, so C can create, read and call it.
- Every rule is decided at compile time. The run-time work is a load and an indirect call for dispatch. A checked cast is a compare. Reflection is a lookup in a read-only table.
- Nothing runs that the program did not write, with three named exceptions. `construct` runs after creation. `destruct` runs at the end of a value's scope. Dispatch runs through a table.
- There is one implicit conversion in the language. A pointer to a class converts to a pointer to any of its bases and interfaces.

## Struct

- `struct Rect { x: f32, y: f32, w: f32, h: f32 }`. A struct body holds fields, nothing else. No functions, no constants, no defaults, no `inherits`, no visibility markers. Every field is readable and writable everywhere.
- A struct has C layout on the target. Unions, bitfields, `packed` and `align(N)` follow the rules of `docs/decisions.md`.
- The method-call sugar of the core applies. `v.f(args)` rewrites to `f(&v, args)` or `f(v, args)` for a free function `f` in the module that declares `v`'s type. The struct does not know the function exists.
- A struct bound from C with `anti bind` is a struct. An exported struct is a struct.
- A struct that appears in a class, or is exported, gets a descriptor in read-only data. The struct does not reference it. See [Descriptor and reflection](#descriptor-and-reflection).
- A struct cannot inherit, implement or `use` anything, and cannot be inherited or implemented.

## Enum

- `enum Color { Red, Green, Blue }` declares a named integer type with values from 0. `enum Mode: u8 { A = 1, B = 4 }` names the underlying type and gives explicit values. Without an underlying type it is `c_int`.
- An enum has C layout. It converts with `as` to and from its underlying type, both ways unchecked. It compares with `== != < <= > >=`. A value is written `Kind.Circle`.
- An enum value is a constant expression.
- `switch` on an enum without `else` must cover every value. The message names the missing ones.
- `anti bind` emits a C enum as an Anti enum, and the header writes `enum`.

## Class declaration

```anti
class Circle
{
	inherits Shape,
	implements ser: Serializable,
	implements dr: Drawable,

	r: f32,
	pub label: str = "circle",

	const MAX_R: f32 = 1000.0;
	static atomic count: int = 0;

	fn construct(self, r: f32) -> *Error { }
	fn destruct(self) { }
	pub fn area(self) -> f32 { }
	concrete fn Serializable::serialize(self, out: *text.Builder) { }
	protected fn recompute(self) { }
	fn helper(self) { }
	pub fn new(r: f32) -> Circle { }
}
```

- `class Name { }` declares a class. The body holds, in this order by convention and in any order by grammar: one `inherits` line, `implements` lines, `use` lines, fields, constants, static fields, functions.
- `inherits Base,` names the base class. At most one. Without it the class inherits `anti.rt.Object`, the root. The base is nested whole at offset 0, trailing padding included, and the class's own fields follow at `size_of(Base)`. The base part is reached as `self.super`.
- `implements name: Interface,` places an interface sub-object in the class at a named field. Any number. See [Interfaces](#interfaces).
- `use name: T,` is a field with promotion. See [Composition](#composition).
- `abstract class Name { }` is required for a class with any function that has no body, own or inherited. A class with an open function and no `abstract` is refused, naming the open functions.
- `final class Name { }` forbids inheritance. `final` is a contextual word before `class` and before `fn`.
- `singleton class Name { }` declares a class with one instance. See [Static fields and singletons](#static-fields-and-singletons).
- A class may not redeclare a field, function or constant that its base chain already has, except a `concrete fn`. Two `implements` of one interface in a class or its chain are refused.
- `packed` and `align(N)` apply to classes as to structs and leave a base, interface or `use` part as it is.
- The first field of every class is its table pointer. The Anti programmer never names it. See [Tables and dispatch](#tables-and-dispatch).

## Fields

- `name: T` declares a field. `name: T = e` gives a default, a constant expression, applied by every literal that omits the field.
- A field is private unless marked. `pub name: T` is visible everywhere. `protected name: T` is visible to the class and every class in its chain below. A field without a marker is visible to the class only. See [Visibility](#visibility).
- `own name: *T`, `own name: []T` and `own name: []byte` mark a pointer or slice field as owned. See [Ownership and copies](#ownership-and-copies).
- A field of class or struct type is inline and owned by definition. `own` on it is refused as redundant.
- `atomic name: T` declares an atomic field. See [Static fields and singletons](#static-fields-and-singletons).
- `const NAME: T = e;` declares a constant of the class, reached as `Name.NAME`. `self.NAME` is refused.

## Functions

- Functions are declared between the braces. Their symbol is `module.Class.f`. Two classes in one module may both declare `f`.
- `self` is the receiver, of type `*Class`, written as the first parameter. Fields are reached through `self.x`. A bare `x` in a function body is a local, a parameter or a module item, never a field.
- A function without `self` is a static function, called as `Class.f(args)`.
- `pub fn` has an entry in the class's table and is visible everywhere. `protected fn` and `fn` without a marker have no entry and are called directly. See [Visibility](#visibility).
- `abstract fn f(self) -> R;` is a `pub` function without a body.
- `concrete fn` marks a function that replaces an inherited entry. See [Tables and dispatch](#tables-and-dispatch).
- `final fn` forbids replacement in any derived class.
- `operator fn` marks a function that an operator calls. See [Operators](#operators).
- `fn construct(self, args...) -> *Error` and `fn destruct(self)` are the constructor and destructor. See [Literals and construction](#literals-and-construction) and [Destruction](#destruction).
- `v.f(args)` resolves in the class first, then its base chain, then its `use` fields, then the module. A field wins over a function. `Class.f(&v, args)` calls the same function without the sugar.
- `self.super.f(args)` calls the base class's entry for `f`, found in the base's table at compile time. It is a direct call.
- `let f = c.area;` is a bound function, a value of two words, object and entry, with the function type of `area` without `self`. `f()` calls it. No captures.

## Visibility

Four levels, and each applies where it makes sense:

| Marker | Module item | Class function | Class field |
|---|---|---|---|
| `pub` | every module | everywhere, in the table | everywhere |
| `internal` | modules under the same package root | not allowed | not allowed |
| `protected` | not allowed | the class and its chain below | the class and its chain below |
| none | the module | the class only | the class only |

- `internal` items go into the `.antl` interface marked as internal. The loader checks that the importing module shares the package root. `anti doc` puts them in the developer docs.
- `abstract fn`, `concrete fn` and `operator fn` are `pub`. `export fn` inside a class requires `pub`.
- A private or protected function is always a direct call. It has no table entry and does not appear in the C header.
- Visibility is checked in name resolution and costs nothing at run time. The C header declares every field regardless, with `/* private */` on the ones that are.
- `use` promotes `pub` members only. Reflection reaches every field.

## Inheritance

- One base per class, named by `inherits`. The chain ends at `anti.rt.Object`.
- The derived class adds fields after the base's. It cannot remove, reorder or retype a base field. A pointer to the derived class is a pointer to the base at the same address.
- A derived class inherits every table entry and every non-private function of its base. It replaces an entry with `concrete fn`. It reaches the replaced body with `self.super.f()`.
- `final class` stops the chain. `` `Circle` cannot inherit `final` class `Dot` `` is the message.
- A class with any open entry is abstract and must say so. An abstract class has no complete value: no `let s: Shape`, no `alloc(Shape, n)`, no `alloc Shape { }`, no literal, no plain field of type `Shape`. It appears as a base, as an interface, and behind a pointer.
- An interface in Anti is an abstract class. It may carry fields, constants, private helpers and public functions with bodies. Every derived or implementing class shares those bodies unless it replaces them. There is no other interface form.

## Interfaces

- `implements ser: Serializable,` places a sub-object of the abstract class `Serializable` inside the class at a named field. Only an abstract class may be implemented. A complete class or a struct is refused.
- The sub-object holds the interface's table pointer and the interface's fields. Its fields are reached as `self.ser.x`.
- `&c` converts to `*Serializable` by taking the address of the sub-object. The pointer points into the middle of the object, at a table for `Serializable`. That table belongs to `Circle` and its entries point at thunks. A thunk subtracts the sub-object's offset and jumps to `Circle`'s function with `self` as a `*Circle`. So every function is written once and serves both the direct and the interface call.
- A class may implement more than one interface. Each has its own sub-object and its own table. The same interface twice in a class or its chain is refused.
- An interface may inherit another abstract class. Its sub-object then contains the base's table pointer as well.
- Two implemented interfaces that declare a function with one name and one signature are filled by one unqualified `concrete fn`. Two that declare one name with different signatures are filled by two qualified ones. See [Tables and dispatch](#tables-and-dispatch).
- `implements` promotes the interface's `pub` members onto the class as `use` does. A name provided by two sub-objects is an error at the call, naming both.

## Composition

- `use name: T,` is an ordinary field with promotion. `T` is a struct or a complete class.
- The `pub` functions and the `pub` fields of `T` are reachable on the containing class. `v.f(args)` rewrites to `T.f(&v.name, args)`, and `v.x` to `v.name.x`. A promoted call is direct. The containing class is not `T` and does not convert to `*T`.
- A `use` field may sit anywhere, and a class may have more than one. Own names win. A name two `use` fields both provide is an error at the call that names both. Promotion follows a chain of `use` fields.
- The reading for chapter 2. A class inherits what it is. It implements what it can act as. It uses what it is made of.

## Literals and construction

- A class literal names fields of the whole inheritance chain directly, in any order: `Circle { x: 0.0, r: 2.0 }`. Defaults from any level apply. The base is never written as a nested value. `use` and `implements` fields are written nested by name, because two of them may hold the same field names.
- A literal outside the class may name `pub` fields only. Private and protected fields take their defaults. A literal inside the class's own functions may name any field.
- A literal sets the table pointers of the class and of every interface sub-object, writes every default, then runs `construct`. Lowering may copy a read-only prototype and store only the named fields.
- `fn construct(self)` runs after every literal and every `alloc` of the class, base first down the chain. It takes no arguments in this form and cannot fail.
- `fn construct(self, args...) -> *Error` takes arguments and may fail. The class is then created with `alloc Circle(10.0)` on the heap or `Circle(10.0)` as a value. Defaults are applied, then `construct` runs with the arguments. On an error the heap object is freed, or the value is discarded, and the error is handed to the caller. See [Errors](#errors). A base `construct` with arguments is called by name from the derived one, `self.super.construct(x)`, at the top of its body.
- One `construct` per class. Every alternative constructor is a static function with a name: `Circle.from_points(a, b)`.
- `alloc Circle { r: 2.0 }` allocates one object on the heap, writes the literal into it, runs `construct`, and returns `*Circle`. `alloc(T, n)` stays the raw form for any type and returns uninitialised memory.
- `[&c, &s]` has type `[2]*Shape` only when the context gives that type. Without context each element keeps its own pointer type.

## Destruction

- `fn destruct(self)` is the destructor. A class declares it for anything the `own` rule cannot express. It never calls `self.super.destruct()`, because the compiler chains it.
- `delete(p)` runs the concrete `destruct`, then each base's up the chain, frees the `own` fields at each level, then frees the object. `p` must be a heap object.
- `destroy(&c)` runs the same chain without the final free, for an object on the stack or inline in another object.
- A local of class type whose chain declares `destruct` or has `own` fields is destroyed at the end of its block, as if `destroy(&c)` had been written as the last `defer`. A heap object is never destroyed by itself.
- A direct call `c.destruct()` is refused.
- `destruct` does not run on an object whose `construct` failed. A `construct` that fails after acquiring something frees it before returning.
- The memory model stays C's for heap objects. Nothing frees a heap object but `delete`.

## Ownership and copies

- `own` before a pointer or slice field says the object owns the memory behind it. It is refused on `str`, which is immutable and shared. It is refused on inline class and struct fields, which are owned by definition. Ownership is a tree by rule, stated and not checked.
- `dup(p)` allocates the object's concrete size from the descriptor and copies it. Value fields copy. `own` fields and inline class fields get fresh memory and a copy of their contents, recursively. Other pointer fields copy the address. `Object.copy` is the function behind it, and a class may replace it with `concrete fn copy`. `dup` returns a pointer of the same static type as `p`.
- `=` between two values of a class with `own` fields anywhere in its chain is refused, because a byte copy would give two owners. `dup` is the way, and the message says so. `=` between values without `own` fields copies bytes, table pointers included.
- `serialize` follows `own` fields and writes other pointers as addresses. `equals` compares the contents of `own` fields and the addresses of others. `destruct` frees `own` fields.

## Pointers and conversions

- `&d` converts implicitly to `*B` for every `B` in `d`'s base chain, `*anti.rt.Object` included, and to `*I` for every interface `d` implements. A base conversion is the same address. An interface conversion adds the sub-object's offset. A conversion with two paths, an interface reached through two sub-objects, is refused, and the program names the path: `&c.ser as *Closable`.
- Values never convert. Passing a `Circle` where a `Shape` is expected is an error, and `c.super` is the explicit base part.
- `p is *T` gives a `bool`. `p as *T` on a class pointer is checked and traps on a mismatch. `p as? *T` gives `null` instead. All three work from any base or interface pointer, because every table's descriptor records the offset to the enclosing object.
- `==` and `!=` on class pointers compare object identity: each pointer is adjusted by its offset to the enclosing object, then the addresses are compared. So a `*Serializable` and a `*Drawable` taken from one circle are equal. Pointers of two unrelated class types are a type error. Struct pointers compare addresses.
- In dev mode `delete`, `destroy`, `dup`, `is`, `as` and every dispatch check the table pointer for null and trap with the class name. Release mode keeps the raw load.

## Tables and dispatch

- Every class has one primary table and one table per implemented interface. A table is a read-only global per concrete class, `anti_Circle_vtable` and `anti_Circle_Serializable_vtable`.
- Entry 0 of every table points at the class descriptor. The remaining entries are the `pub` functions.
- The primary table holds one entry per `pub` function of the base chain. Base entries come first in declaration order, own entries are appended. A derived class's table starts as a copy of its base's.
- An interface table holds one entry per `pub` function of the interface and its chain, in the same order. Its entries point at thunks.
- `concrete fn f(self)` unqualified fills every table that has an entry named `f` and no qualified body. All those entries must share one signature, or the message names the ones that differ.
- `concrete fn Circle::f(self)`, the class's own name, means the same as the unqualified form and may be written for clarity. Declaring both for one name is an error.
- `concrete fn Serializable::f(self)`, a base or an interface, fills that table only, with that table's signature. A qualified body wins in its table over an unqualified one.
- `concrete` is required on every replacement. A function that matches an inherited entry without it is an error. A `concrete fn` that matches nothing is an error. A replacement of a `final fn` is an error.
- An abstract entry is null until a class fills it. A class with any null entry is abstract.
- A call through a pointer is direct when the function is `final`, the class is `final`, or release mode proves no class replaces it. Otherwise it loads the entry and calls it with the object as `self`. Release mode devirtualises across the whole program. Dev mode compiles one module and relies on `final`.
- `self.super.f()` is a direct call to the base's entry. `Serializable.serialize(&self.ser)` is a direct call to the interface's own body by name.
- `c.f()` on a `*Circle` resolves to the unqualified function when there is one. When only qualified bodies exist for `f`, the call is ambiguous and the message names the interfaces. The caller writes `c.ser.f()` or converts to the interface pointer.

## Descriptor and reflection

- The class descriptor is a read-only record that entry 0 of every table points at. It holds the class name, the parent descriptor, size and alignment. It holds the depth in the chain and an array of ancestor descriptors. For an interface table it holds the offset to the enclosing object. It holds the field list, with name, offset, type id, visibility and an `own` bit. A class or struct field also carries its descriptor. It holds the function list, with name, signature id and table slot.
- `p is *T` is one comparison: the ancestor at `T`'s depth equals `T`'s descriptor. This is Cohen's display.
- A struct that appears in a class or is exported has a descriptor too, unreferenced by the struct. `type_of(T)` gives any type's descriptor.
- `anti.reflect` reads descriptors: `describe(obj)`, `fields(d)`, `get(obj, field) -> Value`, `set(obj, field, value)`, `functions(d)`, `call(obj, function, args: []Value) -> Value`, `new(name: str) -> *Object`. `Value` is a tagged union of the primitive types, `str` and pointers.
- `call` goes through the table like any dispatch. The compiler emits one trampoline per distinct signature in the program that unpacks a `[]Value` into a call. `new` reads a registry of every class descriptor that the link step writes, allocates, applies defaults and runs `construct`.
- Reflection reaches every field, private ones included, because the descriptor is the class's own data.
- `--no-reflect` drops the field list, the function list, the trampolines and the registry. It keeps the name, the parent, the size and the ancestors, so `is` and `as` still work.

## The root class

- `anti.rt.Object` is the base of every class without `inherits`. It has no fields beyond the table pointer.
- It declares seven `pub` functions with default bodies over the descriptor: `type_name(self) -> str`, `to_text(self) -> str`, `equals(self, other: *Object) -> bool`, `hash(self) -> u64`, `serialize(self, out: *text.Builder)`, `copy(self) -> *Object`, and `destruct(self)`, which is empty. A static `Object.deserialize` is the counterpart of `serialize`, in the format `anti.json` defines.
- A class may replace any of them with `concrete fn`. Replacing `equals` without `hash`, or the reverse, is a warning. The default bodies walk the field list and are slow by design.
- Every table starts with these seven entries after the descriptor pointer. A C program that has the header of one class knows the head of every table.

## Operators

- `operator fn add(self, other: Vec2) -> Vec2` marks a function that an operator calls. `operator` is a contextual word before `fn` and implies `pub`.
- The mapping is a closed table:

| Operator | Function | |
|---|---|---|
| `a + b` | `add` | |
| `a - b` | `sub` | |
| `a * b` | `mul` | |
| `a / b` | `div` | |
| `a % b` | `rem` | |
| `-a` | `neg` | unary |
| `a == b` | `eq` | returns `bool`. `!=` is `!eq` |
| `a < b` | `lt` | returns `bool`. `>` swaps, `<=` and `>=` derive |
| `a & b` | `and` | |
| `a \| b` | `or` | |
| `a ^ b` | `xor` | |
| `a << b` | `shl` | |
| `a >> b` | `shr` | |
| `~a` | `not` | unary |

- `a op b` rewrites to `a.f(b)` when the left operand's type declares `operator fn f`. Only the left operand's type is looked up. Compound assignments derive from the binary form. `operator fn` with a name outside the table is an error listing the fourteen.
- For a struct the operator is a free function in the module that declares the type: `operator fn add(a: Vector2, b: Vector2) -> Vector2`.
- Not overloadable: `=`, `&&`, `||`, `.`, `()`, `[]`, `as`.
- Without an `operator fn eq`, `==` on a class value or a struct value is undefined, as before. `==` on class pointers is identity, see [Pointers and conversions](#pointers-and-conversions).

## Static fields and singletons

- `static atomic count: int = 0;` declares a field of the class, reached as `Class.count`. It is a global with the class's mangled symbol. A `static` field must be `atomic`. Mutable globals exist only as atomics, so `parallel` keeps its guarantee.
- `atomic` applies to integer types, `bool` and pointers, on static and on instance fields. An atomic field has no `=` and no `+=`. It has `load()`, `store(v)`, `add(v)`, `sub(v)`, `and(v)`, `or(v)`, `swap(v)` and `compare_swap(expected, new) -> bool`. `add`, `sub`, `and`, `or` and `swap` return the value the field held before the operation. Each operation is a call into the runtime, compiled per target, and the closing guide lists inline sequences as a later optimisation. Reading it without `load()` is refused. Memory order is sequentially consistent, always.
- A static field is the one kind of global the program writes, so it goes to the writable data section of its object format. The sections that hold literals and relocated constants are both protected, and a write to either faults.
- `singleton class Config { }` declares a class with one instance. `Config.get() -> *Config` is generated: lazy, created once with `compare_swap`, from `construct` when the class declares one without arguments and from the defaults otherwise. No `alloc` of a singleton by the program, no literal of it outside the class, and `delete` on it is refused.
- In a singleton, a field without a marker is read-only after creation. It may be set in `construct` and nowhere else. `atomic` fields are as above. `mutable` fields are ordinary fields the program may write anywhere. `mutable` is a contextual word allowed only in a singleton.
- A read or write of a `mutable` singleton field inside a `worker fn` is an error naming the field and the worker. The same holds inside any function a `worker fn` calls. The check is a whole-program pass over IR, run in every build mode. See [Threads](#threads).

## Threads

- The pointer-free test of `parallel` exempts the table pointer and `own` fields. A class value whose other fields are pointer-free is pointer-free, so `[]Circle` chunks like any array. `[]*Shape` is refused with a message that points at `dispatch`.
- `dispatch obj -> f(args)` submits one object to the pool for `worker fn f(o: *T, args...) -> R`. The other arguments follow the pointer-free rule. It returns a `Job` struct. The pool records the object's address in an in-flight map before running and removes it after. A `dispatch` of an object already in flight returns a null job. `join(job) -> R` blocks and returns the result. `join_all(jobs: []Job)` waits for a set. A dispatched worker may not `delete` its object. Same pool, same inline fallback, same `ANTI_THREADS` as `parallel`. The object header stays one word, and the in-flight state is the pool's.
- Analysis that only reports runs on the whole program's IR in every build mode. It runs after the last module compiles and before the link. The singleton check, an abstract class no concrete class fills, and a `delete` inside a worker live there. Optimisation that changes code runs on the whole program in release mode only.

## Errors

- `anti.error.Error` is a class with `pub code: int`, `pub message: str` and `own cause: *Error`. The runtime owns `anti.rt`, so the class lives in the standard library's `anti.error` module. Libraries subclass it. `Error.new(code, message)`, `Error.from_errno()` and `Error.from_win32()` build one. `e.text()` gives code, message and the cause chain. `e.print()` writes it to stderr. `e.fatal()` prints and exits with `e.code`, or 1 when the code is 0. `rt.on_fatal(f)` registers one function that runs before `fatal` exits.
- A function that can fail returns `*Error`, `null` on success, and writes its results through out pointers. A function that cannot fail returns its value. A function whose only failure is "not present" may return `bool`.
- A call to a failing function must handle the error. A bare call that drops it is a compile error.
- `let n = f(args) catch e { ... };` handles it at the call. The compiler supplies the out pointer for `n`. The handler either leaves the enclosing block or ends with `yield v`, a value of `n`'s type that takes the place of the result. `catch { }` binds no name. `catch fatal` prints and exits.
- `try f(args)` is `f(args) catch e { return e; }` and is allowed only in a function that returns `*Error`.
- `try { ... } catch e { ... }` handles every unhandled failing call in the block. The first error abandons the rest of the block and runs `defer` statements on the way out. Then the handler runs, and execution continues after the block unless the handler left the function. Nothing crosses a function boundary, so nothing is unwound. Nested blocks bind inward.
- The name after `catch` is any identifier, scoped to the handler. It shadows an outer name, and `anti check` warns when it does.
- An error bound by `catch e` is deleted when the handler exits, by `yield`, by falling off the end, by `break` or `continue`. `return e` and `try` pass it to the caller instead. `catch { }` deletes it at once. A handler that keeps the error writes `dup(e)`, and a bare `e` stored into anything is refused.
- `construct` with an error return uses the same forms: `let c = alloc Circle(10.0) catch fatal;`.

## The C view

- The header of an exported class declares the full layout as nested C structs: `struct Shape { struct anti_Object base; float x; float y; }` and `struct Circle { struct Shape base; struct Serializable ser; float r; }`, with `struct anti_Object { const struct anti_Object_vtable *vtable; }` innermost. Private fields carry `/* private */`, `own` fields carry `/* own */`.
- It declares the table type per class and per interface, `struct Shape_vtable` and `struct Serializable_vtable`, with the descriptor pointer first and one function pointer per `pub` function of the chain. It declares one `extern const` table per concrete class per table, `anti_Circle_vtable` and `anti_Circle_Serializable_vtable`, and each descriptor as `extern const`.
- An abstract class gets layout and table type, no table symbol and no init, with a comment saying why.
- It declares a prototype per `pub` function, `float Circle_area(struct Circle *self);`, and per static function, `struct Circle Circle_new(float r);`. Private and protected functions do not appear. Static atomic fields appear as `_Atomic` globals under the `anti_` prefix.
- Every generated helper is under the `anti_` prefix and never collides with a user function: `anti_Circle_init(c)` sets every table pointer and writes every default, then runs `construct` when it takes no arguments. `anti_Circle_construct(c, args...)` runs a `construct` with arguments and returns `struct anti_Error *`. `anti_Circle_delete(c)` runs the `destruct` chain, frees `own` fields and the object. `anti_Circle_destroy(c)` runs the chain without the free. `anti_Shape_area(s)` is a `static inline` dispatch wrapper through the table, beside the direct `Shape_area`. `anti_Circle_as_Serializable(c)` returns the interface pointer.
- The header compiles as C11 and as C++17. Names that are keywords in either get a trailing `_`. No generated struct is named `class`.
- A bound function crosses as a struct of two pointers. A class is exportable when every `pub` function follows the export signature rule. `anti bind --header` writes all of the above from the `.antl`.
- A bound struct is never a class. A C library's struct has no table pointer and gets none.

## Messages

- `` `r` is private to `Circle` ``
- `` `recompute` is protected in `Shape` ``
- `` `helper` is private to `Counter` ``
- `` `Counter` has no function `rest` ``
- `` `Counter` has no field `rest` ``
- `` `Shape` has open functions `area` and `draw` and must be `abstract` ``
- `` `Shape` is abstract and has no complete value ``
- `` `Circle.area` replaces `Shape.area` and needs `concrete` ``
- `` `Circle.aera` is `concrete` and replaces nothing ``
- `` `Circle` cannot inherit `final` class `Dot` ``
- `` `Circle.area` replaces `final` function `Shape.area` ``
- `` `Serializable` is not abstract and cannot be implemented ``
- `` `serialize` is provided by both `ser` and `log` ``
- `` `&c` converts to `*Closable` through `ser` and through `w`, name one ``
- `` `r` is already a field of `Shape` ``
- `` `count` is `static` and must be `atomic` ``
- `` `destruct` is never called directly, use `delete` or `destroy` ``
- `` `title` is `mutable` in singleton `Config` and `worker fn render` reaches it ``
- `` `Circle` has `own` fields, use `dup` instead of `=` ``
- `` the error of `parse_int` is not handled ``
- `` `e` outlives its `catch`, use `dup` ``
- `` `[]Shape` holds no complete values, use `[]*Shape` ``
- `` `Sprite` replaces `equals` without `hash` `` as a warning
- `` `e` shadows a variable in scope `` as a warning from `anti check`

## Keywords

- Keywords: `class`, `self`, `super`, `abstract`, `concrete`, `enum`, `use`, `inherits`, `implements`, `is`, `dup`, `delete`, `destroy`, `static`, `singleton`, `internal`, `protected`, `catch`, `try`, `yield`. `atomic`, `dispatch`, `join` and `yield` were reserved already.
- Contextual words: `final`, `own`, `operator`, `mutable`. They join `packed`, `align`, `by`, `in` after a `for` binding and `fatal` after `catch`.
- Tokens: `::` in a `concrete fn` qualifier, `as?`, `=>` in `switch`.

## Not in the language

Multiple concrete bases and virtual bases. An `interface` keyword. `virtual` and `override`. Overloading by signature. Closures with captures and generics, which are on the roadmap after the book as compile-time features. Retroactive conformance, a type gaining an interface from outside its declaration. Value polymorphism. Exceptions. Properties and annotations. Nested and anonymous classes. Covariant return types beyond `dup`.

## Example

```anti
import anti.io;
import anti.text;
import anti.error;

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

abstract class Serializable
{
	abstract fn serialize(self, out: *text.Builder);
}

abstract class Shape
{
	inherits Drawable,

	pub kind: Kind,
	pub x: f32 = 0.0,
	pub y: f32 = 0.0,

	const MAX_SIDE: f32 = 1000.0;
	static atomic count: int = 0;

	abstract fn area(self) -> f32;

	fn construct(self)
	{
		Shape.count.add(1);
	}

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

final class Circle
{
	inherits Shape,
	implements ser: Serializable,

	r: f32,

	fn construct(self, r: f32) -> *error.Error
	{
		if r <= 0.0 {
			return error.Error.new(1, "radius must be positive");
		}
		self.r = r;
		return null;
	}

	concrete fn area(self) -> f32
	{
		return PI * self.r * self.r;
	}

	concrete fn draw(self)
	{
		io.println("circle");
	}

	concrete fn Serializable::serialize(self, out: *text.Builder)
	{
		out.append(f"circle {self.r}");
	}
}

class Square
{
	inherits Shape,

	side: f32,
	own label: []byte,

	pub fn new(side: f32) -> Square
	{
		let buf = []byte { ptr: alloc(byte, 8), len: 8 };
		return Square { kind: Kind.Square, side: side, label: buf };
	}

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
	pub texture: int,
}

class Vec2
{
	pub x: f32,
	pub y: f32,

	operator fn add(self, o: Vec2) -> Vec2
	{
		return Vec2 { x: self.x + o.x, y: self.y + o.y };
	}
}

singleton class Config
{
	path: str = "config.toml",
	atomic requests: int = 0,
	mutable title: str = "untitled",
}

fn total(shapes: []*Shape) -> f32
{
	let sum: f32 = 0.0;
	for s in shapes {
		sum = sum + s.area();
		s.draw();
	}
	return sum;
}

fn main() -> int
{
	let c = alloc Circle(2.0) catch fatal;
	defer delete(c);
	let s = Square.new(3.0);
	c.move(1.0, 1.0);
	let shapes: [2]*Shape = [c, &s];
	let t = total(shapes[0..2]);
	if shapes[1] is *Square {
		let q = shapes[1] as *Square;
		io.println(q.describe());
	}
	let copy = dup(c);
	let f = c.area;
	let a = f();
	assert(a > 12.0);
	delete(copy);
	let ser = c as *Serializable;
	let b = text.Builder.new();
	ser.serialize(&b);
	let sp = Sprite { rect: Rect { x: 0.0, y: 0.0, w: 8.0, h: 8.0 }, texture: 7 };
	sp.x = 4.0;
	let v = Vec2 { x: 1.0, y: 2.0 } + Vec2 { x: 3.0, y: 4.0 };
	Config.get().requests.add(1);
	io.println(Config.get().title);
	return 0;
}
```

The example shows, in order: an enum with an underlying type, a struct that is only data, two interfaces, an abstract class that inherits one and leaves both open. Then public fields with defaults, a constant, a static atomic, a `construct` without arguments, a shared function and a root call. Then a `final` class with a private field, a `construct` with an argument that can fail, three replacements including one qualified by interface. Then a class with an `own` field filled from `alloc` and a static `new`, a replacement that calls `super`, a private helper, a class that uses a struct, an operator, and a singleton. Then a `for` over a slice with dispatch, `alloc` with arguments and `catch fatal`, `defer delete`, `is` and a checked `as`, a deep `dup`, a bound function, an interface pointer, an operator in use, and the singleton in use. `s` is destroyed at the end of `main` because `Square` has an `own` field.

## Consistency checklist for the book

Every chapter is checked against the object model document. A chapter that says otherwise is corrected to match. The points most likely to disagree with older text:

- Chapter 2: structs are fields only. Classes are the only place for functions, defaults, constants, `inherits`, `implements` and `use`. Fields are private unless `pub`. `for` and `switch` exist. `construct` and `destruct` exist and run when the model says. The error forms are `catch`, `try` and the `try` block, and a bare failing call is an error.
- Chapter 4: the keywords and contextual words listed above, `::`, `as?` and `=>`.
- Chapter 5: the class body grammar, `abstract class`, `final`, `singleton class`, `alloc T { }` and `alloc T(args)`, `is`, `dup`, `delete`, `destroy`, `catch` and `try` forms, `operator fn`, `concrete fn X::f`.
- Chapter 6: resolution order, the four visibility levels, table layout and replacement checking, abstractness. The pointer conversions and the two-path refusal, the export rule, the handled-error rule, every message listed above.
- Chapter 7: atomic operations, `assert`, and the error branch as IR.
- Chapter 8: tables, thunks, descriptors and prototypes as read-only globals. Literal lowering, `construct` and `destruct` chains, scope-end destruction, the indirect call, `super`, `is`, `as`, `dup`, bound functions, `catch` and `try` lowering.
- Chapter 9: classes, tables, descriptors, `internal` and visibility bits in the interface and the IR of a library file.
- Chapter 10: devirtualisation in release mode, the whole-program analysis pass in every mode.
- Chapters 14 and 15: the atomic patterns, thunks.
- Chapter 18: class layout, interface sub-objects, the nested base with its padding, defaults in literals.
- Chapter 22: the exemptions, `dispatch`, `join`, the in-flight map, the singleton check.
- Chapter 23: the C view above, with the two worked examples.
- Chapter 24: the compiler's side of the model, in the order above.
- Every listing in every chapter compiles under the rules above and is pinned by a test.
