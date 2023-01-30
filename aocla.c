#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>
#include <stdarg.h>

#define NOTUSED(V) ((void) V)

/* =========================== Data structures ============================== */

/* This describes our Aocla object type. It can be used to represent
 * lists (and code: they are the same type in Aocla), integers, strings
 * and so forth.
 *
 * Type are defined so that each type ID is a different set bit, this way
 * in checkStackType() we may ask the function to check if some argument
 * is one among a list of types just bitwise-oring the type IDs together. */
#define OBJ_TYPE_INT    (1<<0)
#define OBJ_TYPE_LIST   (1<<1)
#define OBJ_TYPE_TUPLE  (1<<2)
#define OBJ_TYPE_STRING (1<<3)
#define OBJ_TYPE_SYMBOL (1<<4)
#define OBJ_TYPE_BOOL   (1<<5)
#define OBJ_TYPE_ANY    INT_MAX /* All bits set. For checkStackType(). */
typedef struct obj {
    int type;       /* OBJ_TYPE_... */
    int refcount;   /* Reference count. */
    int line;       /* Source code line number where this was defined, or 0. */
    union {
        int i;      /* Integer. Literal: 1234 */
        int istrue; /* Boolean. */
        struct {    /* List or Tuple: Literal: [1 2 3 4] or (a b c) */
            struct obj **ele;
            size_t len;
            int quoted; /* Used for quoted tuples. Don't capture vars if true.
                           Just push the tuple on stack. */
        } l;
        struct {    /* Mutable string & unmutable symbol. */
            char *ptr;
            size_t len;
            int quoted; /* Used for quoted symbols: when quoted they are
                           not executed, but just pushed on the stack by
                           eval(). */
        } str;
    };
} obj;

/* Procedures. They are just lists with associated names. There are also
 * procedures implemented in C. In this case proc is NULL and cproc has
 * the value of the function pointer implementing the procedure. */
struct aoclactx;
typedef struct aproc {
    const char *name;
    obj *proc;      /* If not NULL it's an Aocla procedure (list object). */
    int (*cproc)(struct aoclactx *); /* C procedure. */
    struct aproc *next;
} aproc;

/* We have local vars, so we need a stack frame. We start with a top level
 * stack frame. Each time a procedure is called, we create a new stack frame
 * and free it once the procedure returns. */
#define AOCLA_NUMVARS 256
typedef struct stackframe {
    obj *locals[AOCLA_NUMVARS];/* Local var names are limited to a,b,c,...,z. */
    aproc *curproc;            /* Current procedure executing or NULL.  */
    int curline;               /* Current line number during execution. */
    struct stackframe *prev;   /* Upper level stack frame or NULL. */
} stackframe;

/* Interpreter state. */
#define ERRSTR_LEN 256
typedef struct aoclactx {
    size_t stacklen;        /* Stack current len. */
    obj **stack;
    aproc *proc;            /* Defined procedures. */
    stackframe *frame;      /* Stack frame with locals. */
    /* Syntax error context. */
    char errstr[ERRSTR_LEN]; /* Syntax error or execution error string. */
} aoclactx;

void setError(aoclactx *ctx, const char *ptr, const char *msg);
aproc *lookupProc(aoclactx *ctx, const char *name);
void loadLibrary(aoclactx *ctx);

/* ================================= Utils ================================== */

/* Life is too short to handle OOM. alloc() and realloc() that
 * abort on OOM. free() is the same, so no wrapper. */
void *myalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr,"Out of memory allocating %zu bytes\n", size);
        exit(1);
    }
    return p;
}

void *myrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr,size);
    if (!p) {
        fprintf(stderr,"Out of memory allocating %zu bytes\n", size);
        exit(1);
    }
    return p;
}

/* =============================== Objects ================================== */

/* Recursively free an Aocla object, if the refcount just dropped to zero. */
void release(obj *o) {
    if (o == NULL) return;
    assert(o->refcount >= 0);
    if (--o->refcount == 0) {
        switch(o->type) {
        case OBJ_TYPE_LIST:
        case OBJ_TYPE_TUPLE:
            for (size_t j = 0; j < o->l.len; j++)
                release(o->l.ele[j]);
            free(o->l.ele);
            break;
        case OBJ_TYPE_SYMBOL:
        case OBJ_TYPE_STRING:
            free(o->str.ptr);
            break;
        default:
            break;
            /* Nothing special to free. */
        }
        free(o);
    }
}

/* Increment the object ref count. Use when a new reference is created. */
void retain(obj *o) {
    o->refcount++;
}

/* Allocate a new object of type 'type. */
obj *newObject(int type) {
    obj *o = myalloc(sizeof(*o));
    o->refcount = 1;
    o->type = type;
    o->line = 0;
    return o;
}

/* Return true if the character 'c' is within the Aocla symbols charset. */
int issymbol(int c) {
    if (isalpha(c)) return 1;
    switch(c) {
    case '@':
    case '$':
    case '+':
    case '-':
    case '*':
    case '/':
    case '=':
    case '?':
    case '%':
    case '>':
    case '<':
    case '_':
    case '\'':
        return 1;
    default:
        return 0;
    }
}

/* Utility function for parseObject(). It just consumes spaces and comments
 * and return the new pointer after the consumed part of the string. */
const char *parserConsumeSpace(const char *s, int *line) {
    while(1) {
        while(isspace(s[0])) {
            if (s[0] == '\n' && line) (*line)++;
            s++;
        }
        if (s[0] != '/' || s[1] != '/') break; /* // style comments. */
        while(s[0] && s[0] != '\n') s++; /* Seek newline after comment. */
    }
    return s;
}

/* Given the string 's' return the obj representing the list or
 * NULL on syntax error. '*next' is set to the next byte to parse, after
 * the current e was completely parsed.
 * 
 * The 'ctx' argument is only used to set an error in the context in case
 * of parse error, it is possible to pass NULL.
 *
 * Returned object has a ref count of 1. */
obj *parseObject(aoclactx *ctx, const char *s, const char **next, int *line) {
    obj *o = newObject(-1);

    /* Consume empty space and comments. */
    s = parserConsumeSpace(s,line);
    if (line)
        o->line = *line; /* Set line number where this object is defined. */

    if ((s[0] == '-' && isdigit(s[1])) || isdigit(s[0])) { /* Integer. */
        char buf[64];
        size_t len = 0;
        while((*s == '-' || isdigit(*s)) && len < sizeof(buf)-1)
            buf[len++] = *s++;
        buf[len] = 0;
        o->type = OBJ_TYPE_INT;
        o->i = atoi(buf);
        if (next) *next = s;
    } else if (s[0] == '[' || /* List, tuple or quoted tuple. */
               s[0] == '(' ||
               (s[0] == '\'' && s[1] == '('))
    {
        if (s[0] == '\'') {
            o->l.quoted = 1;
            s++;
        } else {
            o->l.quoted = 0;
        }
        o->type = s[0] == '[' ? OBJ_TYPE_LIST : OBJ_TYPE_TUPLE;
        o->l.len = 0;
        o->l.ele = NULL;
        s++;
        /* Parse comma separated elements. */
        while(1) {
            /* The list may be empty, so we need to parse for "]"
             * ASAP. */
            s = parserConsumeSpace(s,line);
            if ((o->type == OBJ_TYPE_LIST && s[0] == ']') ||
                (o->type == OBJ_TYPE_TUPLE && s[0] == ')'))
            {
                if (next) *next = s+1;
                return o;
            }

            /* Parse the current sub-element recursively. */
            const char *nextptr;
            obj *element = parseObject(ctx,s,&nextptr,line);
            if (element == NULL) {
                release(o);
                return NULL;
            } else if (o->type == OBJ_TYPE_TUPLE &&
                       (element->type != OBJ_TYPE_SYMBOL ||
                        element->str.len != 1))
            {
                /* Tuples can be only composed of one character symbols. */
                release(element);
                release(o);
                setError(ctx,s,
                    "Tuples can only contain single character symbols");
                return NULL;
            }
            o->l.ele = myrealloc(o->l.ele, sizeof(obj*)*(o->l.len+1));
            o->l.ele[o->l.len++] = element;
            s = nextptr; /* Continue from first byte not parsed. */

            continue; /* Parse next element. */
        }
        /* Syntax error (list not closed). */
        setError(ctx,s,"List never closed");
        release(o);
        return NULL;
    } else if (issymbol(s[0])) {         /* Symbol. */
        o->type = OBJ_TYPE_SYMBOL;
        if (s[0] == '\'') {
            o->str.quoted = 1;
            s++;
        } else {
            o->str.quoted = 0;
        }
        const char *end = s;
        while(issymbol(*end)) end++;
        o->str.len = end-s;
        char *dest = myalloc(o->str.len+1);
        o->str.ptr = dest;
        memcpy(dest,s,o->str.len);
        dest[o->str.len] = 0;
        if (next) *next = end;
    } else if (s[0]=='#') {             /* Boolean. */
        if (s[1] != 't' && s[1] != 'f') {
            setError(ctx,s,"Booelans are either #t or #f");
            release(o);
            return NULL;
        }
        o->type = OBJ_TYPE_BOOL;
        o->istrue = s[1] == 't' ? 1 : 0;
        s += 2;
        if (next) *next = s;
    } else if (s[0] == '"') {           /* String. */
        s++; /* Skip " */
        o->type = OBJ_TYPE_STRING;
        o->str.ptr = myalloc(1); /* We need at least space for nullterm. */
        o->str.len = 0;
        while(s[0] && s[0] != '"') {
            int c = s[0];
            switch(c) {
            case '\\':
                s++;
                int q = s[0];
                switch(q) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = q; break;
                }
            default:
                break;
            }
            /* Here we abuse realloc() ability to overallocate for us
             * in order to avoid complexity. We allocate len+2 because we
             * need 1 byte for the current char, 1 for the nullterm. */
            o->str.ptr = myrealloc(o->str.ptr,o->str.len+2);
            o->str.ptr[o->str.len++] = c;
            s++;
        }
        if (s[0] != '"') {
            setError(ctx,s,"Quotation marks never closed in string");
            release(o);
            return NULL;
        }
        o->str.ptr[o->str.len] = 0; /* nullterm. */
        s++;
        if (next) *next = s;
    } else {
        /* Syntax error. */
        setError(ctx,s,"No object type starts like this");
        release(o);
        return NULL;
    }
    return o;
}

/* Compare the two objects 'a' and 'b' and return:
 * -1 if a<b; 0 if a==b; 1 if a>b. */
#define COMPARE_TYPE_MISMATCH INT_MIN
int compare(obj *a, obj *b) {
    /* Int VS Int */
    if (a->type == OBJ_TYPE_INT && b->type == OBJ_TYPE_INT) {
        if (a->i < b->i) return -1;
        else if (a->i > b->i) return 1;
        return 0;
    }

    /* Bool vs Bool. */
    if (a->type == OBJ_TYPE_BOOL && b->type == OBJ_TYPE_BOOL) {
        if (a->istrue < b->istrue) return -1;
        else if (a->istrue > b->istrue) return 1;
        return 0;
    }

    /* String|Symbol VS String|Symbol. */
    if ((a->type == OBJ_TYPE_STRING || a->type == OBJ_TYPE_SYMBOL) &&
        (b->type == OBJ_TYPE_STRING || b->type == OBJ_TYPE_SYMBOL))
    {
        int cmp = strcmp(a->str.ptr,b->str.ptr);
        /* Normalize. */
        if (cmp < 0) return -1;
        if (cmp > 0) return 1;
        return 0;
    }

    /* List|Tuple vs List|Tuple. */
    if ((a->type == OBJ_TYPE_LIST || a->type == OBJ_TYPE_TUPLE) &&
        (b->type == OBJ_TYPE_LIST || b->type == OBJ_TYPE_TUPLE))
    {
        /* Len wins. */
        if (a->l.len < b->l.len) return -1;
        else if (a->l.len > b->l.len) return 1;
        return 0;
    }

    /* Comparison impossible. */
    return COMPARE_TYPE_MISMATCH;
}

/* qsort() helper to sort arrays of obj pointers. */
int qsort_obj_cmp(const void *a, const void *b) {
    obj **obja = (obj**)a, **objb = (obj**)b;
    return compare(obja[0],objb[0]);
}

/* Output an object human readable representation .*/
#define PRINT_RAW 0             /* Nothing special. */
#define PRINT_COLOR (1<<0)      /* Colorized by type. */
#define PRINT_REPR (1<<1)       /* Print in Aocla literal form. */
void printobj(obj *obj, int flags) {
    const char *escape;
    int color = flags & PRINT_COLOR;
    int repr = flags & PRINT_REPR;

    if (color) {
        switch(obj->type) {
        case OBJ_TYPE_LIST: escape = "\033[33;1m"; break;       /* Yellow. */
        case OBJ_TYPE_TUPLE: escape = "\033[34;1m"; break;      /* Blue. */
        case OBJ_TYPE_SYMBOL: escape = "\033[36;1m"; break;     /* Cyan. */
        case OBJ_TYPE_STRING: escape = "\033[32;1m"; break;     /* Green. */
        case OBJ_TYPE_INT: escape = "\033[37;1m"; break;        /* Gray. */
        case OBJ_TYPE_BOOL: escape = "\033[35;1m"; break;       /* Gray. */
        }
        printf("%s",escape); /* Set color. */
    }

    switch(obj->type) {
    case OBJ_TYPE_INT:
        printf("%d",obj->i);
        break;
    case OBJ_TYPE_SYMBOL:
        printf("%s",obj->str.ptr);
        break;
    case OBJ_TYPE_STRING:
        if (!repr) {
            fwrite(obj->str.ptr,obj->str.len,1,stdout);
        } else {
            printf("\"");
            for (size_t j = 0; j < obj->str.len; j++) {
                int c = obj->str.ptr[j];
                switch(c) {
                case '\n': printf("\\n"); break;
                case '\r': printf("\\r"); break;
                case '\t': printf("\\t"); break;
                case '"': printf("\\\""); break;
                default: printf("%c", c); break;
                }
            }
            printf("\"");
        }
        break;
    case OBJ_TYPE_BOOL:
        printf("#%c",obj->istrue ? 't' : 'f');
        break;
    case OBJ_TYPE_LIST:
    case OBJ_TYPE_TUPLE:
        if (repr) printf("%c",obj->type == OBJ_TYPE_LIST ? '[' : '(');
        for (size_t j = 0; j < obj->l.len; j++) {
            printobj(obj->l.ele[j],flags);
            if (j != obj->l.len-1) printf(", ");
        }
        if (color) printf("%s",escape); /* Restore upper level color. */
        if (repr) printf("%c",obj->type == OBJ_TYPE_LIST ? ']' : ')');
        break;
    }
    if (color) printf("\033[0m"); /* Color off. */
}

/* Allocate an int object with value 'i'. */
obj *newInt(int i) {
    obj *o = newObject(OBJ_TYPE_INT);
    o->i = i;
    return o;
}

/* Allocate a boolean object with value 'b' (1 true, 0 false). */
obj *newBool(int b) {
    obj *o = newObject(OBJ_TYPE_BOOL);
    o->istrue = b;
    return o;
}

/* Allocate a string object initialized with the content at 's' for
 * 'len' bytes. */
obj *newString(const char *s, size_t len) {
    obj *o = newObject(OBJ_TYPE_STRING);
    o->str.len = len;
    o->str.ptr = myalloc(len+1);
    memcpy(o->str.ptr,s,len);
    o->str.ptr[len] = 0;
    return o;
}

/* Deep copy the passed object. Return an object with refcount = 1. */
obj *deepCopy(obj *o) {
    if (o == NULL) return NULL;
    obj *c = newObject(o->type);
    switch(o->type) {
    case OBJ_TYPE_INT: c->i = o->i; break;
    case OBJ_TYPE_BOOL: c->istrue = o->istrue; break;
    case OBJ_TYPE_LIST:
    case OBJ_TYPE_TUPLE:
        c->l.len = o->l.len;
        c->l.ele = myalloc(sizeof(obj*)*o->l.len);
        for (size_t j = 0; j < o->l.len; j++)
            c->l.ele[j] = deepCopy(o->l.ele[j]);
        break;
    case OBJ_TYPE_STRING:
    case OBJ_TYPE_SYMBOL:
        c->str.len = o->str.len;
        c->str.quoted = o->str.quoted; /* Only useful for symbols. */
        c->str.ptr = myalloc(o->str.len+1);
        memcpy(c->str.ptr,o->str.ptr,o->str.len+1);
        break;
    }
    return c;
}

/* This function performs a deep copy of the object if it has a refcount > 1.
 * The copy is returned. Otherwise if refcount is 1, the function returns
 * the same object we passed as argument. This is useful when we want to
 * modify a shared object. */
obj *getUnsharedObject(obj *o) {
    if (o->refcount > 1) {
        return deepCopy(o);
    } else {
        return o;
    }
}

/* ========================== Interpreter state ============================= */

/* Set the syntax or runtime error, if the context is not NULL. */
void setError(aoclactx *ctx, const char *ptr, const char *msg) {
    if (!ctx) return;
    if (!ptr) ptr = ctx->frame->curproc ?
                    ctx->frame->curproc->name : "unknown context";
    size_t len =
        snprintf(ctx->errstr,ERRSTR_LEN,"%s: '%.30s%s'",
            msg,ptr,strlen(ptr)>30 ? "..." :"");

    stackframe *sf = ctx->frame;
    while(sf && len < ERRSTR_LEN) {
        len += snprintf(ctx->errstr+len,ERRSTR_LEN-len," in %s:%d ",
            sf->curproc ? sf->curproc->name : "unknown",
            sf->curline);
        sf = sf->prev;
    }
}

/* Create a new stack frame. */
stackframe *newStackFrame(aoclactx *ctx) {
    stackframe *sf = myalloc(sizeof(*sf));
    memset(sf->locals,0,sizeof(sf->locals));
    sf->curproc = NULL;
    sf->prev = ctx ? ctx->frame : NULL;
    return sf;
}

/* Free a stack frame. */
void freeStackFrame(stackframe *sf) {
    for (int j = 0; j < AOCLA_NUMVARS; j++) release(sf->locals[j]);
    free(sf);
}

aoclactx *newInterpreter(void) {
    aoclactx *i = myalloc(sizeof(*i));
    i->stacklen = 0;
    i->stack = NULL; /* Will be allocated on push of new elements. */
    i->proc = NULL; /* That's a linked list. Starts empty. */
    i->frame = newStackFrame(NULL);
    loadLibrary(i);
    return i;
}

/* Push an object on the interpreter stack. No refcount change. */
void stackPush(aoclactx *ctx, obj *o) {
    ctx->stack = myrealloc(ctx->stack,sizeof(obj*) * (ctx->stacklen+1));
    ctx->stack[ctx->stacklen++] = o;
}

/* Pop an object from the stack without modifying its refcount.
 * Return NULL if stack is empty. */
obj *stackPop(aoclactx *ctx) {
    if (ctx->stacklen == 0) return NULL;
    return ctx->stack[--ctx->stacklen];
}

/* Return the pointer to the last object (if offset == 0) on the stack
 * or NULL. Offset of 1 means penultimate and so forth.  */
obj *stackPeek(aoclactx *ctx, size_t offset) {
    if (ctx->stacklen <= offset) return NULL;
    return ctx->stack[ctx->stacklen-1-offset];
}

/* Show the current content of the stack. */
#define STACK_SHOW_MAX_ELE 10
void stackShow(aoclactx *ctx) {
    ssize_t j = ctx->stacklen - STACK_SHOW_MAX_ELE;
    if (j < 0) j = 0;
    while(j < (ssize_t)ctx->stacklen) {
        obj *o = ctx->stack[j];
        printobj(o,PRINT_COLOR|PRINT_REPR); printf(" ");
        j++;
    }
    if (ctx->stacklen > STACK_SHOW_MAX_ELE)
        printf("[... %zu more object ...]", j);
    if (ctx->stacklen) printf("\n");
}

/* ================================ Eval ==================================== */

/* Evaluate the program in the list 'l' in the specified context 'ctx'.
 * Expects a list object. Evaluation uses the following rules:
 *
 * 1. List elements are scanned from left to right.
 * 2. If an element is a symbol, a function bound to such symbol is
 *    searched and executed. If no function is found with such a name
 *    an error is raised.
 * 3. If an element is a tuple, the stack elements are captured into the
 *    local variables with the same names as the tuple elements. If we
 *    run out of stack, an error is raised.
 * 4. Any other object type is just pushed on the stack.
 *
 * Return 1 on runtime erorr. Otherwise 0 is returned.
 */
int eval(aoclactx *ctx, obj *l) {
    assert (l->type == OBJ_TYPE_LIST);

    for (size_t j = 0; j < l->l.len; j++) {
        obj *o = l->l.ele[j];
        aproc *proc;
        ctx->frame->curline = o->line;

        switch(o->type) {
        case OBJ_TYPE_TUPLE:                /* Capture variables. */
            /* Quoted tuples just get pushed on the stack, losing
             * their quoted status. */
            if (o->l.quoted) {
                obj *notq = deepCopy(o);
                notq->l.quoted = 0;
                stackPush(ctx,notq);
                break;
            }

            if (ctx->stacklen < o->l.len) {
                setError(ctx,o->l.ele[ctx->stacklen]->str.ptr,
                    "Out of stack while capturing local");
                return 1;
            }

            ctx->stacklen -= o->l.len;
            for (size_t i = 0; i < o->l.len; i++) {
                int idx = o->l.ele[i]->str.ptr[0];
                release(ctx->frame->locals[idx]);
                ctx->frame->locals[idx] =
                    ctx->stack[ctx->stacklen+i];
            }
            break;
        case OBJ_TYPE_SYMBOL:
            /* Quoted symbols don't generate a procedure call, but like
             * any other object they get pushed on the stack. */
            if (o->str.quoted) {
                obj *notq = deepCopy(o);
                notq->str.quoted = 0;
                stackPush(ctx,notq);
                break;
            }

            /* Not quoted symbols get looked up and executed if they
             * don't start with "$". Otherwise are handled as locals
             * push on the stack. */
            if (o->str.ptr[0] == '$') {     /* Push local var. */
                int idx = o->str.ptr[1];
                if (ctx->frame->locals[idx] == NULL) {
                    setError(ctx,o->str.ptr, "Unbound local var");
                    return 1;
                }
                stackPush(ctx,ctx->frame->locals[idx]);
                retain(ctx->frame->locals[idx]);
            } else {                        /* Call procedure. */
                proc = lookupProc(ctx,o->str.ptr);
                if (proc == NULL) {
                    setError(ctx,o->str.ptr,
                        "Symbol not bound to procedure");
                    return 1;
                }
                if (proc->cproc) {
                    /* Call a procedure implemented in C. */
                    aproc *prev = ctx->frame->curproc;
                    ctx->frame->curproc = proc;
                    int err = proc->cproc(ctx);
                    ctx->frame->curproc = prev;
                    if (err) return err;
                } else {
                    /* Call a procedure implemented in Aocla. */
                    stackframe *oldsf = ctx->frame;
                    ctx->frame = newStackFrame(ctx);
                    ctx->frame->curproc = proc;
                    int err = eval(ctx,proc->proc);
                    freeStackFrame(ctx->frame);
                    ctx->frame = oldsf;
                    if (err) return err;
                }
            }
            break;
        default:
            stackPush(ctx,o);
            retain(o);
            break;
        }
    }
    return 0;
}

/* ============================== Library ===================================
 * Here we implement a number of things useful to play with the language.
 * Performance is not really a concern here, so certain core things are
 * implemented in Aocla itself for the sake of brevity.
 * ========================================================================== */

/* Make sure the stack len is at least 'min' or set an error and return 1.
 * If there are enough elements 0 is returned. */
int checkStackLen(aoclactx *ctx, size_t min) {
    if (ctx->stacklen < min) {
        setError(ctx,NULL,"Out of stack");
        return 1;
    }
    return 0;
}

/* Check that the stack elements contain at least 'count' elements of
 * the specified type. Otherwise set an error and return 1.
 * The function returns 0 if there are enough elements of the right type. */
int checkStackType(aoclactx *ctx, size_t count, ...) {
    if (checkStackLen(ctx,count)) return 1;
    va_list ap;
    va_start(ap, count);
    for (size_t i = 0; i < count; i++) {
        int type = va_arg(ap,int);
        if (!(type & ctx->stack[ctx->stacklen-count+i]->type)) {
            setError(ctx,NULL,"Type mismatch");
            return 1;
        }
    }
    va_end(ap);
    return 0;
}

/* Search for a procedure with that name. Return NULL if not found. */
aproc *lookupProc(aoclactx *ctx, const char *name) {
    aproc *this = ctx->proc;
    while(this) {
        if (!strcmp(this->name,name)) return this;
        this = this->next;
    }
    return NULL;
}

/* Allocate a new procedure object and link it to 'ctx'.
 * It's up to the caller to to fill the actual C or Aocla procedure pointer. */
aproc *newProc(aoclactx *ctx, const char *name) {
    aproc *ap = myalloc(sizeof(*ap));
    ap->name = myalloc(strlen(name)+1);
    memcpy((char*)ap->name,name,strlen(name)+1);
    ap->next = ctx->proc;
    ctx->proc = ap;
    return ap;
}

/* Add a procedure to the specified context. Either cproc or list should
 * not be null, depending on the fact the new procedure is implemented as
 * a C function or natively in Aocla. If the procedure already exists it
 * is replaced with the new one. */
void addProc(aoclactx *ctx, const char *name, int(*cproc)(aoclactx *), obj *list) {
    assert((cproc != NULL) + (list != NULL) == 1);
    aproc *ap = lookupProc(ctx, name);
    if (ap) {
        if (ap->proc != NULL) {
            release(ap->proc);
            ap->proc = NULL;
        }
    } else {
        ap = newProc(ctx,name);
    }
    ap->proc = list;
    ap->cproc = cproc;
}

/* Add a procedure represented by the Aocla code 'prog', that must
 * be a valid list. On error (not valid list) 1 is returned, otherwise 0. */
int addProcString(aoclactx *ctx, const char *name, const char *prog) {
    obj *list = parseObject(NULL,prog,NULL,NULL);
    if (prog == NULL) return 1;
    addProc(ctx,name,NULL,list);
    return 0;
}

/* Implements +, -, *, %, ... */
int procBasicMath(aoclactx *ctx) {
    if (checkStackType(ctx,2,OBJ_TYPE_INT,OBJ_TYPE_INT)) return 1;
    obj *a = stackPop(ctx);
    obj *b = stackPop(ctx);

    int res;
    const char *fname = ctx->frame->curproc->name;
    if (fname[0] == '+' && fname[1] == 0) res = a->i + b->i;
    if (fname[0] == '-' && fname[1] == 0) res = a->i - b->i;
    if (fname[0] == '*' && fname[1] == 0) res = a->i * b->i;
    if (fname[0] == '/' && fname[1] == 0) res = a->i / b->i;
    stackPush(ctx,newInt(res));
    release(a);
    release(b);
    return 0;
}

/* Implements ==, >=, <=, !=. */
int procCompare(aoclactx *ctx) {
    if (checkStackLen(ctx,2)) return 1;
    obj *b = stackPop(ctx);
    obj *a = stackPop(ctx);
    int cmp = compare(a,b);
    if (cmp == COMPARE_TYPE_MISMATCH) {
        stackPush(ctx,b);
        stackPush(ctx,a);
        setError(ctx,NULL,"Type mismatch in comparison");
        return 1;
    }

    int res;
    const char *fname = ctx->frame->curproc->name;
    if (fname[1] == '=') {
        switch(fname[0]) {
        case '=': res = cmp == 0; break;
        case '!': res = cmp != 0; break;
        case '>': res = cmp >= 0; break;
        case '<': res = cmp <= 0; break;
        }
    } else {
        switch(fname[0]) {
        case '>': res = cmp > 0; break;
        case '<': res = cmp < 0; break;
        }
    }
    stackPush(ctx,newBool(res));
    release(a);
    release(b);
    return 0;
}

/* Implements sort. Sorts a list in place. */
int procSortList(aoclactx *ctx) {
    if (checkStackType(ctx,1,OBJ_TYPE_LIST)) return 1;
    obj *l = stackPop(ctx);
    l = getUnsharedObject(l);
    qsort(l->l.ele,l->l.len,sizeof(obj*),qsort_obj_cmp);
    stackPush(ctx,l);
    return 0;
}

/* "def" let Aocla define new procedures, binding a list to a
 * symbol in the procedure table. */
int procDef(aoclactx *ctx) {
    if (checkStackType(ctx,2,OBJ_TYPE_LIST,OBJ_TYPE_SYMBOL)) return 1;
    obj *sym = stackPop(ctx);
    obj *code = stackPop(ctx);
    addProc(ctx,sym->str.ptr,NULL,code);
    release(sym);
    return 0;
}

/* if, ifelse, while.
 *
 * (list) => (result)               // if
 * (list list) => (result)          // ifelse and while
 *
 * We could implement while in AOCLA itself, once we have ifelse, however
 * this way we would build everything on a recursive implementation (still
 * we don't have tail recursion implemented), making every other thing
 * using while a issue with the stack length. Also stack trace on error
 * is a mess. And if you see the implementation, while is mostly an obvious
 * result of the ifelse implementation itself. */
int procIf(aoclactx *ctx) {
    int w = ctx->frame->curproc->name[0] == 'w';        /* while? */
    int e = ctx->frame->curproc->name[2] == 'e';        /* ifelse? */
    int retval = 1;
    if (e) {
        if (checkStackType(ctx,3,OBJ_TYPE_LIST,OBJ_TYPE_LIST,OBJ_TYPE_LIST))
            return 1;
    } else {
        if (checkStackType(ctx,2,OBJ_TYPE_LIST,OBJ_TYPE_LIST))
            return 1;
    }

    obj *elsebranch, *ifbranch, *cond;
    elsebranch = e ? stackPop(ctx) : NULL;
    ifbranch = stackPop(ctx);
    cond = stackPop(ctx);

    while(1) {
        /* Evaluate the conditional program. */
        if (eval(ctx,cond)) goto rterr;
        if (checkStackType(ctx,1,OBJ_TYPE_BOOL)) goto rterr;
        obj *condres = stackPop(ctx);
        int res = condres->istrue;
        release(condres);

        /* Now eval the true or false branch depending on the
         * result. */
        if (res) { /* True branch (if, ifelse, while). */
            if (eval(ctx,ifbranch)) goto rterr;
            if (w) continue;
        } else if (e) { /* False branch (ifelse). */
            if (eval(ctx,elsebranch)) goto rterr;
        }
        break;
    }
    retval = 0; /* Success. */

rterr:  /* Cleanup. We jump here on error with retval = 1. */
    release(cond);
    release(ifbranch);
    release(elsebranch);
    return retval;
}

/* Evaluate the given list, consuming it. */
int procEval(aoclactx *ctx) {
    if (checkStackType(ctx,1,OBJ_TYPE_LIST)) return 1;
    obj *l = stackPop(ctx);
    int retval = eval(ctx,l);
    release(l);
    return retval;
}

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

/* Print the top object to stdout, consuming it */
int procPrint(aoclactx *ctx) {
    if (checkStackLen(ctx,1)) return 1;
    obj *o = stackPop(ctx);
    printobj(o,PRINT_RAW);
    release(o);
    return 0;
}

/* Like print but also prints a newline at the end. */
int procPrintnl(aoclactx *ctx) {
    if (checkStackLen(ctx,1)) return 1;
    int ret = procPrint(ctx); printf("\n");
    return ret;
}

/* Len -- gets object len. Works with many types.
 * (object) => (len) */
int procLen(aoclactx *ctx) {
    if (checkStackType(ctx,1,OBJ_TYPE_LIST|OBJ_TYPE_TUPLE|OBJ_TYPE_STRING|
                             OBJ_TYPE_SYMBOL)) return 1;

    obj *o = stackPop(ctx);
    int len;
    switch(o->type) {
    case OBJ_TYPE_LIST: case OBJ_TYPE_TUPLE:    len = o->l.len; break;
    case OBJ_TYPE_STRING: case OBJ_TYPE_SYMBOL: len = o->str.len; break;
    }
    release(o);
    stackPush(ctx,newInt(len));
    return 0;
}

/* Implements -> and <-, appending element x in list with stack
 *
 * (x [1 2 3]) => ([1 2 3 x]) | ([x 1 2 3])
 *
 * <- is very inefficient as it memmoves all N elements. */
int procListAppend(aoclactx *ctx) {
    int tail = ctx->frame->curproc->name[0] == '-';     /* Append on tail? */
    if (checkStackType(ctx,2,OBJ_TYPE_ANY,OBJ_TYPE_LIST)) return 1;
    obj *l = getUnsharedObject(stackPop(ctx));
    obj *ele = stackPop(ctx);
    l->l.ele = myrealloc(l->l.ele,sizeof(obj*)*(l->l.len+1));
    if (tail) {
        l->l.ele[l->l.len] = ele;
    } else {
        memmove(l->l.ele+1,l->l.ele,sizeof(obj*)*l->l.len);
        l->l.ele[0] = ele;
    }
    l->l.len++;
    stackPush(ctx,l);
    return 0;
}

/* get@ -- get element at index. Works for lists, strings, tuples.
 * (object index) => (element). */
int procListGetAt(aoclactx *ctx) {
    if (checkStackType(ctx,2,OBJ_TYPE_LIST|OBJ_TYPE_STRING|OBJ_TYPE_TUPLE,
                             OBJ_TYPE_INT)) return 1;
    obj *idx = stackPop(ctx);
    obj *o = stackPop(ctx);
    int i = idx->i;
    size_t len = o->type == OBJ_TYPE_STRING ? o->str.len : o->l.len;
    if (i < 0) i = len+i; /* -1 is last element, and so forth. */
    release(idx);
    if (i < 0 || (size_t)i >= len) {
        stackPush(ctx,newBool(0)); // Out of index? Just push false.
    } else {
        if (o->type == OBJ_TYPE_STRING) {
            stackPush(ctx,newString(o->str.ptr+i,1));
        } else {
            stackPush(ctx,o->l.ele[i]);
            retain(o->l.ele[i]);
        }
    }
    release(o);
    return 0;
}

/* Show the current stack. Useful for debugging. */
int procShowStack(aoclactx *ctx) {
    stackShow(ctx);
    return 0;
}

/* Load the "standard library" of Aocla in the specified context. */
void loadLibrary(aoclactx *ctx) {
    addProc(ctx,"+",procBasicMath,NULL);
    addProc(ctx,"-",procBasicMath,NULL);
    addProc(ctx,"*",procBasicMath,NULL);
    addProc(ctx,"/",procBasicMath,NULL);
    addProc(ctx,"==",procCompare,NULL);
    addProc(ctx,">=",procCompare,NULL);
    addProc(ctx,">",procCompare,NULL);
    addProc(ctx,"<=",procCompare,NULL);
    addProc(ctx,"<",procCompare,NULL);
    addProc(ctx,"!=",procCompare,NULL);
    addProc(ctx,"sort",procSortList,NULL);
    addProc(ctx,"def",procDef,NULL);
    addProc(ctx,"if",procIf,NULL);
    addProc(ctx,"ifelse",procIf,NULL);
    addProc(ctx,"while",procIf,NULL);
    addProc(ctx,"eval",procEval,NULL);
    addProc(ctx,"upeval",procUpeval,NULL);
    addProc(ctx,"print",procPrint,NULL);
    addProc(ctx,"printnl",procPrintnl,NULL);
    addProc(ctx,"len",procLen,NULL);
    addProc(ctx,"->",procListAppend,NULL);
    addProc(ctx,"<-",procListAppend,NULL);
    addProc(ctx,"get@",procListGetAt,NULL);
    addProc(ctx,"showstack",procShowStack,NULL);

    /* Since the point of this interpreter to be a short and understandable
     * programming example, we implement as much as possible in Aocla itself
     * without caring much about performances. */
    addProcString(ctx,"dup","[(x) $x $x]");
    addProcString(ctx,"swap","[(x y) $y $x]");
    addProcString(ctx,"drop","[(_)]");

    /* [1 2 3] [dup *] map => [1 4 9] */
    addProcString(ctx,"map", "[(l f) $l len (e) 0 (j) [] [$j $e <] [ $l $j get@ $f upeval swap -> $j 1 + (j)] while]");

    /* [1 2 3] [printnl] foreach */
    addProcString(ctx,"foreach"," [(l f) $l len (e) 0 (j) [$j $e <] [$l $j get@ $f upeval $j 1 + (j)] while]");

    /* [1 2 3] first => 1 */
    addProcString(ctx,"first","[0 get@]");

    /* [1 2 3] rest => [2 3] */
    addProcString(ctx,"rest","[#t (f) [] (n) [[$f] [#f (f) drop] [$n -> (n)] ifelse] foreach $n]");

    /* [1 2 3] [4 5 6] cat => [1 2 3 4 5 6] */
    addProcString(ctx,"cat","[(a b) $b [$a -> (a)] foreach $a]");
}

/* ================================ CLI ===================================== */

/* Real Eval Print Loop. */
void repl(void) {
    char buf[1024];
    aoclactx *ctx = newInterpreter();
    while(1) {
        printf("aocla> "); fflush(stdout);

        /* Aocla programs are Aocla lists, so when users just write
         * in the REPL we need to surround with []. */
        buf[0] = '[';

        if (fgets(buf+1,sizeof(buf)-2,stdin) == NULL) break;
        size_t l = strlen(buf);
        if (l && buf[l-1] == '\n') buf[--l] = 0;
        if (l == 0) continue;

        /* Add closing ]. */
        buf[l] = ']';
        buf[l+1] = 0;

        obj *list = parseObject(ctx,buf,NULL,NULL);
        if (!list) {
            printf("Parsing program: %s\n", ctx->errstr);
            continue;
        }
        if (eval(ctx,list)) {
            printf("%s\n", ctx->errstr);
        } else {
            stackShow(ctx);
        }
        release(list);
    }
}

/* Execute the program contained in the specified filename.
 * Return 1 on error, 0 otherwise. */
int evalFile(const char *filename, char **argv, int argc) {
    FILE *fp = fopen(filename,"r");
    if (!fp) {
        perror("Opening file");
        return 1;
    }

    /* Read file into buffer. */
    int incrlen = 1024; /* How much to allocate when we are out of buffer. */
    char *buf = myalloc(incrlen);
    size_t buflen = 1, nread;
    size_t leftspace = incrlen-buflen;
    buf[0] = '[';
    while((nread = fread(buf+buflen,1,leftspace,fp)) > 0) {
        buflen += nread;
        leftspace -= nread;
        if (leftspace == 0) {
            buf = myrealloc(buf,buflen+incrlen);
            leftspace += incrlen;
        }
    }
    if (leftspace < 2) buf = myrealloc(buf,buflen+2);
    buf[buflen++] = ']';
    buf[buflen++] = 0;
    fclose(fp);

    /* Parse the program before eval(). */
    aoclactx *ctx = newInterpreter();
    int line = 1;
    obj *l = parseObject(ctx,buf,NULL,&line);
    free(buf);
    if (!l) {
        printf("Parsing program: %s\n", ctx->errstr);
        return 1;
    }

    /* Before evaluating the program, let's push on the arguments
     * we received on the stack. */
    for (int j = 0; j < argc; j++) {
        obj *o = parseObject(NULL,argv[j],NULL,0);
        if (!o) {
            printf("Parsing command line argument: %s\n", ctx->errstr);
            release(l);
            return 1;
        }
        stackPush(ctx,o);
    }

    /* Run the program. */
    int retval = eval(ctx,l);
    if (retval) printf("Runtime error: %s\n", ctx->errstr);
    release(l);
    return retval;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        repl();
    } else if (argc >= 2) {
        if (evalFile(argv[1],argv+2,argc-2)) return 1;
    }
    return 0;
}
