Aocla (Advent of Code inspired Language) is a toy stack-based programming
language written as an extension of [day 13 Advent of Code 2022 puzzle](https://adventofcode.com/2022/day/13).
After completing the coding exercise, I saw other solutions resorting to `eval` and thought they were missing the point. The puzzle seemed more hinted at writing parsers for nested objects.

Now, a nice fact about parsers of lists with integers and nested
lists is that they are dangerously near, if written in the proper way, to become interpreters of Lisp-alike or FORTH-alike toy programming languages.

The gentle reader should be aware that I've a soft spot for [little languages](http://oldblog.antirez.com/page/picol.html). However, Picol was too much of a toy, while [Jim](http://jim.tcl.tk/index.html/doc/www/www/index.html) was too big as a coding example. I also like writing small programs that serve as [examples](https://github.com/antirez/kilo) of how you could design bigger programs, while retaining a manageable size. Don't took me wrong: it's not like I believe my code should be taken as an example, it's just that I learned a lot from such small programs, so, from time to time, I like writing new ones and sharing them. This time I wanted to obtain something of roughly the size of the Kilo editor, that is around ~1000 lines of code, showing the real world challenges arising when writing an actual interpreter for a programming language more complex than Picol. That's the result.

This README will first explain the language briefly. Later we will talk extensively about the implementation and its design. Without counting comments, the Aocla implementation is less than 1000 lines of code, and the core itself is around 500 lines (the rest of the code is the library implementation, the REPL, and so forth): hopefully, you will find the code easy to follow even if you are not used to C and to writing interpreters. I tried to keep stuff simple, as I always do when I write code, for myself and the others having the misfortune of modifying them.

Not every feature I desired to have is implemented, and certain data types, like the string type, lack any useful procedure to work with them. This choice was made in order to avoid making the source code more complex than needed, and also, on my side, to avoid writing too much useless code, given that this language will never be used to write actual code. Besides, implementing some of the missing parts is a good exercise for the willing reader, assuming she or he are new to this kind of stuff.

# Aocla

Aocla is a very simple language, more similar to Joy than to FORTH (higher level). It has a total of six datatypes:

* Lists: `[1 2 3 "foo"]`
* Symbols: `mysymbol`, `==` or `$x`
* Integers: `500`
* Booleans: `#t` or `#f`
* Tuples: `(x y z)`
* Strings: `"Hello World!\n"`

Floating point numbers are not provided for simplicity (writing an implementation should not be too hard, and is a good exercise). Aocla programs are valid Aocla lists, so the language is [homoiconic](https://en.wikipedia.org/wiki/Homoiconicity). While Aocla is a stack-based language, like FORTH, Joy and Factor, it introduces the idea of *local variables capturing*. Because of this construct, Aocla programs look a bit different (and simpler to write and understand in my opinion) compared to other stack-based languages. However locals capturing is optional: any program using locals can be rewritten to avoid using them.

## Our first program

The following is a valid Aocla program, taking 5 and squaring it, to obtain 25.

    [5 dup *]

Since all the programs must be lists, and thus are enclosed between `[` and `]`, both the Aocla CLI (Command Line Interface) and the execution of programs from files are designed to avoid needing the brackets. Aocla will put the program inside `[]` for you, so the above program should be written like that:

    5 dup *

Programs are executed from left to right, *word by word*. If a word is not a symbol nor a tuple, its execution results into pushing its value on the stack. Symbols will produce a procedure call: the symbol name will be looked up in the table of procedures, and if a procedure with a matching name is found, it gets called. So the above program will perform the following steps:

* `5`: the value 5 is pushed on the stack. The stack will contain `(5)`.
* `dup`: is a symbol. A procedure called `dup` is looked up and executed. What `dup` does is to take the top value on the stack and duplicate it, so now the stack will contain `(5 5)`.
* `*`: is another symbol. The procedure is called. It will take the last two elements on the stack, check if they are integers, multiply them together and push the result on the stack. Now the stack will contain `(25)`.

If an Aocla word is a tuple, like `(x y)`, its execution has the effect of removing a corresponding number of elements from the stack and binding them to the local variables having the specified names:

    10 20 (x y)

After the above program is executed, the stack will be empty and the local variables x and y will contain 10 and 20.

Finally, if an Aocla word is a symbol starting with the `$` character and a single additional character, the object stored at the specfied variable is pushed on the stack. So the program to square 5 we wrote earlier can be rewritten as:

    5 (x) $x $x *

The ability to capture stack values into locals allow to make complex stack manipulation in a simple way, and make programs more explicit to read and easier to write. Still they have the remarkably quality of not making the language semantically more complex (if not for a small thing we will cover later -- search `upeval` inside this document if you want to know ASAP, but if you know the Tcl programming language, you already understood from the name). In general, while locals help the handling of the stack in the local context of the procedure, words communicate via the stack, so the main advantages of stack-based languages are untouched.

*Note: why allowing locals with just single letter names? The only reason is to make the implementation of the Aocla interpreter simpler to understand. This way, we don't need to make use of any dictionary data structure. If I would design Aocla to be a real language, I would remove this limitation.*

We said that symbols normally trigger a procedure call. But symbols can also be pushed on the stack like any other value. To do so, symbols must be quoted, with the `'` character at the start.

    'Hello printnl

The `printnl` procedure prints the last element in the stack and also prints a newline character, so the above program will just print `Hello` on the screen. For now you may wonder what's the point of quoting symbols: you could just use strings, but later we'll see this is important in order to write Aocla programs that write Aocla programs.

Quoting also works with tuples, so if you want to push the tuple `(a b c)` on the stack, instead of capturing the variables a, b and c, you can write:

    '(a b c) printnl

## Inspecting the stack content

When you start the Aocla interpreter without a file name, it gets executed
in REPL mode (Read Eval Print Loop). You write a code fragment, press enter, the code gets executed and the current state of the stack is shown:

    aocla> 1
    1
    aocla> 2
    1 2
    aocla> ['a 'b "foo"]
    1 2 [a b "foo"]

This way you always know the stack content.
When you execute programs from files, in order to debug their executions you can print the stack content using the `showstack` procedure.

## User defined procedures

Aocla programs are just lists, and Aocla functions are lists bound to a
name. The name is given as a symbol, and the way to bind a list with a
symbol is an Aocla procedure itself, and not special syntax:

    [dup *] 'square def

The `def` procedure will bind the list `[dup *] to the `square` symbol,
so later we can use the `square` symbol and it will call our procedure:

    aocla> 5 square
    25

Calling a symbol (not quoted symbols are called by default) that is not
bound to any program will produce an error:

    aocla> foobar
    Symbol not bound to procedure: 'foobar' in unknown:0

## Working with lists

Lists are the central data structure of the language: they are used to represent programs and are useful as a general purpose data structure to represent data. So most of the very few built-in procedures that Aocla offers are lists manipulation procedures.

Showing by examples, via the REPL, is probably the simplest way to show how to write Aocla programs. This pushes an empty list on the stack:

    aocla> []
    []

We can add elements to the tail or head of the list, using the `<-` and `->` procedures:

aocla> 1 swap ->
[1] 
aocla> 2 swap ->
[1 2] 

Note that these procedures are designed to insert the last element in the
stack into the list that is the penultimate element in the stack, so,
in this specific case, we have to swap the order of the last two elements
on the stack before calling `->`. It is possible to design these procedures
in a different way, that is: to the expect `list, element` on the stack instead
of `element, list`. There is no clear winner: one or the other approach is
better or worse depending on the use case. In Aocla, local variables make
all this less important compared to other stack based languages. It is always
possible to make things more explicit, like in the following example:

    aocla> [1 2 3] 
    [1 2 3] 
    aocla> (l) 4 $l ->
    [1 2 3 4] 
    aocla> (l) 5 $l ->
    [1 2 3 4 5] 

Then, to know how many elements there are in the list, we can use the
`len` procedure, that also works for other data types:

    aocla> ['a 'b 1 2] 
    [a b 1 2] 
    aocla> len
    4 
    aocla> "foo"
    4 "foo" 
    aocla> len
    4 3 

Other useful list operations are the following, that you may find quite
obvious if you have any Lisp background:

    aocla> [1 2 3] [4 5 6] cat
    [1 2 3 4 5 6]
    aocla> [1 2 3] first
    1
    aocla> [1 2 3] rest
    [2 3]

There is, of course, map:

    aocla> [1 2 3] [dup *] map
    [1 4 9]

If you want to just do something with list elements, in an imperative way, you can use foreach:

    aocla> [1 2 3] [printnl] foreach
    1
    2
    3

And a few more, like `get@` to get a specific element in a given
position, `sort`, to sort a list, and if I remember correctly nothing
more about lists. And many of the above things are implemented inside the
C source code of Aocla in Aocla language itself. This is, for instance,
the implementation of `map`:

    [(a b) // Concat list a and b.
        $b [$a -> (a)] foreach // for each element of b, append to a.
        $a // return a
    ] 'cat def

## Conditionals



## Evaluating lists

## Eval and local variables

Give a look at the quite imperative implementation of `map` inside Aocla:

    [(l f) // list and function to apply
        $l len (e)  // Get list len in "e"
        0 (j)       // j is our current index
        []          // We will populate this empty list
        [$j $e <] [
            $l $j get@
            $f upeval
            swap ->
            $j 1 + (j)
        ] while
    ] 'map def

## Creating programs at runtime

# Aocla internals
