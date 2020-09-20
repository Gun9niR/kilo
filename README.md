# Kilo
Kilo is a trivial text editor that implements some fundamental features of a editor without dependencies on any external libs.

## Features
- Open & save (one file at a time)
- Text editing (of course lol)
- Scrolling (also pretty much obvious)
- Incremental searching
- Syntax highlighting

## Usage
Since the program uses <terminos.h> to interact with the terminal at a low level, it, Kilo can only be compiled and run in Linux environment.

To compile, run `make` in the terminal.

To fire up Kilo, run `./kilo (optional)"filename"`, for instance, `./kilo kilo.c`. You can create a new file by feeding no argument to it.

Note: Kilo does NOT support UTF-8, so don't type in Chinese.
## To-do list
- [x] Config file
- [x] Line numbers
- [x] Auto indent