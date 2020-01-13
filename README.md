# Re-Kilo

Re-Kilo is my personal implementation of the Kilo text editor. I have always enjoyed the feel of using a terminal based editor and so I wanted to learn more about how they operate. I recently finished the tutorial and am planning to return to the project in the future to add vim like functionality and refactor the code to make use of immutability which should result in more extensible and maintainable code.

The tutorial for the text editor can be found [here](https://viewsourcecode.org/snaptoken/kilo/02.enteringRawMode.html) and was created by the fictional programmer named Pailey.

## Installation

1. Clone the repository
2. Change working directory `cd kilo`
3. Compile `make`
4. Run `./kilo example.c` or `./kilo` (to create a new file)

## Features
- Save Functionality (CTRL-S)
- Search Functionality (CTRL-F)
- Status Bar
- Syntax Highlighting in C and C++
- Extensible syntax highlighting profiles
- Scrolling capability
- Displays special characters
