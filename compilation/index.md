# Compilation related topics

- [No includes](#no-includes)
- [The current compilation model](#the-current-compilation-model)
- [The proposed changes](#the-proposed-changes)
- [Template resolutions](#template-resolution)
- [Continous compiler presence](#continous-compiler-presence)
- [Conclusion](#conclusion)

## No includes
The main idea is simple, you should be just able to completely remove #include from the language without any replacement, including forward declarations.
When you want to start to use some symbol from the project you just start using it, it is there all the time, it doesn't matter if it is defined in cpp, or hpp (which would be optional now), or in the same file later on, all symbols are available all the time.
We wouldn't have include path, but something very similiar, we could call it something like "scope-expansion-path", which would basically define all the files that are part of your code.
This implies, that it would no longer be possible to create two duplicate symbols on different compilation units, which is considered an advantage, as it forces better symbol scoping.

The real goal is to actually improve compilation time, not make it worse, so core changes to the way compilation is done would have to be done, mainly we need to make sure that every source file is opened and compiled only once.

Compared to the current model, when the string.hpp file (for example) can be parsed thousands of times in a project, as it is basically parsed for every compilation unit. This somewhat relates to the modules part of C++ standard.

The compiler needs to be able to detect and error on the circular symbol dependency.

Implications:
1. Order of definitions doesn't matter at all, yet runtime static variables assignment should still respect the order in the given file.
2. Macros could still exist, but there would have to be some changes. We would probably have 2 types of macros: A) global macro, this would apply to macros specified on the commandline of the compiler and also in the code, these macros would be visible in the whole project regardles on where defined, this macro can't be removed by #undef B) local macro, specified in the context of one file for some macro magic, visible only in the one file, and could be #undef.

# The current compilation model
This is based on my basic understanding of how the compiler works, let me know if I'm wrong.

The current compilation method is problematic:
1. The preprocessor (unaware of how the language works), blindly pastes all the include files to create (typically huge) cpp file for the compiler. It is typically huge once you include any of the typical things from the std, like string, as it cascades into a lot of other includes, my measures were hundrets of thousands of lines.
2. At this point, vast, vast majority of the code to be compiled are duplications from the libraries. The compiler has to take each of the files, and compile it independently. At this step, we can take advantage of threading, but it is a big task anyway, here we create the obj files.
3. All the obj files are processed, and the data deduplicated to create the final code structure and the resulting binaries.

This is partially solved by unity builds (merging several cpp files together to reduce duplication), and by precompiled headers, but it is just a bandaid, and we are still typically left with very cpu-demanding compilation process.
Another problem of this approach is, that changing anything in include files used in a lot of places results in recompilation of big part of the project.

# The proposed changes
But now, when the files are no longer considered to be just text files to be blindly included, we can do better.
Instead of the compiler having independent compilation forks, the compiler would be one multithreaded program where the threads tightly cooperate.

Let me try to do high-level proposal of how the full recompilation could work:

1. The individual threads of the compilers would just start opening all the relevant files, facing a lot of code that uses unknown symbols.
   In this phase, the compiler would create just an internal tree-like structure of what is what, so if it encounters function like this, without knowing what is A or B, it can still add it to the structure, with B and A as unresolved type references and a.b as unresloved member reference etc, but we would already have indexed, that function foo exists and what is the code.

```
B foo(A a)
{
  return a.b;
};
```

2. Now we know that all symbols are indexed, and we can try to match all of the unresolved type references.
3. We know the sizes of the objects, and we have fully specified tree of all the symbols, so we can optimise the code and finish the compilation process by generating machine code.

All of the threads performing steps 1 + 2 + 3 have to work on shared data, but we live in a modern days where things like this are not really a problem.

# Templates resolution
In the phase 1, templates are indexed as other symbols, just with generic types.

But then, we will have to resolve the template type resolution requests and we need to make sure, that each of the templated types and code is generated exactly once. This means, that if some of the threads makes a request for vector&lt;int&gt; to be registered as type, this can happen:
1. It is already indexed, so we just reference it.
2. Some other thread is generating it, we wait until it is finished.
3. We generate the code. (This could obviously trigger a lot of cascading code generation requests, and it is improtant to allow more workers to participate on the tasks)

This could mean that a lot of threads could wait for some of the popular templates to be done, but the worst case of all of the threads waiting for one template to be done is at most as bad as all of the threads duplicitly compiling the same template on its own, and then adding work to the linker to deduplicate it.

# Continous compiler presence
The next important change is, that for the contiuous programmer work, we wouldn't use the compiler just as an external tool to process our textual files, but the compiler process would keep running, and it would keep its binary representation of the code in memory waiting for:
1. Another recompilation request, where it could precisely figure out what needs to be recompiled
2. IDE API requests the programmer needs to navigate the code
3. IDE API requests to apply the changes in the compiler on the fly, so it is as much up-to-date as possible.

# Conclusion
The expected results should be:

1. Dramatical compile time reduction (as well as disk space reduction)
2. No linking
3. No includes
4. Deduplication of the code parsing (compiler and ide)
