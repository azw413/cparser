Functional-C - A C dialect supporting modern functional programming 
===================================================================

Introduction
------------

C is a fantastically powerful and yet simple language hence it's popularity. It
is showing it's age a little now though because of the esoteric way of doing 
things and lacks features such as first class functions and closures which 
would allow a much more functional style of programming. Sure you could do this 
with function pointers but the types for this would be prohibitvely complicated
and ugly. This is a PoC to take cparser and add :- 

1. Type inferencing using a new 'let' keyword. This is an important building
   block because it will remove the type ugliness. 
   
2. A new 'function' keyword to allow defining anonymous functions as an expression.

3. Closures will capture local state at the point of function definition. 

4. Maybe an enhanced macro system to allow more features to be added. 

Functional-C will act as a pre-processor and parse functional-c files (.fc) and 
emit regular ANSI C files (.c) which can be built with any compiler on any 
system. This way functional-c will be entirely portable to virtually any platform.

It's based on cparser - see below.




cparser - A C99 parser (with GNU extensions)
============================================

Introduction
------------

cparser is a recursive descent C99 parser written in C99.  It contains a
preprocessor, lexer, parser, constructs an AST and does semantic analysis.  It
acts as a frontend to the libFirm intermediate representation library.  This
way optimization and code generation is performed. The compiler supports cross
compilation to multiple target architectures with a command-line switch.  It
comes with driver logic for calling assemblers and linkers as well as parsing
command-line options.  This allows it to be a drop-in replacement for gcc or
clang in many situations.

Building and Installation
-------------------------

Requirements:

* A C99 compiler (gcc and icc are known to work).
* libFirm-1.22

### Building with make

Unpack libfirm in a directory called libfirm in the source directory
alternatively you may setup an alternate location with a 'config.mak' file.
Just type 'make' in the source directory. The results are put into a directory
called "build". You can override the existing preprocessor, compiler and linker
flags and built-in paths for include directories by creating a 'config.mak'
file.

### Building with cmake

cparser has an additional cmake build system. CMake is a complexer build system
than the make based build and most cparser developers do not use it.  However
it can adapt the compiler and linker flags to build shared libraries for a
wider range of systems, provides an installation target and is often more
familiar for people preparing packages for distribution.

### Notes for a cparser installation

While cparser often runs fine from the source/build directory, a proper
installation should be configured with correct system paths. For this a
`config.mak` file should be created and the following variables set apropriately
for the system: `PREFIX`, `SYSTEM_INCLUDE_DIR`, `LOCAL_INCLUDE_DIR`,
`COMPILER_INCLUDE_DIR`, `MULTILIB_M32_TRIPLE`, `MULTILIB_M64_TRIPLE`,
`variant=optimize`.
The variable may be defined empty; See `config.default.mak` and `Makefile` for
details.

Further Information and Contact
-------------------------------

Official website: http://libfirm.org/

Contact E-Mail: firm@ipd.info.uni-karlsruhe.de

Mailing list: https://lists.ira.uni-karlsruhe.de/mailman/listinfo/firm

Bugtracker: http://pp.ipd.kit.edu/~firm/bugs

Internet relay chat: irc://chat.freenode.net/#firm
