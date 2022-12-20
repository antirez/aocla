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
 * and so forth. */
#define OBJ_TYPE_INT  0
#define OBJ_TYPE_LIST 1
#define OBJ_TYPE_TUPLE 2
#define OBJ_TYPE_STRING 3
#define OBJ_TYPE_SYMBOL 4
typedef struct obj {
    int type;       /* OBJ_TYPE_... */
    int refcount;   /* Reference count. */
    union {
        int i;      /* Integer. Literal: 1234 */
        struct {    /* List: Literal: [1,2,3,4] or [1 2 3 4] */
            struct obj **ele;
            size_t len;
        } l;
        struct {    /* Mutable string & unmutable symbol. */
            char *ptr;
            size_t len;
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
    int (*cproc)(const char *, struct aoclactx *); /* C procedure. */
    struct aproc *next;
} aproc;

/* We have local vars, so we need a stack frame. We start with a top level
 * stack frame. Each time a procedure is called, we create a new stack frame
 * and free it once the procedure returns. */
#define AOCLA_NUMVARS ('z'-'a'+1)
typedef struct stackframe {
    obj *locals[AOCLA_NUMVARS];/* Local var names are limited to a,b,c,...,z. */
    aproc *curproc;            /* Current procedure executing or NULL.  */
} stackframe;

/* Interpreter state. */
#define ERRSTR_LEN 128
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
        case OBJ_TYPE_INT: break; /* Nothing nested to free. */
        case OBJ_TYPE_LIST:
            for (size_t j = 0; j < o->l.len; j++)
                release(o->l.ele[j]);
            free(o->l.ele);
            break;
        }
        free(o);
    }
}

/* Increment the object ref count. Use when a new reference is created. */
void retain(obj *o) {
    o->refcount++;
}

/* Return true if the character 'c' is within the Aocla symbols charset. */
int issymbol(int c) {
    if (isalpha(c)) return 1;
    switch(c) {
    case '$':
    case '+':
    case '-':
    case '*':
    case '/':
    case '=':
    case '?':
    case '%':
        return 1;
    default:
        return 0;
    }
}

/* Given the string 's' return the obj representing the list or
 * NULL on syntax error. '*next' is set to the next byte to parse, after
 * the current e was completely parsed.
 *
 * Returned object has a ref count of 1. */
obj *parseList(aoclactx *ctx, const char *s, const char **next) {
    obj *o = myalloc(sizeof(*o));
    o->refcount = 1;
    while(isspace(s[0])) s++;
    if (s[0] == '-' || isdigit(s[0])) { /* Integer. */
        char buf[64];
        size_t len = 0;
        while((*s == '-' || isdigit(*s)) && len < sizeof(buf)-1)
            buf[len++] = *s++;
        buf[len] = 0;
        o->type = OBJ_TYPE_INT;
        o->i = atoi(buf);
        if (next) *next = s;
        return o;
    } else if (s[0] == '[' || s[0] == '(') { /* List or Tuple. */
        o->type = s[0] == '[' ? OBJ_TYPE_LIST : OBJ_TYPE_TUPLE;
        o->l.len = 0;
        o->l.ele = NULL;
        s++;
        /* Parse comma separated elements. */
        while(1) {
            /* The list may be empty, so we need to parse for "]"
             * ASAP. */
            while(isspace(s[0])) s++;
            if ((o->type == OBJ_TYPE_LIST && s[0] == ']') ||
                (o->type == OBJ_TYPE_TUPLE && s[0] == ')')) {
                if (next) *next = s+1;
                return o;
            }

            /* Parse the current sub-element recursively. */
            const char *nextptr;
            obj *element = parseList(ctx,s,&nextptr);
            if (element == NULL) {
                release(o);
                return NULL;
            } else if (o->type == OBJ_TYPE_TUPLE &&
                       (element->type != OBJ_TYPE_SYMBOL ||
                        element->str.len != 1 ||
                        !islower(element->str.ptr[0])))
            {
                /* Tuples can be only composed of one character symbols. */
                release(element);
                release(o);
                setError(ctx,s,"Non lower case letter in tuple");
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
        const char *end = s;
        while(issymbol(*end)) end++;
        o->str.len = end-s;
        char *dest = myalloc(o->str.len+1);
        o->str.ptr = dest;
        memcpy(dest,s,o->str.len);
        dest[o->str.len] = 0;
        *next = end;
    } else if (s[0] == '"') {           /* String. */
        printf("IMPLEMENT STRING PARSING\n");
        exit(1);
    } else {
        /* Syntax error. */
        setError(ctx,s,"No object type starts like this");
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
void printobj(obj *obj, int color) {
    const char *escape;
    if (color) {
        switch(obj->type) {
        case OBJ_TYPE_LIST: escape = "\033[33;1m"; break;       /* Yellow. */
        case OBJ_TYPE_TUPLE: escape = "\033[34;1m"; break;      /* Blue. */
        case OBJ_TYPE_SYMBOL: escape = "\033[36;1m"; break;     /* Cyan. */
        case OBJ_TYPE_STRING: escape = "\033[32;1m"; break;     /* Green. */
        case OBJ_TYPE_INT: escape = "\033[37;1m"; break;        /* Gray. */
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
    case OBJ_TYPE_LIST:
    case OBJ_TYPE_TUPLE:
        printf("%c",obj->type == OBJ_TYPE_LIST ? '[' : '(');
        for (size_t j = 0; j < obj->l.len; j++) {
            printobj(obj->l.ele[j],color);
            if (j != obj->l.len-1) printf(", ");
        }
        if (color) printf("%s",escape); /* Restore upper level color. */
        printf("%c",obj->type == OBJ_TYPE_LIST ? ']' : ')');
        break;
    }
    if (color) printf("\033[0m"); /* Color off. */
}

/* Allocate a new object of type 'type. */
obj *newObject(int type) {
    obj *o = myalloc(sizeof(*o));
    o->refcount = 1;
    o->type = type;
    return o;
}

/* Allocate an int object with value 'i'. */
obj *newInt(int i) {
    obj *o = newObject(OBJ_TYPE_INT);
    o->i = i;
    return o;
}

/* Deep copy the passed object. Return an object with refcount = 1. */
obj *deepCopy(obj *o) {
    if (o == NULL) return NULL;
    /* TODO: actually implement it. */
    printf("IMPLEMENT ME \\o/\n");
    exit(1);
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
    snprintf(ctx->errstr,ERRSTR_LEN,"%s: %.30s%s",
        msg,ptr,strlen(ptr)>30 ? "..." :"");
}

/* Create a new stack frame. */
stackframe *newStackFrame(void) {
    stackframe *sf = myalloc(sizeof(*sf));
    memset(sf->locals,0,sizeof(sf->locals));
    sf->curproc = NULL;
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
    i->frame = newStackFrame();
    loadLibrary(i);
    return i;
}

/* Push an object on the interpreter stack. No refcount change. */
void stackPush(aoclactx *ctx, obj *o) {
    ctx->stack = myrealloc(ctx->stack,sizeof(obj*) * (ctx->stacklen+1));
    ctx->stack[ctx->stacklen++] = o;
    retain(o);
}

/* Pop an object from the stack without modifying its refcount.
 * Return NULL if stack is empty. */
obj *stackPop(aoclactx *ctx) {
    if (ctx->stacklen == 0) return NULL;
    return ctx->stack[--ctx->stacklen];
}

/* Return the pointer to the last object on the stack or NULL. */
obj *stackPeek(aoclactx *ctx) {
    if (ctx->stacklen == 0) return NULL;
    return ctx->stack[ctx->stacklen-1];
}

/* Show the current content of the stack. */
#define STACK_SHOW_MAX_ELE 10
void stackShow(aoclactx *ctx) {
    ssize_t j = ctx->stacklen - STACK_SHOW_MAX_ELE;
    if (j < 0) j = 0;
    while(j < (ssize_t)ctx->stacklen) {
        obj *o = ctx->stack[j];
        printobj(o,1); printf(" ");
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

        switch(o->type) {
        case OBJ_TYPE_TUPLE:    /* Capture variables. */
            if (ctx->stacklen < o->l.len) {
                setError(ctx,NULL,"Out of stack while capturing locals");
                return 1;
            }

            ctx->stacklen -= o->l.len;
            for (size_t i = 0; i < o->l.len; i++) {
                int idx = o->l.ele[i]->str.ptr[0] - 'a';
                release(ctx->frame->locals[idx]);
                ctx->frame->locals[idx] =
                    ctx->stack[ctx->stacklen - o->l.len];
            }
            break;
        case OBJ_TYPE_SYMBOL:   /* Execute procedure. */
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
                int err = proc->cproc(o->str.ptr,ctx);
                ctx->frame->curproc = prev;
                if (err) return err;
            } else {
                /* Call a procedure implemented in Aocla. */
                stackframe *oldsf = ctx->frame;
                ctx->frame = newStackFrame();
                ctx->frame->curproc = proc;
                int err = eval(ctx,proc->proc);
                freeStackFrame(ctx->frame);
                ctx->frame = oldsf;
                if (err) return err;
            }
            break;
        default:
            stackPush(ctx,o);
            break;
        }
    }
    return 0;
}

/* ============================== Library =================================== */

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
        if (type != ctx->stack[ctx->stacklen-1-i]->type) {
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
 * not be null. */
void addProc(aoclactx *ctx, const char *name, int(*cproc)(const char *, aoclactx *), obj *list) {
    assert((cproc != NULL) + (list != NULL) == 1);
    aproc *ap = newProc(ctx,name);
    ap->proc = list;
    ap->cproc = cproc;
}

/* Implements +, -, *, %, ... */
int procBasicMath(const char *fname, aoclactx *ctx) {
    if (checkStackType(ctx,2,OBJ_TYPE_INT,OBJ_TYPE_INT)) return 1;
    obj *a = stackPop(ctx);
    obj *b = stackPop(ctx);

    int res;
    if (fname[0] == '+' && fname[1] == 0) res = a->i + b->i;
    if (fname[0] == '-' && fname[1] == 0) res = a->i - b->i;
    if (fname[0] == '*' && fname[1] == 0) res = a->i * b->i;
    if (fname[0] == '/' && fname[1] == 0) res = a->i / b->i;
    stackPush(ctx,newInt(res));
    return 0;
}

/* Implements ==, >=, <=, !=. */
int procCompare(const char *fname, aoclactx *ctx) {
    if (checkStackLen(ctx,2)) return 1;
    obj *a = stackPop(ctx);
    obj *b = stackPop(ctx);
    int cmp = compare(a,b);
    if (cmp == COMPARE_TYPE_MISMATCH) {
        stackPush(ctx,b);
        stackPush(ctx,a);
        setError(ctx,NULL,"Type mismatch in comparison");
        return 1;
    }

    int res;
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
    stackPush(ctx,newInt(res));
    return 0;
}

/* Implements sort. Sorts a list in place. */
int procSortList(const char *fname, aoclactx *ctx) {
    NOTUSED(fname);
    if (checkStackType(ctx,1,OBJ_TYPE_LIST)) return 1;
    obj *l = stackPop(ctx);
    l = getUnsharedObject(l);
    qsort(l->l.ele,l->l.len,sizeof(obj*),qsort_obj_cmp);
    stackPush(ctx,l);
    return 0;
}

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

        obj *list = parseList(ctx,buf,NULL);
        if (!list) {
            printf("Parsing string: %s\n", ctx->errstr);
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

void evalFile(const char *filename, char **argv, int argc) {
}

int main(int argc, char **argv) {
    if (argc == 1) {
        repl();
    } else if (argc >= 2) {
        evalFile(argv[1],argv+1,argc-1);
    }
    return 0;
}
