# About litesrpent
This is litesrpent, an open-source piece of software developed by me using Claude Code (Opus 4.6/4.7) to create a Common Lisp implementation.\
litesrpent is free to distribute, and has certain limitations currently as it is in beta (see below).
# Setup details (source code)
Before compilation you must place the files under "include" into "src" or many compilation errors will trigger.\
core.lisp is **not** meant to be compiled, it is loaded on running litesrpent.exe once litesrpent.c is compiled.\
Make sure to **keep** core.lisp in stdlib, otherwise litesrpent will be utterly useless.
# Setup details (precompiled)
Keep **every file** where it is to avoid errors.\
The /stdlib folder is only for core.lisp, it needs to load it and will otherwise break.
# Build requirements
GCC (C11+)\
Git (amalgamation)\
Resource Hacker (optional, but necessary for icon)\
Windows or Linux as OS
# Limitations in beta
**1.** No package definition. Will show unknown error if definition was attempted.\
**2.** No GUI. GUI modules exist, but syntax for declaration is undefined.\
**3.** Currently no file I/O; purely for testing and to gain popularity. Normal I/O works (like looping and finish-output).
# Strong capabilities
**1.** Loops. litesrpent has very, very fast loops (tested)\
**2.** Processing. litesrpent can process huge quantities of code very quickly.\
**3.** File size (primary executable). only 1.2 MB with 400 KB icon linking plus multiple other powerful features.
# About modifications to core.lisp
core.lisp **can** be modified, if so please make a GitHub repo where it can be downloaded as an extension to the litesrpent stdlibs!
# Limits on Linux/Unix
**1.** Precompiled source code is only distributed as a *Windows* executable, source code must be recompiled on Linux/Unix environments to run.\
**2.** No icons on Linux/Unix environments as the logo is a .ico file, compatible primarly with Windows, requires converter for Linux/Unix environments.\
**3.** The terminal is Windows, using Linux executables for it would result in a different interface.
# The good part of litesrpent
**Completely open source software that is free to distribute and develop.**

