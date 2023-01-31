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

Finally, if an Aocla word is a symbol starting with the `$` character and a single additional character, the object stored at the specified variable is pushed on the stack. So the program to square 5 we wrote earlier can be rewritten as:

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

*Note: cat also works with strings, tuples, symbols.*

There is, of course, map:

    aocla> [1 2 3] [dup *] map
    [1 4 9]

If you want to do something with list elements, in an imperative way, you can use foreach:

    aocla> [1 2 3] [printnl] foreach
    1
    2
    3

There are a few more list procedures. `get@` to get a specific element in
a given position, `sort`, to sort a list, and if I remember correctly nothing
more about lists. Many of the above procedures are implemented inside the
C source code of Aocla, in Aocla language itself. Others are implemented
in C because of performance concerns or because it was simpler to do so.
For instance, this is the implementation of `foreach`:

    [(l f) // list and function to call with each element.
        $l len (e)  // Get list len in "e"
        0 (j)       // j is our current index
        [$j $e <] [
            $l $j get@  // Get list[j]
            $f upeval   // We want to evaluate in the context of the caller
            $j 1 + (j)  // Go to the next index
        ] while
    ] 'foreach def

As you can see from the above code, Aocla syntax also supports comments:
anything from `//` to the end of the line is ignored.

## Conditionals

Aocla conditionals are just `if` and `ifelse`. There is also a
quite imperative looping construct, that is `while`. You could loop
in the Scheme way, using recursion, but I like to give the language
a Common Lisp vibe, where you can write imperative code, too.

The words `if` and `ifelse` do what you could imagine:

    aocla> 5 (a)
    5
    aocla> [$a 2 >] ["a is > 2" printnl] if
    a is > 2

So `if` takes two programs (two lists), one is evaluated to see if it is
true or false. The other is executed only if the first program is true.

The same is true for ifelse, but it takes three programs: condition, true-program, false-program:

    aocla> 9 (a)
    aocla> [$a 11 ==] ["11 reached" printnl] [$a 1 + (a)] ifelse
    aocla> [$a 11 ==] ["11 reached" printnl] [$a 1 + (a)] ifelse
    aocla> [$a 11 ==] ["11 reached" printnl] [$a 1 + (a)] ifelse
    11 reached

And finally, an example of while:

    aocla> 10 [dup 0 >] [dup printnl 1 -] while
    10
    9
    8
    7
    6
    5
    4
    3
    2
    1

Or, for a longer but more usual program making use of Aocla locals:

    aocla> 10 (x) [$x 0 >] [$x printnl $x 1 - (x)] while
    10
    9
    8
    7
    6
    5
    4
    3
    2
    1

Basically two programming styles are possible: one that uses the stack
mainly in order to pass state from different procedures, and otherwise
uses locals a lot for local state, and another one where almost everything
will use the stack, like in FORTH, and locals will be used only from time
to time when stack manipulation is less clear. For instance Imagine
I've three values on the stack:

    aocla> 1 2 3
    1 2 3

If I want to sum the first and the third, and leave the second one
on the stack, even in a programming style where the code mainly uses
the stack to hold state, one could write:

    aocla> (a _ b) $_ $a $b +
    2 4 

## Evaluating lists

Words like `map` or `foreach` are written in Aocla itself. They are not 
implemented in C, even if they could and probably should for performance
reasons (and this is why `while` is implemented in C).

In order to implement procedures that execute code, Aocla provides the
`eval` built-in word. It just consumes the list on the top of the
stack and evaluates it.

    aocla> 5 [dup dup dup] eval
    5 5 5 5

In the above example we executed the list containing the program that calls
`dup` three times. Let's write a better example, a procedure that executes
the same code a specified number of times:

    [(n l)
        [$n 0 >]
        [$l eval $n 1 - (n)]
        while
    ] 'repeat def

Example usage:

    aocla> 3 ["Hello!" printnl] repeat
    Hello!
    Hello!
    Hello!

## Eval and local variables

There is a problem with the above implementation of `repeat`, it does
not mix well with local variables:

    aocla> 10 (x) 3 [$x printnl] repeat
    Unbound local var: '$x' in eval:0  in unknown:0

Here the problem is that once we call a new procedure, that is `repeat`,
the local variable `x` no longer exist in the context of the called
procedure. So when `repeat` evaluates our program we get an error.
This is the only case where Aocla local variables make the semantics of
Aocla more complex than other stack based languages without this feature.
In order to solve the problem above, Aocla has a specialized form of
`eval` that is called `upeval`: it executes a program in the context
(stack frame, in low level terms) of the caller. Let's rewrite
the `repeat` procedure using `upeval`:

    [(n l)
        [$n 0 >]
        [$l upeval $n 1 - (n)]
        while
    ] 'repeat def

After the change, it works as expected:

    aocla> 10 (x) 3 [$x printnl] repeat
    10
    10
    10

Now, out of the blue, without even knowing how Aocla is implemented,
let's check the C implementation of `uplevel`:

    /* Like eval, but the code is evaluated in the stack frame of the calling
     * procedure, if any. */
    int procUpeval(aoclactx *ctx) {
        if (checkStackType(ctx,1,OBJ_TYPE_LIST)) return 1;
        obj *l = stackPop(ctx);
        stackframe *saved = NULL;
        if (ctx->frame->prev) {
            saved = ctx->frame;
            ctx->frame = ctx->frame->prev;
        }
        int retval = eval(ctx,l);
        if (saved) ctx->frame = saved;
        release(l);
        return retval;
    }

What happens here is quite clear: we check to see if the stack contains
a list, as top level element. If so, we capture that value in the variable
`l`, then save the current stack frame, that contains our local variables
for the current procedure, and substitute it with the *previous procedure*
stack frame. Now we can call `eval()` and finally restore the original
stack frame.

## Creating programs at runtime

Aocla is homoiconic, as we already said earlier. Programs are
represented with the same data structures that Aocla code can manipulate.
Because of that, we can write programs writing programs. For instance let's
create a program that creates a procedure incrementing a variable of
the specified name.

The procedure exects two elements on the stack: the name of the procedure
we want to create, and the variable name that the procedure will increment:

    proc-name, var-name

And here is the program to do this:

    [ (p v) // Procedure, var.
        []                      // Accumulate our program into an empty list
        '$ $v cat swap ->       // Push $<varname> into the stack
        1 swap ->               // Push 1
        '+ swap ->              // Call +
        $v [] -> make-tuple swap -> // Capture back value into <varname>
        [] ->                       // Put all into a nested list
        'upeval swap ->             // Call upeval against the program
        $p def // Create the procedure  // Bind to the specified proc name
    ] 'create-incrementing-proc def

Basically calling `create-incrementing-proc` will end generating
a list like that (you can check the intermediate results by adding
`showstack` calls in your programs):

    [[$x 1 + (x)] upeval]

And finally the list is bound to the specified symbol using `def`.

Certain times programs that write programs can be quite useful. They are a
central feature in many Lisp dialects. However in the specific case of
Aocla different procedures can be composed via the stack, and we also
have `uplevel`, so I feel their usefulness is greatly reduced. Also note
that if Aocla was a serious language, it would have a lot more constructs
to making writing programs that write programs a lot simpler than the above. Anyway, as you saw earlier, when we implemented the `repeat` procedure, in Aocla
you can already do interesting stuff without using this programming
paradigm.

Ok, I think that's enough. We saw the basic of stack languages, the specific
stuff Aocla adds and how the language feels like. This isn't a course
on stack languages, nor I would be the best person to talk about the
argument. This is a course on how to write a small interpreter in C, so
let's dive into the Aocla interpreter internals.

# Aocla internals

At the start of this README I told you Aocla started from an Advent of
Code puzzles. The Puzzle could be solved by parsing representations
of lists like that, and then writing a comparison function for
the representations of the lists (well, actually this is how I solved it,
but one could even take the approach of comparing *while* parsing,
probably). This is an example of such lists:

    [1,[2,[3,[4,[5,6,7]]]],8,9]

Parsing such lists representations was not too hard, however this is
not single-level object, as it has elements that are sub lists. So
a recursive parser was the most obvious solution. This is what I wrote
back then, the 13th of December:

    /* This describes our elf object type. It can be used to represent
     * nested lists of lists and/or integers. */
    #define ELFOBJ_TYPE_INT  0
    #define ELFOBJ_TYPE_LIST 1
    typedef struct elfobj {
	int type;       /* ELFOBJ_TYPE_... */
	union {
	    int i;      /* Integer value. */
	    struct {    /* List value. */
		struct elfobj **ele;
		size_t len;
	    } l;
	} val;
    } elfobj;

Why `elfobj`? Well, because it was Christmas and AoC is about elves.
The structure above is quite trivial, just two types and a union in order
to represent both types.

Let's see the parser:

    /* Given the string 's' return the elfobj representing the list or
     * NULL on syntax error. '*next' is set to the next byte to parse, after
     * the current value was completely parsed. */
    elfobj *parseList(const char *s, const char **next) {
	elfobj *obj = elfalloc(sizeof(*obj));
	while(isspace(s[0])) s++;
	if (s[0] == '-' || isdigit(s[0])) {
	    char buf[64];
	    size_t len = 0;
	    while((*s == '-' || isdigit(*s)) && len < sizeof(buf)-1)
		buf[len++] = *s++;
	    buf[len] = 0;
	    obj->type = ELFOBJ_TYPE_INT;
	    obj->val.i = atoi(buf);
	    if (next) *next = s;
	    return obj;
	} else if (s[0] == '[') {
	    obj->type = ELFOBJ_TYPE_LIST;
	    obj->val.l.len = 0;
	    obj->val.l.ele = NULL;
	    s++;
	    /* Parse comma separated elements. */
	    while(1) {
		/* The list may be empty, so we need to parse for "]"
		 * ASAP. */
		while(isspace(s[0])) s++;
		if (s[0] == ']') {
		    if (next) *next = s+1;
		    return obj;
		}

		/* Parse the current sub-element recursively. */
		const char *nextptr;
		elfobj *element = parseList(s,&nextptr);
		if (element == NULL) {
		    freeElfObj(obj);
		    return NULL;
		}
		obj->val.l.ele = elfrealloc(obj->val.l.ele,
					    sizeof(elfobj*)*(obj->val.l.len+1));
		obj->val.l.ele[obj->val.l.len++] = element;
		s = nextptr; /* Continue from first byte not parsed. */

		while(isspace(s[0])) s++;
		if (s[0] == ']') continue; /* Will be handled by the loop. */
		if (s[0] == ',') {
		    s++;
		    continue; /* Parse next element. */
		}

		/* Syntax error. */
		freeElfObj(obj);
		return NULL;
	    }
	    /* Syntax error (list not closed). */
	    freeElfObj(obj);
	    return NULL;
	} else {
	    /* In a serious program you don't printf() in the middle of
	     * a function. Just return NULL. */
	    fprintf(stderr,"Syntax error parsing '%s'\n", s);
	    return NULL;
	}
	return obj;
    }

OK, what are the important parts of the above code? First: the parser is,
as I already said, recursive. To parse each element of the list we call
the same function again and again. This will make the magic of handling
any complex nested list without having to do anything special. I know, I know.
This is quite obvious for experienced enough programmers, but I claim it
is still kinda of magic, like a Mandelbrot set, like standing with a mirror
in front of another mirror admiring the infinite repeating images one
inside the other. Recursion remains magic even when it was understood.

--- work in progress ---


