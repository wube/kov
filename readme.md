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
## this is a reference
We are used to write this->, but it doesn't make sense, as you can't change this, and it shouldn't be nullptr. You can currently call member methods on nulllptr this, but I consider it a corner case.
This would make any usage of custom operators on this nicer, and make it more unified with rest of the code, where reference means that you can't change the value and it can't be nullptr.

I don't like to write
`(*this)[7]` or `!*this`
Lets make it
`this[7]` and `!this`

## It is requried to specify this
Simple as that, currently writing `this->x` is optional, we learned to put it into our code standards for a good reason. It is really useful to know, that we talk about x in the current class, and not a local or global variable.
