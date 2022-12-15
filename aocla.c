#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>

/* =========================== Data structures ============================== */

/* This describes our Aocla object type. It can be used to represent
 * lists (and code: they are the same type in Aocla), integers, strings
 * and so forth. */
#define OBJ_TYPE_INT  0
#define OBJ_TYPE_LIST 1
#define OBJ_TYPE_STRING 2
#define OBJ_TYPE_SYMBOL 3
typedef struct obj {
    int type;       /* OBJ_TYPE_... */
    union {
        int i;      /* Integer. Literal: 1234 */
        struct {    /* List: Literal: [1,2,3,4] or [1 2 3 4] */
            struct obj **ele;
            size_t len;
        } l;
        struct {    /* Mutable string. Literal: "Hello World"  */
            char *ptr;
            size_t len;
        } str;
        struct sym { /* Symbol (non mutable string). Literal: foo */
            const char *ptr;
            size_t len;
        } sym;
    };
} obj;

/* Procedures. They are just lists with names. */
typedef struct aproc {
    const char *name;
    obj *list;
    struct aproc *next;
} aproc;

/* We have local vars, so we need a stack frame. We start with a top level
 * stack frame. Each time a procedure is called, we create a new stack frame
 * and free it once the procedure returns. */
#define AOCLA_NUMVARS ('z'-'a'+1)
typedef struct stackframe {
    obj *locals[AOCLA_NUMVARS];/* Local var names are limited to a,b,c,...,z. */
    int lstate[AOCLA_NUMVARS]; /* Local state. When a local is assigned, it's
                               set to 1. If a local is pushed, it drops to zero
                               (but lcoals[N] will still be not NULL). So
                               next time it is pushed, we know that we need
                               to perform a deep copy of the object. */
} stackframe;

/* Interpreter state. */
typedef struct aoclactx {
    size_t maxstack, sl;    /* Stack max len and stack current len. */
    obj **stack;
    aproc *proc;            /* Procedures. Lists bound to specific names. */
    stackframe *frame;      /* Stack frame with locals. */
} aoclactx;

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

/* Recursively free an Aocla object. */
void freeobj(obj *o) {
    switch(o->type) {
    case OBJ_TYPE_INT: break; /* Nothing nested to free. */
    case OBJ_TYPE_LIST:
        for (size_t j = 0; j < o->l.len; j++)
            freeobj(o->l.ele[j]);
        free(o->l.ele);
        break;
    }
    free(o);
}

/* Return true if the character 'c' is within the Aocla symbols charset. */
int issymbol(int c) {
    if (isalpha(c)) return 1;
    switch(c) {
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
 * the current e was completely parsed. */
obj *parseList(const char *s, const char **next) {
    obj *o = myalloc(sizeof(*o));
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
    } else if (s[0] == '[') {           /* List. */
        o->type = OBJ_TYPE_LIST;
        o->l.len = 0;
        o->l.ele = NULL;
        s++;
        /* Parse comma separated elements. */
        while(1) {
            /* The list may be empty, so we need to parse for "]"
             * ASAP. */
            while(isspace(s[0])) s++;
            if (s[0] == ']') {
                if (next) *next = s+1;
                return o;
            }

            /* Parse the current sub-element recursively. */
            const char *nextptr;
            obj *element = parseList(s,&nextptr);
            if (element == NULL) {
                freeobj(o);
                return NULL;
            }
            o->l.ele = myrealloc(o->l.ele, sizeof(obj*)*(o->l.len+1));
            o->l.ele[o->l.len++] = element;
            s = nextptr; /* Continue from first byte not parsed. */

            continue; /* Parse next element. */

            /* Syntax error. */
            freeobj(o);
            return NULL;
        }
        /* Syntax error (list not closed). */
        freeobj(o);
        return NULL;
    } else if (issymbol(s[0])) {         /* Symbol. */
        o->type = OBJ_TYPE_SYMBOL;
        const char *end = s;
        while(issymbol(*end)) end++;
        o->sym.len = end-s;
        char *dest = malloc(o->sym.len+1);
        o->sym.ptr = dest;
        memcpy(dest,s,o->sym.len);
        dest[o->sym.len] = 0;
        *next = end;
    } else if (s[0] == '"') {           /* String. */
        printf("IMPLEMENT STRING PARSING\n");
        exit(1);
    } else {
        /* Syntax error. */
        return NULL;
    }
    return o;
}

/* Compare the two objects 'a' and 'b' and return:
 * -1 if a<b; 0 if a==b; 1 if a>b. */
int compare(obj *a, obj *b) {
    if (a->type == OBJ_TYPE_INT && b->type == OBJ_TYPE_INT) {
        if (a->i < b->i) return -1;
        else if (a->i > b->i) return 1;
        return 0;
    }

    /* If one of the objects is not a list, promote it to a list.
     * Just use the stack to avoid allocating stuff for a single
     * element list. */
    obj list, listele, *ele[1];
    list.type = OBJ_TYPE_LIST;
    list.l.len = 1;
    list.l.ele = ele;
    list.l.ele[0] = &listele;
    listele.type = OBJ_TYPE_INT;

    /* Promote. */
    if (a->type == OBJ_TYPE_INT) {
        listele.i = a->i;
        a = &list;
    } else if (b->type == OBJ_TYPE_INT) {
        listele.i = b->i;
        b = &list;
    }

    /* Now we can handle the list to list comparison without
     * special cases. */
    size_t minlen = a->l.len < b->l.len ? a->l.len : b->l.len;
    for (size_t j = 0; j < minlen; j++) {
        int cmp = compare(a->l.ele[j],b->l.ele[j]);
        if (cmp != 0) return cmp;
    }

    /* First MIN(len_a,len_b) elements are the same? Longer list wins. */
    if (a->l.len < b->l.len) return -1;
    else if (a->l.len > b->l.len) return 1;
    return 0;
}

/* qsort() helper to sort arrays of obj pointers. */
int qsort_list_cmp(const void *a, const void *b) {
    obj **obja = (obj**)a, **objb = (obj**)b;
    return compare(obja[0],objb[0]);
}

/* Output an object human readable representation .*/
void printobj(obj *obj) {
    switch(obj->type) {
    case OBJ_TYPE_INT:
        printf("%d",obj->i);
        break;
    case OBJ_TYPE_SYMBOL:
        printf("%s",obj->sym.ptr);
        break;
    case OBJ_TYPE_LIST:
        printf("[");
        for (size_t j = 0; j < obj->l.len; j++) {
            printobj(obj->l.ele[j]);
            if (j != obj->l.len-1) printf(", ");
        }
        printf("]");
        break;
    }
}

/* ========================== Interpreter state ============================= */

/* Create a new stack frame. */
stackframe *newStackFrame(void) {
    stackframe *sf = myalloc(sizeof(*sf));
    memset(sf->locals,0,sizeof(sf->locals));
    memset(sf->lstate,0,sizeof(sf->lstate));
    return sf;
}

/* Free a stack frame. */
void freeStackFrame(stackframe *sf) {
    for (int j = 0; j < AOCLA_NUMVARS; j++)
        if (sf->locals[j]) freeobj(sf->locals[j]);
    free(sf);
}

#define AOCLA_STACK_MAX 256
aoclactx *newInterpreter(void) {
    aoclactx *i = myalloc(sizeof(*i));
    i->maxstack = AOCLA_STACK_MAX;
    i->sl = 0;
    i->stack = myalloc(sizeof(obj*)*i->maxstack);
    i->proc = NULL; /* That's a linked list. Starts empty. */
    i->frame = newStackFrame();
    return i;
}

/* ================================ Eval ==================================== */

/* Evaluate the program in the list 'l' in the specified context 'ctx'. */
void eval(aoclactx *ctx, obj *l) {
    if (l->type != OBJ_TYPE_LIST) return;
    printobj(l);
    printf("\n");
}

/* ================================ CLI ===================================== */

/* Read the lists contained in the file 'fp', parse them into an obj
 * type and populate v[...] with the es. The number of lists processed
 * is returned. */
int readLists(FILE *fp, obj **v, size_t vlen) {
    char buf[1024];
    size_t idx = 0;
    while(fgets(buf,sizeof(buf),fp) != NULL && idx < vlen) {
        size_t l = strlen(buf);
        if (l <= 1) continue;
        if (buf[l-1] == '\n') {
            buf[l-1] = 0;
            l--;
        }
        v[idx++] = parseList(buf,NULL);
    }
    return idx;
}

/* Real Eval Print Loop. */
void repl(void) {
    char buf[1024];
    aoclactx *ctx = newInterpreter();
    while(1) {
        printf("aocla> "); fflush(stdout);
        if (fgets(buf,sizeof(buf)-2,stdin) == NULL) break;
        size_t l = strlen(buf);
        if (l && buf[l-1] == '\n') buf[--l] = 0;
        if (l == 0) continue;

        /* Aocla programs are Aocla lists, so when users just write
         * in the REPL we need to surround with []. */
        memmove(buf+1,buf,l);
        buf[0] = '[';
        buf[l+1] = ']';
        buf[l+2] = 0;

        obj *list = parseList(buf,NULL);
        if (!list) {
            printf("Syntax error\n");
            continue;
        }
        eval(ctx,list);
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
