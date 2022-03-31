# Motivation
The motivation is to create a version of C++ which solves some of the biggest issues we have.
The goal is to make the new version very simple to pick-up by existing C++ programmers, and to make it reasonably easy to convert existing codebases to it.
Some of the changes are just additions, that could eventually become part of the C++ standard in the future, but some of them are not backwards compatibile.


# Changes

## No includes
The main idea is simple, you should be just able to completely remove #include from the language without any replacement.
When you want to start to use some symbol from the project you just start using it, it is there all the time, it doesn't matter if it is defined in cpp, or hpp, or in the same file later on, all symbols are available all the time.
We wouldn't have include path, but something very similiar, we could call it something like "scope-expansion-path", which would basically define all the files that are part of your code.
This implies, that it would no longer be possible to create two duplicate symbols on different compilation units, which is considered an advantage, as it forces better symbol scoping. (classes, namespaces)

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
3. There will a way to iterate all the values naturally, probably by letting the compiler auto-generate the static all property (or something else dunno)

This means, that all the standard ways to work with it would be possible:

The most frequent motivation, the for each loop
```
for (Direction direction : Direction::all)
  printf("%s", direction.str());
```

or basically any iterator based algorithms like `std::find_if(Direction::all.begin(), Direction::all.end(), [](Direction direction){ return direction.isVertical(); });`


## this is a reference
We are used to write this->, but it doesn't make sense, as you can't change this, and it shouldn't be nullptr. You can currently call member methods on nulllptr this, but I consider it a corner case.
This would make any usage of custom operators on this nicer, and make it more unified with rest of the code, where reference means that you can't change the value and it can't be nullptr.

I don't like to write
`(*this)[7]` or `!*this`
Lets make it
`this[7]` and `!this`

## It is requried to specify this
Simple as that, currently writing `this->x` is optional, we learned to put it into our code standards for a good reason. It is really useful to know, that we talk about x in the current class, and not a local or global variable.
