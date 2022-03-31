# Motivation
The motivation is to create a version of C++ which solves some of the biggest issues we have.
The goal is to make the new version very simple to pick-up by existing C++ programmers, and to make it reasonably easy to convert existing codebases to it.
Some of the changes are just additions, that could eventually become part of the C++ standard in the future, but some of them are not backwards compatibile.


# Changes

[No includes](#no-includes)

[Real enum class](#real-enum-class)

[*this* is a reference](#this-is-a-reference)

[Required this](#required-this)

[Required override](#required-override)

[Default explicit](#default-explicit)

[Default break in switch](#default-break-in-switch)

[Abort on wrong enum value in switch by default](#abort-on-wrong-enum-value-in-switch-by-default)

[Typed union](#typed-union)

[Safe navigation operator (?->)](#safe-navigation-operator)


## No includes
The main idea is simple, you should be just able to completely remove #include from the language without any replacement, including forward declarations.
When you want to start to use some symbol from the project you just start using it, it is there all the time, it doesn't matter if it is defined in cpp, or hpp, or in the same file later on, all symbols are available all the time.
We wouldn't have include path, but something very similiar, we could call it something like "scope-expansion-path", which would basically define all the files that are part of your code.
This implies, that it would no longer be possible to create two duplicate symbols on different compilation units, which is considered an advantage, as it forces better symbol scoping. (classes, namespaces)

The real goal is to actually improve compilation time, not make it worse, so core changes to the way compilation is done woudl have to be done, mainly we need to make sure that every cpp/hpp file is opened and compiled only once (or maybe twice if we have two step model for the symbols mapping).

Compared to the current model, when the string class hpp file (for example) can be parsed thousands of times in a project, as it is basically parsed for every compilation unit. This somewhat relates to the modules part of C++ standard.

The compiler needs to be able to detect and error on the circular symbol dependency.

## Real enum class
Member method of enums. There are currently way to *almost* achieve it by some tricks, but they are not perfect, and require some duplication.


Here is an simplified example of the current typical implenetation of enum-like class in C++
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
  constexpr Direction() : value(uint8_t(Direction::None)) {}
  constexpr Direction(Enum value) : value(uint8_t(value)) {}
  bool operator==(const Enum value) const { return this->value == uint8_t(value); }
  bool operator==(const Direction& direction) const { return this->value == direction.value; }
  bool operator!=(const Enum value) const { return this->value != uint8_t(value); }
  bool operator!=(const Direction& direction) const { return this->value != direction.value; }
  operator Enum() const { return Enum(this->value); }
  explicit operator bool() const { return this->value != uint8_t(Enum::None); }
  Enum getEnum() const { return Enum(this->value); }
  const char* str() const;
  constexpr bool isVertical() const { return this->value == North || this->value == South; }

  static const std::array<Direction, 4> all;
protected:
  uint8_t value;
};
```

The problems I would like to solve:
1. The duplication of the value definitions for nicer scope
2. The need of custom constructors and operators as the value is inside
3. The need to specify and fill allDirections for nice iteration of all enum values

The proposed syntax:

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
The only real reason, why this isn't a reference is historical, as this was added before references were a thing.
We are used to write this->, but it doesn't make sense, as you can't change this, and it shouldn't be nullptr. You can currently call member methods on nulllptr this, but I consider it a corner case.
This would make any usage of custom operators on this nicer, and make it more unified with rest of the code, where reference means that you can't change the value and it can't be nullptr.

I don't like to write
`(*this)[7]` (operator [] on this object) or `!*this` (negation of operator bool on this object)

Lets write it this way now:
`this[7]` and `!this`

## Required *this*
Simple as that, currently writing `this->x` is optional, we learned to put it into our code standards for a good reason. It is really useful to know, that we talk about x in the current class, and not a local or global variable.

It tends to happen, that adding a local variable can suddenly change the existing code in an unexpected way when the *this* is ommited.

## Required override
Adding the override keyword to methods that are overriding a base class method is currently optional, but some of the compilers are able to identify when its ommited and emit a warning.
This shouldn't be even a warning, lets make it always required.

## Default explicit
We have learned to put make almost all of the construcotrs and bool operators to be explicit, as otherwise, very unexpected things tends to happen otherwise.
So all constructors and conversion operators would be explicit by default, and "implicit" would have to be specified for the current default behaviour.

## Default break in switch
Missing break in swithc statements is one of the more annoyin gotchas in code like this:

```
void Position::move(Direction direction)
{
  switch (direction)
  {
    case Direction::North: this->y--; // oups missing break makes this behave differently than planned
    case Direction::East: this->x++;
    case Direction::South: this->y++;
    case Direction::West: this->x--;
  }
}
```
The fallthrough mechanics is useful from time to time, but is much less frequent, even the fact, that some compilers now require you to add [[fallthrough]] when you forget a break shows that this is an issue.

So the proposal would be, that when you actually want a fallthrough, you have to explicitelly state it.
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
1. Compile Error if not all of the values are mentioned (warning in most compilers)
2. Runtime abort (throw?) when invalid enum value is present

## Typed union
We are aware, that std::variant exist, but it has some problems.
1. The template magic behind it makes any bigger variant so unfriendly to fast compiled times, that we avoid it on purpose. (I have an experience, where a single boost variant with 200+ elements in a header file consumed more than 40% of compilation time of a big project)

2. The most typical usage of union is in tandem with enum class, which leads to a typical ugly boilierplate around it to make work.


The proposal is very WIP, but the goal would be to have something along these lines:

```
// type definition
typed union Property
{
  {
    {int, Integer}, // unlike some library class that would combine enum + variant, this makes sure that the enum and type definitions are defined together
    {double, Double},
    {std::string, String},
    {std::string, Comment}
  }

  std::string str(); // as our enum class, this allows to have methods
};

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

Property property;
property[Integer] = 5;
property[String] = "hello";
property[Comment] = "This is a comment";

if (property == Property::Integer)
  printf("%d", property[Integer]);
```

If the [] operator is used with wrong type, it would either throw, or return an empty value?

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

It shouldn't work only for pointers, but for objects with operator bool.

It should be also usable for cases when we want to return optional value, like this:

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

The return value of the chain is always related to the last thing in the chain, if its pointer, it just always returns null if it short-circuits.
It could also work for non-pointer objects, and in that case, it uses the Default constructor to return empty value in case of short-circuit.
