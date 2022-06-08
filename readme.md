# Lets make C++ great
The motivation is to create a version of C++ which solves some of the biggest issues we have.

## Issues
 - Stupid slow compilation times.
 - Stupid bloat of #include definitions and forward declarations.
 - The need to re-parse and recompile all the files we touch from scratch as we work.
 - No tight integration (fast and reliable) of the compiled code structure and the editor features.
 - Missing some basic syntactic sugar (mentioned below).
 - No option to be able to edit the code without the need to think about files or cpp/hpp coupling.
 - Limited support of multiplatform compilers (this leads to the never ending clashes between different compiler quirks when doing multiplatform development)
 - const corectness implied code duplication

## The goal
The goal is to make the new version of C++ very simple to pick-up by existing C++ programmers, and to make it reasonably easy to convert existing codebases to it, while being able to use all the existing C++ libraries easily.

# Changes
- [Compilation changes](compilation/index.md)
- [Real enum class](#real-enum-class)
- [*this* is a reference](#this-is-a-reference)
- [Required this](#required-this)
- [Required override](#required-override)
- [Implicit explicit, Explicit implicit](#implicit-explicit-explicit-implicit)
- [Default break in switch](#default-break-in-switch)
- [Abort on wrong enum value in switch by default](#abort-on-wrong-enum-value-in-switch-by-default)
- [Typed union](#typed-union)
- [Safe navigation operator (?->)](#safe-navigation-operator)
- [Elvis operator](#elvis-operator)
- [Const deduplication](#const-deduplication)
- [Named parameter passing](#named-parameter-passing)
- [Type based parameter resolution](#type-based-parameter-resolution)
- [Don't require typename for dependent types](#dont-require-typename-for-dependent-types)
- [Test tools](#test-tools)
- [accumulate](#accumulate)

# The adaptation cost
- [Using existing libraries](#using-existing-libraries)
- [Converting existing code](#converting-existing-code)
- [IDE support](#ide-support)

## Real enum class
This is mainly about allowing enums to have member methods. There are currently ways to *almost* achieve it by some tricks, but they are not perfect, and require a lot of  ugly boilerplate.

Here is a typical simplified example of the current typical implenetation of enum-like class in C++
```
class Direction
{
public:
  enum class Enum : uint8_t
  {
    North = 0,
    East = 1,
    South = 2,
    West = 3,
    None = 4
  };
  static constexpr uint8_t COUNT = 5;

  // Trick to move the enum definitions to scope of Direction, so the usage isn't Direction::Enum::North, but Direction::North.
  static constexpr Enum North = Enum::North;
  static constexpr Enum East = Enum::East;
  static constexpr Enum South = Enum::South;
  static constexpr Enum West = Enum::West;
  static constexpr Enum None = Enum::None;
  constexpr Direction() = default;
  constexpr Direction(Enum value) : value(value) {}
  bool operator==(const Enum value) const { return this->value == value; }
  bool operator==(const Direction& direction) const { return this->value == direction.value; }
  bool operator!=(const Enum value) const { return this->value != value; }
  bool operator!=(const Direction& direction) const { return this->value != direction.value; }
  operator Enum() const { return this->value; }
  explicit operator bool() const { return this->value != Enum::None; }
  Enum getEnum() const { return Enum(this->value); }
  const char* str() const;
  constexpr bool isVertical() const { return this->value == North || this->value == South; }

  static const std::array<Direction, 4> all;
private:
  Direction value = Direction::None;
};
```

The problems I would like to solve:
1. The duplication of the value definitions for nicer scope
2. The need of custom constructors and operators as the value is inside
3. The need to specify and fill allDirections for nice iteration of all enum values
4. Even with all the boilerplate, some of the expressions don't really work as if it was enum with classes, for example

```
Direction foo(bool useParameter, Direction input)
{
  return useParameter ? input : Direction::North; // some compilers have problem with this, as one result is the class Direction, and one is the enum
}
```

Instead, we would use the proposed syntax:

```
enum class Direction : uint8_t
{
  {
    North = 0,
    East = 1,
    South = 2,
    West = 3,
    None = 4
  }
  constexpr bool isVertical() const { return this->value == North || this->value == South; }
  constexpr explicit operator bool() const { return this->value != Enum::None; }
};
```

1. The class and the enum is the same thing, and member functions are allowed to be defined.
2. The str method has default implementation which returns "North", "East", "South", "West" and "None" values, custom implementation can be provided.
3. There will be a way to iterate all the values naturally, probably by letting the compiler auto-generate the static all property (or something else dunno)

This means, that all the standard ways to work with it would be possible:

The most frequent motivation, the for each loop
```
for (Direction direction : Direction::all)
  printf("%s", direction.str());
```

or basically any iterator based algorithms like `std::find_if(Direction::all.begin(), Direction::all.end(), [](Direction direction){ return direction.isVertical(); });`


## *this* is a reference
The only real reason why *this* isn't a reference is historical, as *this* was added before references were a thing.
We are used to write `this->`, but it doesn't make sense, as you can't change the value of *this*, and it shouldn't be nullptr. You can currently call member methods on nulllptr *this*, but I consider it a corner case, and some compilers actually treat the `this == nullptr` comparison to be invalid.
This would make any usage of custom operators on *this* nicer, and make it more unified with rest of the code, where reference means that you can't change the value and it can't be nullptr.

I don't like to write
`(*this)[7]` (operator [] on *this* object) or `!*this` (negation of operator bool on this object)

Lets write it this way now:
`this[7]` and `!this`

## Required *this*
Simple as that, currently writing `this->x` is optional, we learned to put it into our code standards for a good reason. It is really useful to know, that we talk about x in the current class, and not a local or global variable.

It tends to happen, that adding a local variable can suddenly change the existing code in an unexpected way when the *this* is ommited.

## Required override
Adding the override keyword to methods that are overriding a base class method is currently optional, but some of the compilers are able to identify when its ommited and emit a warning.
This shouldn't be even a warning, lets make it always required.

## Implicit explicit, explicit implicit
We have learned to put make almost all of the constructors and bool operators to be explicit, as otherwise, very unexpected things tends to happen.
So all constructors and conversion operators would be explicit by default, and "implicit" would have to be specified for the current default behaviour.

## Default break in switch
Missing break in switch statements is one of the more annoying gotchas in code like this:

```
void Position::move(Direction direction)
{
  switch (direction)
  {
    case Direction::North: this->y--; // oops missing break makes this behave differently than planned
    case Direction::East: this->x++;
    case Direction::South: this->y++;
    case Direction::West: this->x--;
  }
}
```
The fallthrough mechanic is useful from time to time, but is much less frequent, even the fact, that some compilers now require you to add `[[fallthrough]]` when you forget a break shows that this is an issue.

So the proposal would be, that when you actually want a fallthrough, you have to explicitly state it.
```
// alternative implementation of Direction::isVertical
Direction::isVertical()
{
  switch (this)
  {
    case Direction::North: fallthrough;
    case Direction::South: return true;
    case Direction::East: fallthrough;
    case Direction::West: return false;
  }
}
```

## Abort on wrong enum value in switch by default
In the above example of Direction::isVertical, we have to do some kind of error handling to make the compiler happy, and to make sure it crashes if there is a different (unsupported) value in the enum.

If switch doesn't contain a default, it would:
1. Compile Error if not all of the values are mentioned (warning in most compilers).
2. Runtime abort (throw?) when invalid enum value is present.

## Typed union
We are aware, that std::variant exist, but it has some problems.
1. The template magic behind it makes any bigger variant so unfriendly to compile times, that we avoid it on purpose. (I have an experience, where a single boost variant with 200+ elements in a header file consumed more than 40% of compilation time of a big project).
2. The most typical usage of union is in tandem with enum class, which leads to a typical ugly boilierplate around it to make work.

This is basically just extension of how the enum works, but every value has an associated union type value.
```
// type definition
union enum
{
  {
    {int, Integer}, // the important part is, that the type and enum definitions are defined together
    {double, Double},
    {std::string, String},
    {std::string, Comment}
  }

  std::string str(); // as our enum class, this allows to have methods
};

...

std::string Property::str()
{
  switch (this)
  {
    case Integer: return ssprintf("%d", this[Integer]);
    case Double: return ssprintf("%g", this[Integer]);
    case String: return this[String];
    case Comment: return ssprintf("//%s", this[Comment]);
  }
};

...

Property property;
property[Integer] = 5;
property[String] = "hello";
property[Comment] = "This is a comment";

if (property == Property::Integer)
  printf("%d", property[Integer]);
```

If the [] operator is used with wrong type, it would either throw, or return an empty value, but obviously, there would be more specific methods.

# Safe navigation operator
https://en.wikipedia.org/wiki/Safe_navigation_operator

For expressions without return value, the usage is pretty straightforward.

instead of
```
if (a)
  if (B* b = a->b)
    b->foo();
```
do
```
a?->b?->foo();
```

This would work for any types that are convertible to bool, so pointers and objects with bool operator would work.

Usage in expressions that return value are also possible

Instead of
```
B* getB(A* a)
{
  if (!a)
    return nullptr;
  return a->b;
}
```

We could write:
```
B* getB(A* a)
{
  return a?->b;
}
```

The return value of the chain is always related to the last thing in the chain, there are 2 requirements for this to be possible to use:
1. The final type needs to have a default value (in case of pointer it is nullptr, otherwise, the default constructor)
2. All the values in the chain need to be convertible to bool (as above)

# Elvis operator
https://en.wikipedia.org/wiki/Elvis_operator

Instead of writing:
``` return a ? a : b;```
we could write
``` return a ?: b;```

This is especially useful when `a` is an expression.

# Const deduplication
The *const* mechanics in C++ often implies code duplication or ugly hacks.

### Method const duplication
Very often, we end up with duplicate methods like this, for retrieving *const* and non-const versions of the data based on the const-ness of the parent object:

```
class A
{
public:
  const B* getB() const { return b; }
  B* getB() { return b; }
private:
  B* b;
}
```

### Class const duplication
Typical example are iterators, which have to be duplicated also in the standard library, for example:

`std::vector::iterator`
`std::vector::const_iterator`

The std uses template magic to partially deduplicate it, but is not a nice read.

So we could have a keyword *both_const*, which would be basically  mapped to the same value (*const* or non const) in the whole context of the method based on the current usage.
```
class A
{
public:
  both_const B* getB() both_const { return b; }
private:
  B* b;
};
```

For the class scope deduplication, we could use the keyword *class_const*, which would be used by this
```
class iterator
{
  class_const X& operator*() const { return *x; }
  class_const X* x;
};
```
This basically means, that we defined 2 types by this one definition (as with templates), one has const in in the place of all class_const, and one nothing, and they could be accessed like this:

```
iterator a;
iterator::const b; // as const_iterator, so you can move it, but not the value it points to
const iterator a; // you can't move it, but you can change the value
const iterator::const b; // as const const_iterator you can't move it nor change the value
```
When the iterator is retrieved from the container, we can use the *both_const* mechanism to specify which *subtype* of the iterator is to be returned to deduplicate even the begin/end methods.

```
class Container
{
  iterator::both_const begin() both_const { return iterator::both_const(data); }
  X* data;
};
```

Both of the class_const and both_const can be used to specify type.

In this example, we use the class_const of A to specify which const variant of B should be used for the b property.
```
class A
{
  class B
  {
    class_const int* x;
  };
  B::class_const b;
};
```

TODO:
1. There still needs to be some syntax to allow some methods to be used only in const or non-const version of the class
2. Consider having some constexpr if to check even inside methods
3. 
### Simpler final code deduplication
Since the compiler would know that both of the variants are close together, it could have easier time to deduplicate the const/non-const variants if possible.

# Named parameter passing
It would allow to specify which of the parameters with default values are specified in a function call.

So with this function:
```
int foo(int a = 1, int b = 2, int c = 3);
```

We could call it like this:
```
int foo(b : 3, c : 4);
```

The main advantages:
1. More readable code, especially when the parameters are bools or numbers
2. Deduplication, as having to pass the default value of *a* to be able to pass be basically duplicates the the definition of the default value
3. Shorter code

# Type based parameter resolution
Related to the previous, but based on type overloading

The function would have these parameters
```
int foo(A a = A(1), B b = B(2), C c = C(3));
```

Since A, B, C are different types, we can use the type resolution to figure out how to call the method without specifying the parameter name
```
foo(B(3), C(4));
```

# Don't require typename for dependent types
Not all compilers need that, so lets just not require it ever.
Same with the weird " template " word coming out of nowhere, just because the compiler can't figure it out.
https://en.cppreference.com/w/cpp/language/dependent_name

# Test tools
We should have ways to bring the test as close to the actual code as possible, and some standardised way to run/select the tests.

On the class levels, we might just have specially marked static methods testing the class related logic, ideally we would have a switch to disable/enable showing
these to avoid seeing bloat when discovering code structure.

We need a standardised test dependency system.

C++ code coverage tools are either expensive, or very basic, so it would be nice to have this as a standard part of the compiler.

# accumulate
We tend to have class of methods, which are virtual, but are meant to accumulate the code from all the levels of the hierarchy, for example the save method.

```
class A
{
public:
  virtual void save(Serialiser& output) const { output << x; }
  int x;
};

class B
{
  using super = A;
public:
  virtual void save(Serialiser& output) const override { super::save(output); output << y; }
  int y;
};
```

The equivalent syntax would be:

```
class A
{
public:
  accumulate void save(Serialiser& output) const { output << x; }
  int x;
};

class B
{
public:
  accumulate void save(Serialiser& output) const override { output << y; }
  int y;
};
```
We don't have to call the super::save(output), but we also don't need to define the super, as we use this idiom almost exclusively to create these accumulated calls.

# Using existing libraries
Based on what is mentioned in the [compilation changes](compilation/index.md) section, it might be possible to make it quite easy to use the existing libraries as is.
We could have a backwards-compatibile compilation mode which compiles the libraries in the old and slow way to generate our sombol-tree, which could be cached and used for compilation of our projects.

# Converting existing code
The cost of converting existing code would depend on the way the code is written, but for the typical cases, I can imagine that the conversion could be mostly automated.
1. Changing this to be required is probably going to be a lot of changes, but could be automated.
2. The same for changing this to be a reference.
3. Include removal is probably the easy part, as long as the program doesn't use #includes in some smart way, to introduce different sets of symbols or macros to the scope
4. With the global scope approach, it could happen that there could be clashes of symbols that were previously in separate cpp files.

# IDE support
This one is tricky, as there are lots of IDEs out there and we can't control them.
Yet, I can imagine that if the compiler is very performant, and provides some kind of API to the IDE to be able to understand the code without doing its own compilation in the background, the adaptation could be reasonably easy. I have no idea how much is this doable by some extensions, and how much it would require direct cooperations with the IDE creators.
