/*
 * This software is part of the SBCL system. See the README file for
 * more information.
 *
 * This software is derived from the CMU CL system, which was
 * written at Carnegie Mellon University and released into the
 * public domain. The software is in the public domain and is
 * provided with absolutely no warranty. See the COPYING and CREDITS
 * files for more information.
 */

#include "sbcl.h"

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <sys/time.h>
#ifndef LISP_FEATURE_WIN32
#include <sys/resource.h>
#endif
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include "runtime.h"
#include "parse.h"
#include "vars.h"

#include "print.h"
#include "arch.h"
#include "interr.h"
#include "search.h"
#include "purify.h"
#include "globals.h"
#include "lispregs.h"
#include "interrupt.h"
#include "thread.h"
#include "genesis/static-symbols.h"
#include "genesis/primitive-objects.h"
#include "genesis/gc-tables.h"
#include "gc-internal.h"


/* When we need to do command input, we use this stream, which is not
 * in general stdin, so that things will "work" (as well as being
 * thrown into ldb can be considered "working":-) even in a process
 * where standard input has been redirected to a file or pipe.
 *
 * (We could set up output to go to a special ldb_out stream for the
 * same reason, but there's been no pressure for that so far.)
 *
 * The enter-the-ldb-monitor function is responsible for setting up
 * this stream. */
static FILE *ldb_in = 0;
static int ldb_in_fd = -1;

typedef int cmd(char **ptr);

struct crash_preamble {
    uword_t signature;
    uword_t static_start;
    uword_t static_nbytes;
    uword_t dynspace_start;
    long dynspace_npages;
    int card_size;
    int card_table_nbits;
    // fixedobj data dumped: pages, page table
    uword_t fixedobj_start, fixedobj_size, fixedobj_free_pointer;
    // varyobj data dumped: pages, touched_bits, page table
    uword_t varyobj_start, varyobj_size, varyobj_free_pointer;
    int nthreads;
    int tls_size;
    lispobj lisp_package_vector;
    char sprof_enabled;
    char pin_dynspace_code;
    int sizeof_context;
};
struct crash_thread_preamble {
    uword_t address;
    uword_t has_context;
    uword_t control_stack_nbytes;
    uword_t binding_stack_nbytes;
};

// Prevent some mixups in case you add fields to the crash dump
const int CRASH_PREAMBLE_SIGNATURE =
    (sizeof (struct crash_preamble) << 16) | sizeof (struct crash_thread_preamble);

#if defined LISP_FEATURE_X86_64 // un-tested elsewhere
#include <errno.h>
static void checked_write(int fd, void* buf, long nbytes)
{
    ssize_t wrote = write(fd, buf, nbytes);
    if (wrote != nbytes) lose("short write, errno=%d", errno);
}

#include "immobile-space.h"
void save_gc_crashdump(char *pathname,
                       uword_t approx_stackptr_at_gc_start)
{
    extern int pin_all_dynamic_space_code;
    int fd = open(pathname, O_WRONLY|O_CREAT, 0666);
    struct thread* th;
    int nthreads = 0;
    for_each_thread(th) ++nthreads;
    fprintf(stderr, "save: %d threads\n", nthreads);
    struct crash_preamble preamble;
    unsigned long nbytes_heap = next_free_page * GENCGC_PAGE_BYTES;
#ifdef LISP_FEATURE_SB_THREAD
    int nbytes_tls = SymbolValue(FREE_TLS_INDEX,0);
#else
    int nbytes_tls = sizeof (struct thread);
#endif
    preamble.signature = CRASH_PREAMBLE_SIGNATURE;
    preamble.static_start = STATIC_SPACE_START;
    preamble.static_nbytes = (uword_t)static_space_free_pointer - STATIC_SPACE_START;
    preamble.dynspace_start = DYNAMIC_SPACE_START;
    preamble.dynspace_npages = next_free_page;
    preamble.card_size = GENCGC_CARD_BYTES;
    preamble.card_table_nbits = gc_card_table_nbits;
    preamble.nthreads = nthreads;
    preamble.tls_size = nbytes_tls;
    preamble.lisp_package_vector = lisp_package_vector;
    preamble.sprof_enabled = sb_sprof_enabled;
    preamble.pin_dynspace_code = pin_all_dynamic_space_code;
    preamble.sizeof_context = sizeof (os_context_t);
#ifdef LISP_FEATURE_IMMOBILE_SPACE
    preamble.fixedobj_start = FIXEDOBJ_SPACE_START;
    preamble.fixedobj_size = FIXEDOBJ_SPACE_SIZE;
    preamble.fixedobj_free_pointer = (uword_t)fixedobj_free_pointer;
    preamble.varyobj_start = VARYOBJ_SPACE_START;
    preamble.varyobj_size = VARYOBJ_SPACE_SIZE;
    preamble.varyobj_free_pointer = (uword_t)varyobj_free_pointer;
#endif
    // write the preamble and static space
    checked_write(fd, &preamble, sizeof preamble);
    checked_write(fd, (char*)STATIC_SPACE_START, preamble.static_nbytes);
    // write the dynamic-space, PTEs, card table

    fprintf(stderr, "Writing dynamic space\n");
    checked_write(fd, (char*)DYNAMIC_SPACE_START, nbytes_heap);
    checked_write(fd, page_table, sizeof (struct page) * next_free_page);
    checked_write(fd, gc_card_mark, 1+gc_card_table_mask);
#ifdef LISP_FEATURE_IMMOBILE_SPACE
    int usage = (uword_t)fixedobj_free_pointer - FIXEDOBJ_SPACE_START;
    checked_write(fd, (char*)FIXEDOBJ_SPACE_START, usage);
    int total_npages = FIXEDOBJ_SPACE_SIZE / IMMOBILE_CARD_BYTES;
    checked_write(fd, fixedobj_pages, total_npages * sizeof sizeof(struct fixedobj_page));
    usage = (uword_t)varyobj_free_pointer - VARYOBJ_SPACE_START;
    checked_write(fd, (char*)VARYOBJ_SPACE_START, usage);
    total_npages = VARYOBJ_SPACE_SIZE / IMMOBILE_CARD_BYTES;
    int n_bitmap_elts = ALIGN_UP(total_npages, 32) / 32;
    checked_write(fd, varyobj_page_touched_bits, n_bitmap_elts * sizeof (int));
    checked_write(fd, varyobj_pages, total_npages * sizeof (int));
#endif
    struct crash_thread_preamble thread_preamble;
    for_each_thread(th) {
        int ici = fixnum_value(read_TLS(FREE_INTERRUPT_CONTEXT_INDEX,th));
        os_context_t* threadcontext = nth_interrupt_context(0, th);
        uword_t sp;
        if (ici)
#ifdef LISP_FEATURE_C_STACK_IS_CONTROL_STACK
            sp = *os_context_register_addr(threadcontext, reg_SP);
#else
            sp = *os_context_register_addr(threadcontext, reg_CSP);
#endif
        else {
            if (th != get_sb_vm_thread()) {
              char msg[] = "No stackptr for crash dump\n";
              write(2, msg, sizeof msg-1);
              _exit(1);
            }
#ifdef LISP_FEATURE_C_STACK_IS_CONTROL_STACK
            sp = approx_stackptr_at_gc_start;
#else
            sp = access_control_stack_pointer(th);
#endif
        }
#ifdef LISP_FEATURE_C_STACK_IS_CONTROL_STACK
        int nbytes_control_stack =
            (char*)th->control_stack_end - (char*)sp; // grows downward
        int nbytes_binding_stack =
            (char*)th->binding_stack_pointer - (char*)th->binding_stack_start; // grows upward
#else
        int nbytes_control_stack = (char*)sp - (char*)th->control_stack_start; // grows upward
        int nbytes_binding_stack =
          (char*)get_binding_stack_pointer(th) - (char*)th->binding_stack_start;
#endif
        thread_preamble.address = (uword_t)th;
        thread_preamble.has_context = ici != 0; // boolean for have context or not
        thread_preamble.control_stack_nbytes = nbytes_control_stack;
        thread_preamble.binding_stack_nbytes = nbytes_binding_stack;
        // write the preamble
        checked_write(fd, &thread_preamble, sizeof thread_preamble);
        // write 0 or 1 contexts, control-stack, binding-stack, TLS
        if (ici) write(fd, threadcontext, preamble.sizeof_context);
#ifdef LISP_FEATURE_C_STACK_IS_CONTROL_STACK
        checked_write(fd, (char*)sp, nbytes_control_stack);
#else
        checked_write(fd, th->control_stack_start, nbytes_control_stack);
#endif
        checked_write(fd, th->binding_stack_start, nbytes_binding_stack);
        checked_write(fd, th, nbytes_tls);
    }
    checked_write(fd, "SB.Crash", 8); // trailing signature
    close(fd);
}
#endif

static cmd call_cmd, dump_cmd, print_cmd, quit_cmd, help_cmd;
static cmd flush_cmd, regs_cmd, exit_cmd;
static cmd print_context_cmd, pte_cmd, search_cmd;
static cmd backtrace_cmd, purify_cmd, catchers_cmd;
static cmd grab_sigs_cmd;
static cmd kill_cmd;
static cmd threads_cmd;

extern void gc_stop_the_world(), gc_start_the_world();
static void suspend_other_threads() {
#ifdef LISP_FEATURE_SB_THREAD
    gc_stop_the_world();
#endif
    // It might make sense for each thread's stop-for-gc handler to close its region
    // versus doing this loop
    struct thread *th;
    for_each_thread(th) { gc_close_thread_regions(th); }
    gc_close_collector_regions();
}
static void unsuspend_other_threads() {
#ifdef LISP_FEATURE_SB_THREAD
    gc_start_the_world();
#endif
}

static int save_cmd(char **ptr) {
#if defined LISP_FEATURE_X86_64 && defined LISP_FEATURE_SB_THREAD
    char *name  = parse_token(ptr);
    if (!name) {
        fprintf(stderr, "Need filename\n");
        return 1;
    }
    suspend_other_threads();
    save_gc_crashdump(name, (uword_t)__builtin_frame_address(0));
    unsuspend_other_threads();
#else
    fprintf(stderr, "Unimplemented\n");
#endif
    return 0;
}

static int threads_cmd(char __attribute__((unused)) **ptr) {
    struct thread* th;
    fprintf(stderr, "(thread*,pthread,sb-vm:thread)\n");
    void* pthread;
    for_each_thread(th) {
        memcpy(&pthread, &th->os_thread, N_WORD_BYTES);
        fprintf(stderr, "%p %p %p\n", th, pthread, (void*)th->lisp_thread);
    }
    return 0;
}
static int verify_cmd(char __attribute__((unused)) **ptr) {
    gencgc_verbose = 1;
    suspend_other_threads();
    verify_heap(0);
    unsuspend_other_threads();
    return 0;
}
static int gc_cmd(char **ptr) {
    int last_gen = 0;
    extern generation_index_t verify_gens;
    extern boolean pre_verify_gen_0;
    if (more_p(ptr)) parse_number(ptr, &last_gen);
    gencgc_verbose = 2;
    pre_verify_gen_0 = 1;
    verify_gens = 0;
    suspend_other_threads();
    collect_garbage(last_gen);
    unsuspend_other_threads();
    return 0;
}

static struct cmd {
    char *cmd, *help;
    int (*fn)(char **ptr);
} supported_cmds[] = {
    {"help", "Display this help information.", help_cmd},
    {"?", "(an alias for help)", help_cmd},
    {"backtrace", "Backtrace up to N frames.", backtrace_cmd},
    {"call", "Call FUNCTION with ARG1, ARG2, ...", call_cmd},
    {"catchers", "Print a list of all the active catchers.", catchers_cmd},
    {"context", "Print interrupt context number I.", print_context_cmd},
    {"dump", "Dump memory starting at ADDRESS for COUNT words.", dump_cmd},
    {"d", "(an alias for dump)", dump_cmd},
    {"exit", "Exit this instance of the monitor.", exit_cmd},
    {"flush", "Flush all temp variables.", flush_cmd},
    /* (Classic CMU CL had a "gc" command here, which seems like a
     * reasonable idea, but the code was stale (incompatible with
     * gencgc) so I just flushed it. -- WHN 20000814 */
    {"grab-signals", "Set the signal handlers to call LDB.", grab_sigs_cmd},
    {"kill", "Kill ourself with signal number N (useful if running under gdb)",
     kill_cmd},
    {"purify", "Purify. (Caveat purifier!)", purify_cmd},
    {"print", "Print object at ADDRESS.", print_cmd},
    {"p", "(an alias for print)", print_cmd},
    {"pte", "Page table entry for address", pte_cmd},
    {"quit", "Quit.", quit_cmd},
    {"regs", "Display current Lisp registers.", regs_cmd},
    {"search", "Search heap for object.", search_cmd},
    {"save", "Produce crashdump", save_cmd},
    {"threads", "List threads", threads_cmd},
    {"verify", "Check heap invariants", verify_cmd},
    {"gc", "Collect garbage", gc_cmd},
    {NULL, NULL, NULL}
};

static int
visible(unsigned char c)
{
    if (c < ' ' || c > '~')
        return ' ';
    else
        return c;
}

static boolean valid_widetag_p(unsigned char widetag) {
    // TODO: ensure that widetag is defined (not "unused") and is for a headered object
    // (i.e. is not CHARACTER_WIDETAG and not some other things)
    return other_immediate_lowtag_p(widetag);
}
static int NO_SANITIZE_MEMORY
dump_cmd(char **ptr)
{
    static char *lastaddr = 0;
    static int lastcount = 20;

    char *addr = lastaddr;
    int count = lastcount, displacement;
    int force = 0, decode = 0;

    if (more_p(ptr)) {
        while (1) {
            if (!strncmp(*ptr, "-f ", 3)) {
              force = 1;
              *ptr += 3;
            } else if (!strncmp(*ptr, "-d ", 3)) {
              decode = 1;
              *ptr += 3;
            } else break;
        }
        if (!parse_addr(ptr, !force, &addr)) return 0;

        if (more_p(ptr) && !parse_number(ptr, &count)) return 0;
    }

    if (count == 0) {
        printf("COUNT must be non-zero.\n");
        return 0;
    }

    lastcount = count;

    if (count > 0)
        displacement = N_WORD_BYTES;
    else {
        displacement = -N_WORD_BYTES;
        count = -count;
    }

    boolean aligned = ((uword_t)addr & LOWTAG_MASK) == 0;
    if (decode && (!aligned || displacement < 0)) {
        printf("Sorry, can only decode if aligned and stepping forward\n");
        decode = 0;
    }
    lispobj* next_object = decode ? (lispobj*)addr : 0;

    while (count-- > 0) {
        printf("%p: ", (os_vm_address_t) addr);
        if (force || gc_managed_addr_p((lispobj)addr)) {
            unsigned long *lptr = (unsigned long *)addr;
            unsigned char *cptr = (unsigned char *)addr;

#if N_WORD_BYTES == 8
            printf("0x%016lx | %c%c%c%c%c%c%c%c",
                   lptr[0],
                   visible(cptr[0]), visible(cptr[1]),
                   visible(cptr[2]), visible(cptr[3]),
                   visible(cptr[4]), visible(cptr[5]),
                   visible(cptr[6]), visible(cptr[7]));
#else
            unsigned short *sptr = (unsigned short *)addr;
            printf("0x%08lx   0x%04x 0x%04x   "
                   "0x%02x 0x%02x 0x%02x 0x%02x    "
                   "%c%c"
                   "%c%c",
                   lptr[0], sptr[0], sptr[1],
                   cptr[0], cptr[1], cptr[2], cptr[3],
                   visible(cptr[0]), visible(cptr[1]),
                   visible(cptr[2]), visible(cptr[3]));
#endif
#ifdef LISP_FEATURE_GENCGC
            if (aligned) {
                lispobj ptr = *(lispobj*)addr;
                int gen;
                if (is_lisp_pointer(ptr) && gc_managed_heap_space_p(ptr)
                    && (gen = gc_gen_of(ptr, 99)) != 99) // say that static is 99
                    if (gen != 99) printf(" | %d", gen);
            }
#endif
            if (decode && addr == (char*)next_object) {
                lispobj word = *(lispobj*)addr;
                // ensure validity of widetag because crashing with
                // "no size function" would be worse than doing nothing
                if (word != 0 && !is_lisp_pointer(word)
                    && valid_widetag_p(header_widetag(word))) {
                    printf(" %s", widetag_names[header_widetag(word)>>2]);
                    next_object += sizetab[header_widetag(word)](next_object);
                } else if (!is_header(word)) {
                    next_object += CONS_SIZE;
                } else { // disable decoder if weirdness observed
                    decode = 0;
                }
            }
            printf("\n");
        }
        else
            printf("invalid Lisp-level address\n");

        addr += displacement;
    }

    lastaddr = addr;
    return 0;
}

static int
print_cmd(char **ptr)
{
    lispobj obj;
    if (parse_lispobj(ptr, &obj)) print(obj);
    return 0;
}

static int
pte_cmd(char **ptr)
{
    extern void gc_show_pte(lispobj);
    lispobj obj;
    if (parse_lispobj(ptr, &obj)) gc_show_pte(obj);
    return 0;
}

static int
kill_cmd(char **ptr)
{
#ifndef LISP_FEATURE_WIN32
    int sig;
    if (parse_number(ptr, &sig)) kill(getpid(), sig);
#endif
    return 0;
}

static int
regs_cmd(char __attribute__((unused)) **ptr)
{
    struct thread __attribute__((unused)) *thread=get_sb_vm_thread();

    printf("CSP\t=\t%p   ", access_control_stack_pointer(thread));
#if !defined(LISP_FEATURE_X86) && !defined(LISP_FEATURE_X86_64)
    printf("CFP\t=\t%p   ", access_control_frame_pointer(thread));
#endif

#ifdef reg_BSP
    printf("BSP\t=\t%p\n", get_binding_stack_pointer(thread));
#else
    /* printf("BSP\t=\t%p\n", (void*)SymbolValue(BINDING_STACK_POINTER)); */
    printf("\n");
#endif

#ifdef LISP_FEATURE_GENCGC
    /* printf("DYNAMIC\t=\t%p\n", (void*)DYNAMIC_SPACE_START); */
#else
    printf("STATIC\t=\t%p   ", static_space_free_pointer);
    printf("RDONLY\t=\t%p   ", read_only_space_free_pointer);
    printf("DYNAMIC\t=\t%p\n", (void*)current_dynamic_space);
#endif

#ifndef LISP_FEATURE_GENCGC
    printf("TRIGGER\t=\t%p\n", (void*)current_auto_gc_trigger);
#endif
    return 0;
}

static int
call_cmd(char **ptr)
{
    lispobj thing;
    parse_lispobj(ptr, &thing);
    lispobj function, args[3];
    lispobj result = NIL;

    int numargs;

    if (lowtag_of(thing) == OTHER_POINTER_LOWTAG) {
        lispobj *obj = native_pointer(thing);
        switch (widetag_of(obj)) {
          case SYMBOL_WIDETAG:
              function = symbol_function((struct symbol*)obj);
              if (function == NIL) {
                  printf("Symbol 0x%08lx is undefined.\n", (long unsigned)thing);
                  return 0;
              }
              break;
          case FDEFN_WIDETAG:
              function = FDEFN(thing)->fun;
              if (function == NIL) {
                  printf("Fdefn 0x%08lx is undefined.\n", (long unsigned)thing);
                  return 0;
              }
              break;
          default:
              printf("0x%08lx is not a function pointer, symbol, "
                     "or fdefn object.\n",
                     (long unsigned)thing);
              return 0;
        }
    }
    else if (lowtag_of(thing) != FUN_POINTER_LOWTAG) {
        printf("0x%08lx is not a function pointer, symbol, or fdefn object.\n",
               (long unsigned)thing);
        return 0;
    }
    else
        function = thing;

    numargs = 0;
    while (more_p(ptr)) {
        if (numargs >= 3) {
            printf("too many arguments (no more than 3 supported)\n");
            return 0;
        }
        parse_lispobj(ptr, &args[numargs++]);
    }

    switch (numargs) {
      case 0:
          result = funcall0(function);
          break;
      case 1:
          result = funcall1(function, args[0]);
          break;
      case 2:
          result = funcall2(function, args[0], args[1]);
          break;
      case 3:
          result = funcall3(function, args[0], args[1], args[2]);
          break;
      default:
          lose("unsupported arg count made it past validity check?!");
    }

    print(result);
    return 0;
}

static int
flush_cmd(char __attribute__((unused)) **ptr)
{
    flush_vars();
    return 0;
}

static int
quit_cmd(char __attribute__((unused)) **ptr)
{
    char buf[10];

    printf("Really quit? [y] ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), ldb_in)) {
        if (buf[0] == 'y' || buf[0] == 'Y' || buf[0] == '\n')
            exit(1);
    } else {
        printf("\nUnable to read response, assuming y.\n");
        exit(1);
    }
    return 0;
}

static int
help_cmd(char __attribute__((unused)) **ptr)
{
    struct cmd *cmd;

    for (cmd = supported_cmds; cmd->cmd != NULL; cmd++)
        if (cmd->help != NULL)
            printf("%s\t%s\n", cmd->cmd, cmd->help);
    return 0;
}

static int
exit_cmd(char __attribute__((unused)) **ptr)
{
    return 1; // 'done' flag
}

static int
purify_cmd(char __attribute__((unused)) **ptr)
{
    purify(NIL, NIL);
    return 0;
}

static void
print_context(os_context_t *context)
{
    int i;

    for (i = 0; i < NREGS; i++) {
        printf("%s:\t", lisp_register_names[i]);
        brief_print((lispobj)(*os_context_register_addr(context,i)));

    }
#ifndef REG_PC
    printf("PC:\t\t  0x%08lx\n", (unsigned long)os_context_pc(context));
#endif
}

static int
print_context_cmd(char **ptr)
{
    int free_ici;
    struct thread *thread=get_sb_vm_thread();

    free_ici = fixnum_value(read_TLS(FREE_INTERRUPT_CONTEXT_INDEX,thread));

    if (more_p(ptr)) {
        int index;

        if (!parse_number(ptr, &index)) return 0;

        if ((index >= 0) && (index < free_ici)) {
            printf("There are %d interrupt contexts.\n", free_ici);
            printf("printing context %d\n", index);
            print_context(nth_interrupt_context(index, thread));
        } else {
            printf("There aren't that many/few contexts.\n");
            printf("There are %d interrupt contexts.\n", free_ici);
        }
    } else {
        if (free_ici == 0)
            printf("There are no interrupt contexts!\n");
        else {
            printf("There are %d interrupt contexts.\n", free_ici);
            printf("printing context %d\n", free_ici - 1);
            print_context(nth_interrupt_context(free_ici - 1, thread));
        }
    }
    return 0;
}

static int
backtrace_cmd(char **ptr)
{
    void lisp_backtrace(int frames);
    int n;

    if (more_p(ptr)) {
        if (!parse_number(ptr, &n)) return 0;
    } else
        n = 100;

    printf("Backtrace:\n");
    lisp_backtrace(n);
    return 0;
}

static int search_cmd(char **ptr)
{
    char *addr;
    if (!parse_addr(ptr, 1, &addr)) return 0;
    lispobj *obj = search_all_gc_spaces((void*)addr);
    if(obj)
        printf("#x%"OBJ_FMTX"\n", compute_lispobj(obj));
    else
        printf("Not found\n");
    return 0;
}

static int
catchers_cmd(char __attribute__((unused)) **ptr)
{
    struct catch_block *catch = (struct catch_block *)
        read_TLS(CURRENT_CATCH_BLOCK, get_sb_vm_thread());

    if (catch == NULL)
        printf("There are no active catchers!\n");
    else {
        while (catch != NULL) {
            printf("%p:\n\tuwp  : %p\n\tfp   : %p\n\t"
                   "code : %p\n\tentry: %p\n\ttag: ",
                   catch,
                   catch->uwp,
                   catch->cfp,
#if defined(LISP_FEATURE_X86) || defined(LISP_FEATURE_X86_64) || defined(LISP_FEATURE_ARM64)
                   component_ptr_from_pc((void*)catch->entry_pc),
#else
                   (void*)catch->code,
#endif
                   (void*)(catch->entry_pc));
            brief_print((lispobj)catch->tag);
            catch = catch->previous_catch;
        }
    }
    return 0;
}

/* SIGINT handler that invokes the monitor (for when Lisp isn't up to it) */
static void
sigint_handler(int __attribute__((unused)) signal,
               siginfo_t __attribute__((unused)) *info,
               void *context)
{
    extern void ldb_monitor();
    fprintf(stderr, "\nSIGINT hit at %p\n", (void*)os_context_pc(context));
    ldb_monitor();
    fprintf(stderr, "Returning to lisp (if you're lucky).\n");
}

static int
grab_sigs_cmd(char __attribute__((unused)) **ptr)
{
#ifdef LISP_FEATURE_WIN32
    fprintf(stderr, "sorry no can do\n"); fflush(stderr);
#else
    printf("Grabbing SIGINT.\n");
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sigint_handler;
    sigaction(SIGINT, &sa, 0);
#endif
    return 0;
}

extern boolean gc_active_p;
extern FILE *gc_activitylog_file;
void
ldb_monitor(void)
{
    struct cmd *cmd, *found;
    char buf[256];
    char *line, *ptr, *token;
    int ambig;

    printf("Welcome to LDB, a low-level debugger for the Lisp runtime environment.\n");
    if (gc_active_p) printf("(GC in progress)\n");
    if (gc_activitylog_file) fflush(gc_activitylog_file);
    if (!ldb_in) {
#ifndef LISP_FEATURE_WIN32
        ldb_in = fopen("/dev/tty","r+");
        if (ldb_in == NULL) {
            perror("Error opening /dev/tty");
            ldb_in = stdin;
        }
#else
        ldb_in = stdin;
#endif
        ldb_in_fd = fileno(ldb_in);
    }

    while (1) {
        printf("ldb> ");
        fflush(stdout);
        line = fgets(buf, sizeof(buf), ldb_in);
        if (line == NULL) {
            exit(1);
        }
        ptr = line;
        if ((token = parse_token(&ptr)) == NULL)
            continue;
        ambig = 0;
        found = NULL;
        for (cmd = supported_cmds; cmd->cmd != NULL; cmd++) {
            if (strcmp(token, cmd->cmd) == 0) {
                found = cmd;
                ambig = 0;
                break;
            }
            else if (strncmp(token, cmd->cmd, strlen(token)) == 0) {
                if (found)
                    ambig = 1;
                else
                    found = cmd;
            }
        }
        if (ambig)
            printf("``%s'' is ambiguous.\n", token);
        else if (found == NULL)
            printf("unknown command: ``%s''\n", token);
        else {
            reset_printer();
            int done = (*found->fn)(&ptr);
            if (done) return;
        }
    }
}

/* what we do when things go badly wrong at a low level */
void
monitor_or_something()
{
    ldb_monitor();
}

#ifdef STANDALONE_LDB
void gc_stop_the_world() { } // do nothing
void gc_start_the_world() { } // do nothing
#include <errno.h>
#include "core.h"
#include "gencgc-private.h"
struct lisp_startup_options lisp_startup_options;

void unwind_binding_stack() { lose("Can't unwind binding stack"); }
FILE *prepare_to_save(char *filename, boolean prepend_runtime, void **runtime_bytes,
                      size_t *runtime_size) {
    lose("Can't prepare_to_save");
}
boolean save_runtime_to_filehandle(FILE *output, void *runtime, size_t runtime_size,
                                   int application_type) {
    lose("Can't save_runtime_to_filehandle");
}
boolean save_to_filehandle(FILE *file, char *filename, lispobj init_function,
                           boolean make_executable,
                           boolean save_runtime_options,
                           int core_compression_level) {
    lose("Can't save_to_filehandle");
}

static size_t checked_read(int fd, void* buf, size_t n)
{
    size_t result = read(fd, buf, n);
    if (result != n) { lose("read failed, errno=%d", errno); }
    return result;
}

char *pagetypedesc(int type)
{
    static char what[4];
    switch (type) {
    case PAGE_TYPE_CODE: return "code";
    case PAGE_TYPE_BOXED: return "boxed";
    case PAGE_TYPE_UNBOXED: return "raw";
    case PAGE_TYPE_MIXED: return "mixed";
    default: snprintf(what, 4, "%d", type); return what;
    }
}

extern void gc_allocate_ptes();
extern void recompute_gen_bytes_allocated();
extern void print_generation_stats();
extern struct thread *alloc_thread_struct(void*,lispobj);

int load_gc_crashdump(char* pathname)
{
    int fd;
    os_context_t *contexts[10], *context;
    struct thread* threads = 0;
    struct thread dummy;
    fd = open(pathname, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "can't open %s\n", pathname);
        exit(1);
    }
    struct crash_preamble preamble;
    struct crash_thread_preamble thread_preamble;
    checked_read(fd, &preamble, sizeof preamble);
    printf("static=%lx nbytes=%x\n", preamble.static_start, (int)preamble.static_nbytes);
    printf("heap_start=%lx npages=%d\n", preamble.dynspace_start, (int)preamble.dynspace_npages);
    // pin_dynspace_code is for display only. It gets recomputed as the
    // logical OR of all threads' values of *GC-PIN-CODE-PAGES*.
    printf("sprof_enabled=%d pin_dynspace_code=%d packages=%p\n",
           preamble.sprof_enabled, preamble.pin_dynspace_code,
           (void*)preamble.lisp_package_vector);
    lisp_package_vector = preamble.lisp_package_vector;
    sb_sprof_enabled = preamble.sprof_enabled;
    if (preamble.signature != CRASH_PREAMBLE_SIGNATURE)
        lose("Can't load crashdump: bad header (have %lx, expect %lx)",
             preamble.signature, (long)CRASH_PREAMBLE_SIGNATURE);
    if (preamble.card_size != GENCGC_CARD_BYTES)
        lose("Can't load crashdump: memory parameters differ");
    gc_card_table_nbits = preamble.card_table_nbits;
    gc_allocate_ptes();
    next_free_page = preamble.dynspace_npages;
    checked_read(fd, (char*)STATIC_SPACE_START, preamble.static_nbytes);
    static_space_free_pointer = (lispobj*)(STATIC_SPACE_START + preamble.static_nbytes);
    DYNAMIC_SPACE_START = preamble.dynspace_start;
    long dynspace_nbytes = preamble.dynspace_npages * GENCGC_PAGE_BYTES;
    char* dynspace = os_validate(0, (char*)preamble.dynspace_start,
                                 DEFAULT_DYNAMIC_SPACE_SIZE, 0, 0);
    if (dynspace != (char*)preamble.dynspace_start)
        lose("Didn't map dynamic space where expected: %p vs %p",
             dynspace, (char*)preamble.dynspace_start);
    checked_read(fd, (char*)DYNAMIC_SPACE_START, dynspace_nbytes);
    fprintf(stderr, "snapshot: %ld pages in use (%ld bytes)\n",
            next_free_page, dynspace_nbytes);
    checked_read(fd, page_table, sizeof (struct page) * next_free_page);
    recompute_gen_bytes_allocated();
    checked_read(fd, gc_card_mark, 1+gc_card_table_mask);
    print_generation_stats();
#ifdef LISP_FEATURE_IMMOBILE_SPACE
    extern void gc_init_immobile(),
                calc_immobile_space_bounds(),
                write_protect_immobile_space();
    gc_assert(preamble.fixedobj_size == FIXEDOBJ_SPACE_SIZE);
    gc_assert(preamble.varyobj_size == VARYOBJ_SPACE_SIZE);
    FIXEDOBJ_SPACE_START = preamble.fixedobj_start;
    VARYOBJ_SPACE_START = preamble.varyobj_start;
    fixedobj_free_pointer = (lispobj*)preamble.fixedobj_free_pointer;
    varyobj_free_pointer = (lispobj*)preamble.varyobj_free_pointer;
    os_validate(0, (char*)FIXEDOBJ_SPACE_START, FIXEDOBJ_SPACE_SIZE, 0, 0);
    os_validate(0, (char*)VARYOBJ_SPACE_START, VARYOBJ_SPACE_SIZE, 0, 0);
    gc_init_immobile(); // allocate the page tables
    calc_immobile_space_bounds();
    // Read fixedobj space
    int usage = (uword_t)fixedobj_free_pointer - FIXEDOBJ_SPACE_START;
    checked_read(fd, (char*)FIXEDOBJ_SPACE_START, usage);
    // Always read the whole page table regardless of the current space usage
    int total_npages = FIXEDOBJ_SPACE_SIZE / IMMOBILE_CARD_BYTES;
    checked_read(fd, fixedobj_pages, total_npages * sizeof sizeof(struct fixedobj_page));
    // Read varyobj space
    usage = (uword_t)varyobj_free_pointer - VARYOBJ_SPACE_START;
    checked_read(fd, (char*)VARYOBJ_SPACE_START, usage);
    total_npages = VARYOBJ_SPACE_SIZE / IMMOBILE_CARD_BYTES;
    int n_bitmap_elts = ALIGN_UP(total_npages, 32) / 32;
    checked_read(fd, varyobj_page_touched_bits, n_bitmap_elts * sizeof (int));
    checked_read(fd, varyobj_pages, total_npages * sizeof (int));
    write_protect_immobile_space();
#endif
    fprintf(stderr, "%d threads:\n", (int)preamble.nthreads);
    int i;
    for(i=0; i<(int)preamble.nthreads; ++i) {
        struct thread* th = alloc_thread_struct(0, 0);
        // Push it on the front
        th->prev = 0;
        th->next = threads;
        if (threads) threads->prev = th;
        threads = th;
        checked_read(fd, &thread_preamble, sizeof thread_preamble);
        uword_t* stackptr = (uword_t*)((char*)th->control_stack_end - thread_preamble.control_stack_nbytes);
        context = contexts[i] = malloc(preamble.sizeof_context);
        nth_interrupt_context(0, th) = context;
        if (thread_preamble.has_context) {
            checked_read(fd, context, preamble.sizeof_context);
        }
#ifdef LISP_FEATURE_C_STACK_IS_CONTROL_STACK
        *os_context_sp_addr(context) = (uword_t)stackptr;
#else
        *os_context_register_addr(context, reg_CSP) = (uword_t)stackptr;
#endif
        checked_read(fd, stackptr, thread_preamble.control_stack_nbytes);
        checked_read(fd, th->binding_stack_start, thread_preamble.binding_stack_nbytes);
        // Skip over the initial words of the thread structure that was saved
        // in the file, so that binding_stack_start remains as is in the
        // newly allocated structure. The last word is the only one we want to keep.
        int skip = sizeof dummy-N_WORD_BYTES;
        checked_read(fd, &dummy, skip);
        checked_read(fd, &th->lisp_thread, preamble.tls_size-skip);
        write_TLS(FREE_INTERRUPT_CONTEXT_INDEX, make_fixnum(1), th);
        struct thread_instance* instance = (void*)(th->lisp_thread - INSTANCE_POINTER_LOWTAG);
        lispobj name = instance->name;
        char* cname = (gc_managed_addr_p(name) &&
                       widetag_of(native_pointer(name)) == SIMPLE_BASE_STRING_WIDETAG)
                      ? (char*)name+1 : 0;
        fprintf(stderr, "thread @ %p originally %p, %d bind_stk words, %d val_stk words '%s'\n",
                th, (void*)thread_preamble.address,
                (int)(thread_preamble.binding_stack_nbytes>>WORD_SHIFT),
                (int)(thread_preamble.control_stack_nbytes>>WORD_SHIFT),
                cname);
        // Scan thread stack looking for words which could be valid pointers,
        // but don't find an object when the heap is scanned.
        // Realizing that failure to find isn't necessarily an error,
        // there's nothing that we can do except show some information.
        int nwords = thread_preamble.control_stack_nbytes>>WORD_SHIFT, wordindex;
        int n_definitely_valid = 0, n_dangling = 0;
        for (wordindex = 0; wordindex < nwords; ++wordindex) {
            lispobj word = stackptr[wordindex];
            if (DYNAMIC_SPACE_START <= word && word < DYNAMIC_SPACE_START + dynamic_space_size
                && (is_lisp_pointer(word) ||
                    is_code(page_table[find_page_index((void*)word)].type))) {
                lispobj* found = search_dynamic_space((void*)word);
                if (found) {
                    __attribute__((unused)) page_index_t ind = find_page_index(found);
#if 0
                    fprintf(stderr, "   sp[%5d] = %"OBJ_FMTX" -> %p (g%d,%s)\n",
                            wordindex, word, found,
                            page_table[ind].gen, pagetypedesc(page_table[ind].type));
#endif
                    ++n_definitely_valid;
                } else {
                    fprintf(stderr, " ! sp[%5d] = %"OBJ_FMTX" (not found)\n",
                            wordindex, word);
                    ++n_dangling;
                }
            }
        }
        fprintf(stderr, "%d valid pointers", n_definitely_valid);
        if (n_dangling) fprintf(stderr, " (%d dangling)", n_dangling);
        putc('\n', stderr);
    }
    char signature[8];
    checked_read(fd, signature, 8);
    gc_assert(!strncmp(signature, "SB.Crash", 8));
    gc_assert(read(fd, signature, 1) == 0);
    close(fd);
    all_threads = threads;
    return 0;
}

int main(int argc, char *argv[], char **envp)
{
    extern void calc_asm_routine_bounds();
    if (argc != 2) {
        fprintf(stderr, "Usage: ldb crashdump\n");
        return 1;
    }
    boolean have_hardwired_spaces = os_preinit(argv, envp);
    allocate_lisp_dynamic_space(have_hardwired_spaces);
    gc_init();
    load_gc_crashdump(argv[1]);
    calc_asm_routine_bounds();
    gencgc_verbose = 1;
    ldb_monitor();
}
#endif
