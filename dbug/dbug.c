/******************************************************************************
 *                                                                            *
 *                                 N O T I C E                                *
 *                                                                            *
 *                    Copyright Abandoned, 1987, Fred Fish                    *
 *                                                                            *
 *                                                                            *
 *      This previously copyrighted work has been placed into the  public     *
 *      domain  by  the  author  and  may be freely used for any purpose,     *
 *      private or commercial.                                                *
 *                                                                            *
 *      Because of the number of inquiries I was receiving about the  use     *
 *      of this product in commercially developed works I have decided to     *
 *      simply make it public domain to further its unrestricted use.   I     *
 *      specifically  would  be  most happy to see this material become a     *
 *      part of the standard Unix distributions by AT&T and the  Berkeley     *
 *      Computer  Science  Research Group, and a standard part of the GNU     *
 *      system from the Free Software Foundation.                             *
 *                                                                            *
 *      I would appreciate it, as a courtesy, if this notice is  left  in     *
 *      all copies and derivative works.  Thank you.                          *
 *                                                                            *
 *      The author makes no warranty of any kind  with  respect  to  this     *
 *      product  and  explicitly disclaims any implied warranties of mer-     *
 *      chantability or fitness for any particular purpose.                   *
 *                                                                            *
 ******************************************************************************
 */

/*
 *  FILE
 *
 *      dbug.c   runtime support routines for dbug package
 *
 *  SCCS
 *
 *      @(#)dbug.c      1.25    7/25/89
 *
 *  DESCRIPTION
 *
 *      These are the runtime support routines for the dbug package.
 *      The dbug package has two main components; the user include
 *      file containing various macro definitions, and the runtime
 *      support routines which are called from the macro expansions.
 *
 *      Externally visible functions in the runtime support module
 *      use the naming convention pattern "_db_xx...xx_", thus
 *      they are unlikely to collide with user defined function names.
 *
 *  AUTHOR(S)
 *
 *      Fred Fish               (base code)
 *      Enhanced Software Technologies, Tempe, AZ
 *      asuvax!mcdphx!estinc!fnf
 *
 *      Michael Widenius:
 *        DBUG_DUMP       - To dump a block of memory.
 *        PUSH_FLAG "O"   - To be used instead of "o" if we
 *                          want flushing after each write
 *        PUSH_FLAG "A"   - as 'O', but we will append to the out file instead
 *                          of creating a new one.
 *        Check of malloc on entry/exit (option "S")
 *
 *      Sergei Golubchik:
 *        DBUG_EXECUTE_IF
 *        incremental mode (-#+t:-d,info ...)
 *        DBUG_SET, _db_explain_
 *        thread-local settings
 *        negative lists (-#-d,info => everything but "info")
 *
 *        function/ syntax
 *        (the logic is - think of a call stack as of a path.
 *        "function" means only this function, "function/" means the hierarchy.
 *        in the future, filters like function1/function2 could be supported.
 *        following this logic glob(7) wildcards are supported.)
 *
 */

/*
  We can't have SAFE_MUTEX defined here as this will cause recursion
  in pthread_mutex_lock
*/

#include <my_global.h>
#undef SAFE_MUTEX
#include <m_string.h>
#include <errno.h>

#ifndef DBUG_OFF

#ifdef HAVE_FNMATCH_H
#include <fnmatch.h>
#else
#define fnmatch(A,B,C) strcmp(A,B)
#endif

#if defined(__WIN__)
#include <process.h>
#endif

/*
 *            Manifest constants which may be "tuned" if desired.
 */

#define PRINTBUF              1024    /* Print buffer size */
#define INDENT                2       /* Indentation per trace level */
#define MAXDEPTH              200     /* Maximum trace depth default */

/*
 *      The following flags are used to determine which
 *      capabilities the user has enabled with the settings
 *      push macro.
 *
 *      TRACE_ON is also used in _db_stack_frame_->level
 *      (until we add flags to _db_stack_frame_, increasing it by 4 bytes)
 */

#define DEBUG_ON        (1U <<  1) /* Debug enabled */
#define FILE_ON         (1U <<  2) /* File name print enabled */
#define LINE_ON         (1U <<  3) /* Line number print enabled */
#define DEPTH_ON        (1U <<  4) /* Function nest level print enabled */
#define PROCESS_ON      (1U <<  5) /* Process name print enabled */
#define NUMBER_ON       (1U <<  6) /* Number each line of output */
#define PID_ON          (1U <<  8) /* Identify each line with process id */
#define TIMESTAMP_ON    (1U <<  9) /* timestamp every line of output */
#define FLUSH_ON_WRITE  (1U << 10) /* Flush on every write */
#define OPEN_APPEND     (1U << 11) /* Open for append      */
#define SANITY_CHECK_ON (1U << 12) /* Check memory on every DBUG_ENTER/RETURN */
#define TRACE_ON        (1U << 31) /* Trace enabled. MUST be the highest bit!*/

#define TRACING (cs->stack->flags & TRACE_ON)
#define DEBUGGING (cs->stack->flags & DEBUG_ON)

/*
 *      Typedefs to make things more obvious.
 */

#define BOOLEAN my_bool

/*
 *      Externally supplied functions.
 */

#ifndef HAVE_PERROR
static void perror(char *s)
{
  if (s && *s != '\0')
    (void) fprintf(stderr, "%s: ", s);
  (void) fprintf(stderr, "<unknown system error>\n");
}
#endif

/*
 *      The user may specify a list of functions to trace or
 *      debug.  These lists are kept in a linear linked list,
 *      a very simple implementation.
 */

struct link {
    struct link *next_link;   /* Pointer to the next link */
    char   flags;
    char   str[1];            /* Pointer to link's contents */
};

/* flags for struct link and return flags of InList */
#define SUBDIR          1 /* this MUST be 1 */
#define INCLUDE         2
#define EXCLUDE         4
/* this is not a struct link flag, but only a return flags of InList */
#define MATCHED     65536
#define NOT_MATCHED     0

/*
 * Debugging settings can be shared between threads.
 * But FILE* streams cannot normally be shared - what if
 * one thread closes a stream, while another thread still uses it?
 * As a workaround, we have shared FILE pointers with reference counters
 */
typedef struct {
  FILE *file;
  uint used;
} sFILE;

sFILE shared_stdout = { 0, 1 << 30 }, *sstdout = &shared_stdout;
sFILE shared_stderr = { 0, 1 << 30 }, *sstderr = &shared_stderr;

/*
 *      Debugging settings can be pushed or popped off of a
 *      stack which is implemented as a linked list.  Note
 *      that the head of the list is the current settings and the
 *      stack is pushed by adding a new settings to the head of the
 *      list or popped by removing the first link.
 *
 *      Note: if out_file is NULL, the other fields are not initialized at all!
 */

struct settings {
  uint flags;                   /* Current settings flags               */
  uint maxdepth;                /* Current maximum trace depth          */
  uint delay;                   /* Delay after each output line         */
  uint sub_level;               /* Sub this from code_state->level      */
  sFILE *out_file;              /* Current output stream                */
  char name[FN_REFLEN];         /* Name of output file                  */
  struct link *functions;       /* List of functions                    */
  struct link *keywords;        /* List of debug keywords               */
  struct link *processes;       /* List of process names                */
  struct settings *next;        /* Next settings in the list            */
};

#define is_shared(S, V) ((S)->next && (S)->next->V == (S)->V)

/*
 *      Local variables not seen by user.
 */


static BOOLEAN init_done= FALSE; /* Set to TRUE when initialization done */
static struct settings init_settings;
static const char *db_process= 0;/* Pointer to process name; argv[0] */
my_bool _dbug_on_= TRUE;	 /* FALSE if no debugging at all */

typedef struct _db_code_state_ {
  const char *process;          /* Pointer to process name; usually argv[0] */
  const char *func;             /* Name of current user function            */
  const char *file;             /* Name of current user file                */
  struct _db_stack_frame_ *framep; /* Pointer to current frame              */
  struct settings *stack;       /* debugging settings                       */
  int lineno;                   /* Current debugger output line number      */
  uint level;                   /* Current function nesting level           */

/*
 *      The following variables are used to hold the state information
 *      between the call to _db_pargs_() and _db_doprnt_(), during
 *      expansion of the DBUG_PRINT macro.  This is the only macro
 *      that currently uses these variables.
 *
 *      These variables are currently used only by _db_pargs_() and
 *      _db_doprnt_().
 */

  uint u_line;                  /* User source code line number */
  int  locked;                  /* If locked with _db_lock_file_ */
  const char *u_keyword;        /* Keyword for current macro */
} CODE_STATE;

/*
  The test below is so we could call functions with DBUG_ENTER before
  my_thread_init().
*/
#define get_code_state_if_not_set_or_return if (!cs && !((cs=code_state()))) return
#define get_code_state_or_return if (!((cs=code_state()))) return

        /* Handling lists */
#define ListAdd(A,B,C) ListAddDel(A,B,C,INCLUDE)
#define ListDel(A,B,C) ListAddDel(A,B,C,EXCLUDE)
static struct link *ListAddDel(struct link *, const char *, const char *, int);
static struct link *ListCopy(struct link *);
static int InList(struct link *linkp,const char *cp,int exact_match);
static uint ListFlags(struct link *linkp);
static void FreeList(struct link *linkp);

        /* OpenClose debug output stream */
static void DBUGOpenFile(CODE_STATE *,const char *, const char *, int);
static void DBUGCloseFile(CODE_STATE *cs, sFILE *new_value);
        /* Push current debug settings */
static void PushState(CODE_STATE *cs);
	/* Free memory associated with debug state. */
static void FreeState (CODE_STATE *cs, int free_state);
        /* Test for tracing enabled */
static int DoTrace(CODE_STATE *cs);
static int default_my_dbug_sanity(void);

int (*dbug_sanity)(void)= default_my_dbug_sanity;


/*
  return values of DoTrace.
  Can also be used as bitmask: ret & DO_TRACE
*/
#define DO_TRACE        1
#define DONT_TRACE      2
#define ENABLE_TRACE    3
#define DISABLE_TRACE   4

        /* Test to see if file is writable */
#if defined(HAVE_ACCESS)
static BOOLEAN Writable(const char *pathname);
#endif

static void DoPrefix(CODE_STATE *cs, uint line);

static char *DbugMalloc(size_t size);
static const char *BaseName(const char *pathname);
static void Indent(CODE_STATE *cs, int indent);
static void DbugFlush(CODE_STATE *);
static void DbugExit(const char *why);
static const char *DbugStrTok(const char *s);
static void DbugVfprintf(FILE *stream, const char* format, va_list args);

/*
 *      Miscellaneous printf format strings.
 */

#define ERR_MISSING_RETURN "missing DBUG_RETURN or DBUG_VOID_RETURN macro in function \"%s\"\n"
#define ERR_OPEN "%s: can't open debug output stream \"%s\": "
#define ERR_CLOSE "%s: can't close debug file: "
#define ERR_ABORT "%s: debugger aborting because %s\n"

/*
 *      Macros and defines for testing file accessibility under UNIX and MSDOS.
 */

#undef EXISTS
#if !defined(HAVE_ACCESS)
#define EXISTS(pathname) (FALSE)        /* Assume no existence */
#define Writable(name) (TRUE)
#else
#define EXISTS(pathname)         (access(pathname, F_OK) == 0)
#define WRITABLE(pathname)       (access(pathname, W_OK) == 0)
#endif

/*
** Macros to allow dbugging with threads
*/

#include <my_pthread.h>
/*
** Protects writing to all file descriptors, init_settings.keywords
** pointer and it's pointee - a linked list with keywords.
*/
static pthread_mutex_t THR_LOCK_dbug;

static void LockMutex(CODE_STATE *cs)
{
  if (!cs->locked)
    pthread_mutex_lock(&THR_LOCK_dbug);
  cs->locked++;
}
static void UnlockMutex(CODE_STATE *cs)
{
  --cs->locked;
  assert(cs->locked >= 0);
  if (cs->locked == 0)
    pthread_mutex_unlock(&THR_LOCK_dbug);
}
static void LockIfInitSettings(CODE_STATE *cs)
{
  if (cs->stack == &init_settings)
    LockMutex(cs);
}
static void UnlockIfInitSettings(CODE_STATE *cs)
{
  if (cs->stack == &init_settings)
    UnlockMutex(cs);
}

static CODE_STATE *code_state(void)
{
  CODE_STATE *cs, **cs_ptr;

  /*
    _dbug_on_ is reset if we don't plan to use any debug commands at all and
    we want to run on maximum speed
   */
  if (!_dbug_on_)
    return 0;

  if (!init_done)
  {
    init_done=TRUE;
    sstdout->file= stdout;
    sstderr->file= stderr;
    pthread_mutex_init(&THR_LOCK_dbug, NULL);
    bzero(&init_settings, sizeof(init_settings));
    init_settings.out_file= sstderr;
    init_settings.flags=OPEN_APPEND;
  }

  if (!(cs_ptr= (CODE_STATE**) my_thread_var_dbug()))
    return 0;                                   /* Thread not initialised */
  if (!(cs= *cs_ptr))
  {
    cs=(CODE_STATE*) DbugMalloc(sizeof(*cs));
    bzero((uchar*) cs,sizeof(*cs));
    cs->process= db_process ? db_process : "dbug";
    cs->func= "?func";
    cs->file= "?file";
    cs->stack=&init_settings;
    *cs_ptr= cs;
  }
  return cs;
}

void
dbug_swap_code_state(void **code_state_store)
{
  CODE_STATE *cs, **cs_ptr;

  if (!(cs_ptr= (CODE_STATE**) my_thread_var_dbug()))
    return;
  cs= *cs_ptr;
  *cs_ptr= *code_state_store;
  *code_state_store= cs;
}

void dbug_free_code_state(void **code_state_store)
{
  if (*code_state_store)
  {
    free(*code_state_store);
    *code_state_store= NULL;
  }
}

/*
 *      Translate some calls among different systems.
 */

#ifdef HAVE_SLEEP
/* sleep() wants seconds */
#define Delay(A) sleep(((uint) A)/10)
#else
#define Delay(A) (0)
#endif

/*
 *  FUNCTION
 *
 *      _db_process_       give the name to the current process/thread
 *
 *  SYNOPSIS
 *
 *      VOID _process_(name)
 *      char *name;
 *
 */

void _db_process_(const char *name)
{
  CODE_STATE *cs;

  if (!db_process)
    db_process= name;

  get_code_state_or_return;
  cs->process= name;
}

/*
 *  FUNCTION
 *
 *      DbugParse  parse control string and set current debugger settings
 *
 *  DESCRIPTION
 *
 *      Given pointer to a debug control string in "control",
 *      parses the control string, and sets
 *      up a current debug settings.
 *
 *      The debug control string is a sequence of colon separated fields
 *      as follows:
 *
 *              [+]<field_1>:<field_2>:...:<field_N>
 *
 *      Each field consists of a mandatory flag character followed by
 *      an optional "," and comma separated list of modifiers:
 *
 *              [sign]flag[,modifier,modifier,...,modifier]
 *
 *      See the manual for the list of supported signs, flags, and modifiers
 *
 *      For convenience, any leading "-#" is stripped off.
 *
 *  RETURN
 *      1 - a list of functions ("f" flag) was possibly changed
 *      0 - a list of functions was not changed
 */

static int DbugParse(CODE_STATE *cs, const char *control)
{
  const char *end;
  int rel, f_used=0;
  struct settings *stack;

  stack= cs->stack;

  if (control[0] == '-' && control[1] == '#')
    control+=2;

  rel= control[0] == '+' || control[0] == '-';
  if ((!rel || (!stack->out_file && !stack->next)))
  {
    LockIfInitSettings(cs);
    FreeState(cs, 0);
    stack->flags= 0;
    stack->delay= 0;
    stack->maxdepth= 0;
    stack->sub_level= 0;
    stack->out_file= sstderr;
    stack->functions= NULL;
    stack->keywords= NULL;
    stack->processes= NULL;
    UnlockIfInitSettings(cs);
  }
  else if (!stack->out_file)
  {
    int error_code = 0;
    stack->flags= stack->next->flags;
    stack->delay= stack->next->delay;
    stack->maxdepth= stack->next->maxdepth;
    stack->sub_level= stack->next->sub_level;
    safe_strcpy(stack->name, stack->next->name, sizeof(stack->name), &error_code);
    stack->out_file= stack->next->out_file;
    stack->out_file->used++;
    if (stack->next == &init_settings)
    {
      /* never share with the global parent - it can change under your feet */
      stack->functions= ListCopy(init_settings.functions);
      LockMutex(cs);
      stack->keywords= ListCopy(init_settings.keywords);
      UnlockMutex(cs);
      stack->processes= ListCopy(init_settings.processes);
    }
    else
    {
      stack->functions= stack->next->functions;
      stack->keywords= stack->next->keywords;
      stack->processes= stack->next->processes;
    }
  }

  end= DbugStrTok(control);
  while (control < end)
  {
    int c, sign= (*control == '+') ? 1 : (*control == '-') ? -1 : 0;
    if (sign) control++;
    c= *control++;
    if (*control == ',')
      control++;
    /* XXX when adding new cases here, don't forget _db_explain_ ! */
    switch (c) {
    case 'd':
      if (sign < 0 && control == end)
      {
        LockIfInitSettings(cs);
        if (!is_shared(stack, keywords))
          FreeList(stack->keywords);
        stack->keywords=NULL;
        UnlockIfInitSettings(cs);
        stack->flags &= ~DEBUG_ON;
        break;
      }
      LockIfInitSettings(cs);
      if (rel && is_shared(stack, keywords))
        stack->keywords= ListCopy(stack->keywords);
      UnlockIfInitSettings(cs);
      if (sign < 0)
      {
        if (DEBUGGING)
        {
          LockIfInitSettings(cs);
          stack->keywords= ListDel(stack->keywords, control, end);
          UnlockIfInitSettings(cs);
        }
        break;
      }
      LockIfInitSettings(cs);
      stack->keywords= ListAdd(stack->keywords, control, end);
      UnlockIfInitSettings(cs);
      stack->flags |= DEBUG_ON;
      break;
    case 'D':
      stack->delay= atoi(control);
      break;
    case 'f':
      f_used= 1;
      if (sign < 0 && control == end)
      {
        if (!is_shared(stack,functions))
          FreeList(stack->functions);
        stack->functions=NULL;
        break;
      }
      if (rel && is_shared(stack,functions))
        stack->functions= ListCopy(stack->functions);
      if (sign < 0)
        stack->functions= ListDel(stack->functions, control, end);
      else
        stack->functions= ListAdd(stack->functions, control, end);
      break;
    case 'F':
      if (sign < 0)
        stack->flags &= ~FILE_ON;
      else
        stack->flags |= FILE_ON;
      break;
    case 'i':
      if (sign < 0)
        stack->flags &= ~PID_ON;
      else
        stack->flags |= PID_ON;
      break;
    case 'L':
      if (sign < 0)
        stack->flags &= ~LINE_ON;
      else
        stack->flags |= LINE_ON;
      break;
    case 'n':
      if (sign < 0)
        stack->flags &= ~DEPTH_ON;
      else
        stack->flags |= DEPTH_ON;
      break;
    case 'N':
      if (sign < 0)
        stack->flags &= ~NUMBER_ON;
      else
        stack->flags |= NUMBER_ON;
      break;
    case 'A':
    case 'O':
      stack->flags |= FLUSH_ON_WRITE;
      /* fall through */
    case 'a':
    case 'o':
      if (sign < 0)
      {
        DBUGCloseFile(cs, sstderr);
        stack->flags &= ~FLUSH_ON_WRITE;
        break;
      }
      if (c == 'a' || c == 'A')
        stack->flags |= OPEN_APPEND;
      else
        stack->flags &= ~OPEN_APPEND;
      if (control != end)
        DBUGOpenFile(cs, control, end, stack->flags & OPEN_APPEND);
      else
        DBUGOpenFile(cs, "-",0,0);
      break;
    case 'p':
      if (sign < 0 && control == end)
      {
        if (!is_shared(stack,processes))
          FreeList(stack->processes);
        stack->processes=NULL;
        break;
      }
      if (rel && is_shared(stack, processes))
        stack->processes= ListCopy(stack->processes);
      if (sign < 0)
        stack->processes= ListDel(stack->processes, control, end);
      else
        stack->processes= ListAdd(stack->processes, control, end);
      break;
    case 'P':
      if (sign < 0)
        stack->flags &= ~PROCESS_ON;
      else
        stack->flags |= PROCESS_ON;
      break;
    case 'r':
      stack->sub_level= cs->level;
      break;
    case 't':
      if (sign < 0)
      {
        if (control != end)
          stack->maxdepth-= atoi(control);
        else
          stack->maxdepth= 0;
      }
      else
      {
        if (control != end)
          stack->maxdepth+= atoi(control);
        else
          stack->maxdepth= MAXDEPTH;
      }
      if (stack->maxdepth > 0)
        stack->flags |= TRACE_ON;
      else
        stack->flags &= ~TRACE_ON;
      break;
    case 'T':
      if (sign < 0)
        stack->flags &= ~TIMESTAMP_ON;
      else
        stack->flags |= TIMESTAMP_ON;
      break;
    case 'S':
      if (sign < 0)
        stack->flags &= ~SANITY_CHECK_ON;
      else
        stack->flags |= SANITY_CHECK_ON;
      break;
    }
    if (!*end)
      break;
    control=end+1;
    end= DbugStrTok(control);
  }
  return !rel || f_used;
}
  
#define framep_trace_flag(cs, frp) (frp ?                                    \
                                     frp->level & TRACE_ON :                 \
                              (ListFlags(cs->stack->functions) & INCLUDE) ?  \
                                       0 : (uint)TRACE_ON)

static void FixTraceFlags_helper(CODE_STATE *cs, const char *func,
                                 struct _db_stack_frame_ *framep)
{
  if (framep->prev)
    FixTraceFlags_helper(cs, framep->func, framep->prev);

  cs->func= func;
  cs->level= framep->level & ~TRACE_ON;
  framep->level= cs->level | framep_trace_flag(cs, framep->prev);
  /*
    we don't set cs->framep correctly, even though DoTrace uses it.
    It's ok, because cs->framep may only affect DO_TRACE/DONT_TRACE return
    values, but we ignore them here anyway
  */
  switch(DoTrace(cs)) {
  case ENABLE_TRACE:
    framep->level|= TRACE_ON;
    break;
  case DISABLE_TRACE:
    framep->level&= ~TRACE_ON;
    break;
  }
}

#define fflags(cs) cs->stack->out_file ? ListFlags(cs->stack->functions) : TRACE_ON;

static void FixTraceFlags(uint old_fflags, CODE_STATE *cs)
{
  const char *func;
  uint new_fflags, traceon, level;
  struct _db_stack_frame_ *framep;

  /*
    first (a.k.a. safety) check:
    if we haven't started tracing yet, no call stack at all - we're safe.
  */
  framep=cs->framep;
  if (framep == 0)
    return;

  /*
    Ok, the tracing has started, call stack isn't empty.

    second check: does the new list have a SUBDIR rule ?
  */
  new_fflags=fflags(cs);
  if (new_fflags & SUBDIR)
    goto yuck;

  /*
    Ok, new list doesn't use SUBDIR.

    third check: we do NOT need to re-scan if
    neither old nor new lists used SUBDIR flag and if a default behavior
    (whether an unlisted function is traced) hasn't changed.
    Default behavior depends on whether there're INCLUDE elements in the list.
  */
  if (!(old_fflags & SUBDIR) && !((new_fflags^old_fflags) & INCLUDE))
    return;

  /*
    Ok, old list may've used SUBDIR, or defaults could've changed.

    fourth check: are we inside a currently active SUBDIR rule ?
    go up the call stack, if TRACE_ON flag ever changes its value - we are.
  */
  for (traceon=framep->level; framep; framep=framep->prev)
    if ((traceon ^ framep->level) & TRACE_ON)
      goto yuck;

  /*
    Ok, TRACE_ON flag doesn't change in the call stack.

    fifth check: but is the top-most value equal to a default one ?
  */
  if (((traceon & TRACE_ON) != 0) == ((new_fflags & INCLUDE) == 0))
    return;

yuck:
  /*
    Yuck! function list was changed, and one of the currently active rules
    was possibly affected. For example, a tracing could've been enabled or
    disabled for a function somewhere up the call stack.
    To react correctly, we must go up the call stack all the way to
    the top and re-match rules to set TRACE_ON bit correctly.

    We must traverse the stack forwards, not backwards.
    That's what a recursive helper is doing.
    It'll destroy two CODE_STATE fields, save them now.
  */
  func= cs->func;
  level= cs->level;
  FixTraceFlags_helper(cs, func, cs->framep);
  /* now we only need to restore CODE_STATE fields, and we're done */
  cs->func= func;
  cs->level= level;
}

/*
 *  FUNCTION
 *
 *      _db_set_       set current debugger settings
 *
 *  SYNOPSIS
 *
 *      VOID _db_set_(control)
 *      char *control;
 *
 *  DESCRIPTION
 *
 *      Given pointer to a debug control string in "control",
 *      parses the control string, and sets up a current debug
 *      settings. Pushes a new debug settings if the current is
 *      set to the initial debugger settings.
 *
 */

void _db_set_(const char *control)
{
  CODE_STATE *cs;
  uint old_fflags;
  get_code_state_or_return;
  old_fflags=fflags(cs);
  if (cs->stack == &init_settings)
    PushState(cs);
  if (DbugParse(cs, control))
    FixTraceFlags(old_fflags, cs);
}

/*
 *  FUNCTION
 *
 *      _db_push_       push current debugger settings and set up new one
 *
 *  SYNOPSIS
 *
 *      VOID _db_push_(control)
 *      char *control;
 *
 *  DESCRIPTION
 *
 *      Given pointer to a debug control string in "control", pushes
 *      the current debug settings, parses the control string, and sets
 *      up a new debug settings
 *
 */

void _db_push_(const char *control)
{
  CODE_STATE *cs;
  uint old_fflags;
  get_code_state_or_return;
  old_fflags=fflags(cs);
  PushState(cs);
  if (DbugParse(cs, control))
    FixTraceFlags(old_fflags, cs);
}


/**
  Returns TRUE if session-local settings have been set.
*/

int _db_is_pushed_()
{
  CODE_STATE *cs= NULL;
  get_code_state_or_return FALSE;
  return (cs->stack != &init_settings);
}

/*
 *  FUNCTION
 *
 *      _db_set_init_       set initial debugger settings
 *
 *  SYNOPSIS
 *
 *      VOID _db_set_init_(control)
 *      char *control;
 *
 *  DESCRIPTION
 *      see _db_set_
 *
 */

void _db_set_init_(const char *control)
{
  CODE_STATE tmp_cs;
  bzero((uchar*) &tmp_cs, sizeof(tmp_cs));
  tmp_cs.stack= &init_settings;
  tmp_cs.process= db_process ? db_process : "dbug";
  DbugParse(&tmp_cs, control);
}

/*
 *  FUNCTION
 *
 *      _db_pop_    pop the debug stack
 *
 *  DESCRIPTION
 *
 *      Pops the debug stack, returning the debug settings to its
 *      condition prior to the most recent _db_push_ invocation.
 *      Note that the pop will fail if it would remove the last
 *      valid settings from the stack.  This prevents user errors
 *      in the push/pop sequence from screwing up the debugger.
 *      Maybe there should be some kind of warning printed if the
 *      user tries to pop too many states.
 *
 */

void _db_pop_()
{
  uint old_fflags;
  CODE_STATE *cs;

  get_code_state_or_return;

  if (cs->stack != &init_settings)
  {
    old_fflags=fflags(cs);
    FreeState(cs, 1);
    FixTraceFlags(old_fflags, cs);
  }
}

/*
 *  FUNCTION
 *
 *      _db_explain_    generates 'control' string for the current settings
 *
 *  RETURN
 *      0 - ok
 *      1  - buffer too short, output truncated
 *
 */

/* helper macros */
#define char_to_buf(C)    do {                  \
        *buf++=(C);                             \
        if (buf >= end) goto overflow;          \
      } while (0)
#define str_to_buf(S)    do {                   \
        char_to_buf(',');                       \
        buf=strnmov(buf, (S), (uint) (end-buf)); \
        if (buf >= end) goto overflow;          \
      } while (0)
#define list_to_buf(l, f)  do {                 \
        struct link *listp=(l);                 \
        while (listp)                           \
        {                                       \
          if (listp->flags & (f))               \
          {                                     \
            str_to_buf(listp->str);             \
            if (listp->flags & SUBDIR)          \
              char_to_buf('/');                 \
          }                                     \
          listp=listp->next_link;               \
        }                                       \
      } while (0)
#define int_to_buf(i)  do {                     \
        char b[50];                             \
        int10_to_str((i), b, 10);               \
        str_to_buf(b);                          \
      } while (0)
#define colon_to_buf   do {                     \
        if (buf != start) char_to_buf(':');     \
      } while(0)
#define op_int_to_buf(C, val, def) do {         \
        if ((val) != (def))                     \
        {                                       \
          colon_to_buf;                         \
          char_to_buf((C));                     \
          int_to_buf(val);                      \
        }                                       \
      } while (0)
#define op_intf_to_buf(C, val, def, cond) do {  \
        if ((cond))                             \
        {                                       \
          colon_to_buf;                         \
          char_to_buf((C));                     \
          if ((val) != (def)) int_to_buf(val);  \
        }                                       \
      } while (0)
#define op_str_to_buf(C, val, cond) do {        \
        if ((cond))                             \
        {                                       \
          char *s=(val);                        \
          colon_to_buf;                         \
          char_to_buf((C));                     \
          if (*s) str_to_buf(s);                \
        }                                       \
      } while (0)
#define op_list_to_buf(C, val, cond) do {       \
        if ((cond))                             \
        {                                       \
          int f=ListFlags(val);                 \
          colon_to_buf;                         \
          char_to_buf((C));                     \
          if (f & INCLUDE)                      \
            list_to_buf(val, INCLUDE);          \
          if (f & EXCLUDE)                      \
          {                                     \
            colon_to_buf;                       \
            char_to_buf('-');                   \
            char_to_buf((C));                   \
            list_to_buf(val, EXCLUDE);          \
          }                                     \
        }                                       \
      } while (0)
#define op_bool_to_buf(C, cond) do {            \
        if ((cond))                             \
        {                                       \
          colon_to_buf;                         \
          char_to_buf((C));                     \
        }                                       \
      } while (0)

int _db_explain_ (CODE_STATE *cs, char *buf, size_t len)
{
  char *start=buf, *end=buf+len-4;

  get_code_state_if_not_set_or_return *buf=0;

  LockIfInitSettings(cs);
  op_list_to_buf('d', cs->stack->keywords, DEBUGGING);
  UnlockIfInitSettings(cs);
  op_int_to_buf ('D', cs->stack->delay, 0);
  op_list_to_buf('f', cs->stack->functions, cs->stack->functions);
  op_bool_to_buf('F', cs->stack->flags & FILE_ON);
  op_bool_to_buf('i', cs->stack->flags & PID_ON);
  op_bool_to_buf('L', cs->stack->flags & LINE_ON);
  op_bool_to_buf('n', cs->stack->flags & DEPTH_ON);
  op_bool_to_buf('N', cs->stack->flags & NUMBER_ON);
  op_str_to_buf(
    ((cs->stack->flags & FLUSH_ON_WRITE ? 0 : 32) |
     (cs->stack->flags & OPEN_APPEND ? 'A' : 'O')),
    cs->stack->name, cs->stack->out_file != sstderr);
  op_list_to_buf('p', cs->stack->processes, cs->stack->processes);
  op_bool_to_buf('P', cs->stack->flags & PROCESS_ON);
  op_bool_to_buf('r', cs->stack->sub_level != 0);
  op_intf_to_buf('t', cs->stack->maxdepth, MAXDEPTH, TRACING);
  op_bool_to_buf('T', cs->stack->flags & TIMESTAMP_ON);
  op_bool_to_buf('S', cs->stack->flags & SANITY_CHECK_ON);

  *buf= '\0';
  return 0;

overflow:
  *end++= '.';
  *end++= '.';
  *end++= '.';
  *end=   '\0';
  return 1;
}

#undef char_to_buf
#undef str_to_buf
#undef list_to_buf
#undef int_to_buf
#undef colon_to_buf
#undef op_int_to_buf
#undef op_intf_to_buf
#undef op_str_to_buf
#undef op_list_to_buf
#undef op_bool_to_buf

/*
 *  FUNCTION
 *
 *      _db_explain_init_       explain initial debugger settings
 *
 *  DESCRIPTION
 *      see _db_explain_
 */

int _db_explain_init_(char *buf, size_t len)
{
  CODE_STATE cs;
  bzero((uchar*) &cs,sizeof(cs));
  cs.stack=&init_settings;
  return _db_explain_(&cs, buf, len);
}

/*
 *  FUNCTION
 *
 *      _db_enter_    process entry point to user function
 *
 *  SYNOPSIS
 *
 *      VOID _db_enter_(_func_, _file_, _line_, _stack_frame_)
 *      char *_func_;           points to current function name
 *      char *_file_;           points to current file name
 *      int _line_;             called from source line number
 *      struct _db_stack_frame_ allocated on the caller's stack
 *
 *  DESCRIPTION
 *
 *      Called at the beginning of each user function to tell
 *      the debugger that a new function has been entered.
 *      Note that the pointers to the previous user function
 *      name and previous user file name are stored on the
 *      caller's stack (this is why the ENTER macro must be
 *      the first "executable" code in a function, since it
 *      allocates these storage locations).  The previous nesting
 *      level is also stored on the callers stack for internal
 *      self consistency checks.
 *
 *      Also prints a trace line if tracing is enabled and
 *      increments the current function nesting depth.
 *
 *      Note that this mechanism allows the debugger to know
 *      what the current user function is at all times, without
 *      maintaining an internal stack for the function names.
 *
 */

void _db_enter_(const char *_func_, const char *_file_,
                uint _line_, struct _db_stack_frame_ *_stack_frame_)
{
  CODE_STATE *cs;
  if (!((cs=code_state())))
  {
    _stack_frame_->level= 0; /* Set to avoid valgrind warnings if dbug is enabled later */
    _stack_frame_->prev= 0;
    return;
  }

  _stack_frame_->line= -1;
  _stack_frame_->func= cs->func;
  _stack_frame_->file= cs->file;
  cs->func=  _func_;
  cs->file=  _file_;
  _stack_frame_->prev= cs->framep;
  _stack_frame_->level= ++cs->level | framep_trace_flag(cs, cs->framep);
  cs->framep= _stack_frame_;

  switch (DoTrace(cs)) {
  case ENABLE_TRACE:
    cs->framep->level|= TRACE_ON;
    if (!TRACING) break;
    /* fall through */
  case DO_TRACE:
    if ((cs->stack->flags & SANITY_CHECK_ON) && (*dbug_sanity)())
      cs->stack->flags &= ~SANITY_CHECK_ON;
    if (TRACING)
    {
      int save_errno= errno;
      LockMutex(cs);
      DoPrefix(cs, _line_);
      Indent(cs, cs->level);
      (void) fprintf(cs->stack->out_file->file, ">%s\n", cs->func);
      UnlockMutex(cs);
      DbugFlush(cs);
      errno=save_errno;
    }
    break;
  case DISABLE_TRACE:
    cs->framep->level&= ~TRACE_ON;
    /* fall through */
  case DONT_TRACE:
    break;
  }
}

/*
 *  FUNCTION
 *
 *      _db_return_    process exit from user function
 *
 *  SYNOPSIS
 *
 *      VOID _db_return_(_line_, _stack_frame_)
 *      int _line_;             current source line number
 *      struct _db_stack_frame_ allocated on the caller's stack
 *
 *  DESCRIPTION
 *
 *      Called just before user function executes an explicit or implicit
 *      return.  Prints a trace line if trace is enabled, decrements
 *      the current nesting level, and restores the current function and
 *      file names from the defunct function's stack.
 *
 */

void _db_return_(struct _db_stack_frame_ *_stack_frame_)
{
  uint _slevel_= _stack_frame_->level & ~TRACE_ON;
  CODE_STATE *cs;
  get_code_state_or_return;

  if (_stack_frame_->line == 0)
    return;

  if (_stack_frame_->line == -1 || cs->framep != _stack_frame_)
  {
    char buf[512];
    my_snprintf(buf, sizeof(buf), ERR_MISSING_RETURN, cs->func);
    DbugExit(buf);
  }

  if (DoTrace(cs) & DO_TRACE)
  {
    if ((cs->stack->flags & SANITY_CHECK_ON) && (*dbug_sanity)())
      cs->stack->flags &= ~SANITY_CHECK_ON;
    if (TRACING)
    {
      int save_errno=errno;
      LockMutex(cs);
      DoPrefix(cs, _stack_frame_->line);
      Indent(cs, cs->level);
      (void) fprintf(cs->stack->out_file->file, "<%s\n", cs->func);
      UnlockMutex(cs);
      DbugFlush(cs);
      errno=save_errno;
    }
  }
  /*
    Check to not set level < 0. This can happen if DBUG was disabled when
    function was entered and enabled in function.
  */
  cs->level= _slevel_ != 0 ? _slevel_ - 1 : 0;
  cs->func= _stack_frame_->func;
  cs->file= _stack_frame_->file;
  if (cs->framep != NULL)
    cs->framep= cs->framep->prev;
}


/*
 *  FUNCTION
 *
 *      _db_pargs_    log arguments for subsequent use by _db_doprnt_()
 *
 *  SYNOPSIS
 *
 *      int _db_pargs_(_line_, keyword)
 *      int _line_;
 *      char *keyword;
 *
 *  DESCRIPTION
 *
 *      The new universal printing macro DBUG_PRINT, which replaces
 *      all forms of the DBUG_N macros, needs two calls to runtime
 *      support routines.  The first, this function, remembers arguments
 *      that are used by the subsequent call to _db_doprnt_().
 *
 */

int _db_pargs_(uint _line_, const char *keyword)
{
  CODE_STATE *cs;
  get_code_state_or_return 0;
  cs->u_line= _line_;
  cs->u_keyword= keyword;

  return DEBUGGING && _db_keyword_(cs, cs->u_keyword, 0);
}


/*
 *  FUNCTION
 *
 *      _db_doprnt_    handle print of debug lines
 *
 *  SYNOPSIS
 *
 *      VOID _db_doprnt_(format, va_alist)
 *      char *format;
 *      va_dcl;
 *
 *  DESCRIPTION
 *
 *      When invoked via one of the DBUG macros, tests the current keyword
 *      set by calling _db_pargs_() to see if that macro has been selected
 *      for processing via the debugger control string, and if so, handles
 *      printing of the arguments via the format string.  The line number
 *      of the DBUG macro in the source is found in u_line.
 *
 *      Note that the format string SHOULD NOT include a terminating
 *      newline, this is supplied automatically.
 *
 */

#include <stdarg.h>

void _db_doprnt_(const char *format,...)
{
  va_list args;
  CODE_STATE *cs;
  int save_errno;

  get_code_state_or_return;

  va_start(args,format);

  save_errno=errno;
  LockMutex(cs);
  DoPrefix(cs, cs->u_line);
  if (TRACING)
    Indent(cs, cs->level + 1);
  else
    (void) fprintf(cs->stack->out_file->file, "%s: ", cs->func);
  (void) fprintf(cs->stack->out_file->file, "%s: ", cs->u_keyword);
  DbugVfprintf(cs->stack->out_file->file, format, args);
  UnlockMutex(cs);
  DbugFlush(cs);
  errno=save_errno;

  va_end(args);
}

/*
 * This function is intended as a
 * vfprintf clone with consistent, platform independent output for 
 * problematic formats like %p, %zd and %lld.
 */
static void DbugVfprintf(FILE *stream, const char* format, va_list args)
{
  char cvtbuf[1024];
  (void) my_vsnprintf(cvtbuf, sizeof(cvtbuf), format, args);
  (void) fprintf(stream, "%s\n", cvtbuf);
}


/*
 *  FUNCTION
 *
 *            _db_dump_    dump a string in hex
 *
 *  SYNOPSIS
 *
 *            void _db_dump_(_line_,keyword,memory,length)
 *            int _line_;               current source line number
 *            char *keyword;
 *            char *memory;             Memory to print
 *            int length;               Bytes to print
 *
 *  DESCRIPTION
 *  Dump N characters in a binary array.
 *  Is used to examine corrupted memory or arrays.
 */

void _db_dump_(uint _line_, const char *keyword,
               const unsigned char *memory, size_t length)
{
  int pos;
  CODE_STATE *cs;
  get_code_state_or_return;

  if (_db_keyword_(cs, keyword, 0))
  {
    LockMutex(cs);
    DoPrefix(cs, _line_);
    if (TRACING)
    {
      Indent(cs, cs->level + 1);
      pos= MY_MIN(MY_MAX(cs->level-cs->stack->sub_level,0)*INDENT,80);
    }
    else
    {
      fprintf(cs->stack->out_file->file, "%s: ", cs->func);
    }
    (void) fprintf(cs->stack->out_file->file, "%s: Memory: %p  Bytes: (%ld)\n",
            keyword, memory, (long) length);

    pos=0;
    while (length-- > 0)
    {
      uint tmp= *((unsigned char*) memory++);
      if ((pos+=3) >= 80)
      {
        fputc('\n',cs->stack->out_file->file);
        pos=3;
      }
      fputc(_dig_vec_upper[((tmp >> 4) & 15)], cs->stack->out_file->file);
      fputc(_dig_vec_upper[tmp & 15], cs->stack->out_file->file);
      fputc(' ',cs->stack->out_file->file);
    }
    (void) fputc('\n',cs->stack->out_file->file);
    UnlockMutex(cs);
    DbugFlush(cs);
  }
}


/*
 *  FUNCTION
 *
 *      ListAddDel    modify the list according to debug control string
 *
 *  DESCRIPTION
 *
 *      Given pointer to a comma separated list of strings in "cltp",
 *      parses the list, and modifies "listp", returning a pointer
 *      to the new list.
 *
 *      The mode of operation is defined by "todo" parameter.
 *
 *      If it is INCLUDE, elements (strings from "cltp") are added to the
 *      list, they will have INCLUDE flag set. If the list already contains
 *      the string in question, new element is not added, but a flag of
 *      the existing element is adjusted (INCLUDE bit is set, EXCLUDE bit
 *      is removed).
 *
 *      If it is EXCLUDE, elements are added to the list with the EXCLUDE
 *      flag set. If the list already contains the string in question,
 *      it is removed, new element is not added.
 */

static struct link *ListAddDel(struct link *head, const char *ctlp,
                               const char *end, int todo)
{
  const char *start;
  struct link **cur;
  size_t len;
  int subdir;

  ctlp--;
next:
  while (++ctlp < end)
  {
    start= ctlp;
    subdir=0;
    while (ctlp < end && *ctlp != ',')
      ctlp++;
    len= (int) (ctlp-start);
    if (start[len-1] == '/')
    {
      len--;
      subdir=SUBDIR;
    }
    if (len == 0) continue;
    for (cur=&head; *cur; cur=&((*cur)->next_link))
    {
      if (!strncmp((*cur)->str, start, len))
      {
        if ((*cur)->flags & todo)  /* same action ? */
          (*cur)->flags|= subdir;  /* just merge the SUBDIR flag */
        else if (todo == EXCLUDE)
        {
          struct link *delme=*cur;
          *cur=(*cur)->next_link;
          free((void*) delme);
        }
        else
        {
          (*cur)->flags&=~(EXCLUDE & SUBDIR);
          (*cur)->flags|=INCLUDE | subdir;
        }
        goto next;
      }
    }
    *cur= (struct link *) DbugMalloc(sizeof(struct link)+len);
    memcpy((*cur)->str, start, len);
    (*cur)->str[len]=0;
    (*cur)->flags=todo | subdir;
    (*cur)->next_link=0;
  }
  return head;
}

/*
 *  FUNCTION
 *
 *      ListCopy    make a copy of the list
 *
 *  SYNOPSIS
 *
 *      static struct link *ListCopy(orig)
 *      struct link *orig;
 *
 *  DESCRIPTION
 *
 *      Given pointer to list, which contains a copy of every element from
 *      the original list.
 *
 *      the orig pointer can be NULL
 *
 *      Note that since each link is added at the head of the list,
 *      the final list will be in "reverse order", which is not
 *      significant for our usage here.
 *
 */

static struct link *ListCopy(struct link *orig)
{
  struct link *new_malloc;
  struct link *head;
  size_t len;

  head= NULL;
  while (orig != NULL)
  {
    len= strlen(orig->str);
    new_malloc= (struct link *) DbugMalloc(sizeof(struct link)+len);
    memcpy(new_malloc->str, orig->str, len);
    new_malloc->str[len]= 0;
    new_malloc->flags=orig->flags;
    new_malloc->next_link= head;
    head= new_malloc;
    orig= orig->next_link;
  }
  return head;
}

/*
 *  FUNCTION
 *
 *      InList    test a given string for member of a given list
 *
 *  DESCRIPTION
 *
 *      Tests the string pointed to by "cp" to determine if it is in
 *      the list pointed to by "linkp".  Linkp points to the first
 *      link in the list.  If linkp is NULL or contains only EXCLUDE
 *      elements then the string is treated as if it is in the list.
 *      This may seem rather strange at first but leads to the desired
 *      operation if no list is given.  The net effect is that all
 *      strings will be accepted when there is no list, and when there
 *      is a list, only those strings in the list will be accepted.
 *
 *  RETURN
 *      combination of SUBDIR, INCLUDE, EXCLUDE, MATCHED flags
 *
 */

static int InList(struct link *linkp, const char *cp, int exact_match)
{
  int result;
  for (result=MATCHED; linkp != NULL; linkp= linkp->next_link)
  {
    if (!(exact_match ? strcmp(linkp->str,cp) : fnmatch(linkp->str, cp, 0)))
    {
      result= linkp->flags;
      break;
    }
    if (!(linkp->flags & EXCLUDE))
      result=NOT_MATCHED;
    if (linkp->flags & SUBDIR)
      result|=SUBDIR;
  }
  return result;
}

/*
 *  FUNCTION
 *
 *      ListFlags    returns aggregated list flags (ORed over all elements)
 *
 */

static uint ListFlags(struct link *linkp)
{
  uint f;
  for (f=0; linkp != NULL; linkp= linkp->next_link)
    f|= linkp->flags;
  return f;
}

/*
 *  FUNCTION
 *
 *      PushState    push current settings onto stack and set up new one
 *
 *  SYNOPSIS
 *
 *      static VOID PushState()
 *
 *  DESCRIPTION
 *
 *      Pushes the current settings on the settings stack, and creates
 *      a new settings. The new settings is NOT initialized
 *
 *      The settings stack is a linked list of settings, with the new
 *      settings added at the head.  This allows the stack to grow
 *      to the limits of memory if necessary.
 *
 */

static void PushState(CODE_STATE *cs)
{
  struct settings *new_malloc;

  new_malloc= (struct settings *) DbugMalloc(sizeof(struct settings));
  bzero(new_malloc, sizeof(*new_malloc));
  new_malloc->next= cs->stack;
  cs->stack= new_malloc;
}

/*
 *  FUNCTION
 *
 *	FreeState    Free memory associated with a struct state.
 *
 *  SYNOPSIS
 *
 *	static void FreeState (state)
 *	struct state *state;
 *      int free_state;
 *
 *  DESCRIPTION
 *
 *	Deallocates the memory allocated for various information in a
 *	state. If free_state is set, also free 'state'
 *
 */
static void FreeState(CODE_STATE *cs, int free_state)
{
  struct settings *state= cs->stack;
  LockIfInitSettings(cs);
  if (!is_shared(state, keywords))
  {
    FreeList(state->keywords);
    state->keywords= NULL;
  }
  UnlockIfInitSettings(cs);
  if (!is_shared(state, functions))
    FreeList(state->functions);
  if (!is_shared(state, processes))
    FreeList(state->processes);

  DBUGCloseFile(cs, NULL);

  if (free_state)
  {
    struct settings *next= state->next;
    free(state);
    cs->stack= next;
  }
}


/*
 *  FUNCTION
 *
 *	_db_end_    End debugging, freeing state stack memory.
 *
 *  SYNOPSIS
 *
 *	static VOID _db_end_ ()
 *
 *  DESCRIPTION
 *
 *	Ends debugging, de-allocating the memory allocated to the
 *	state stack.
 *
 *	To be called at the very end of the program.
 *
 */
void _db_end_()
{
  CODE_STATE *cs, dummy_cs;
  /*
    Set _dbug_on_ to be able to do full reset even when DEBUGGER_OFF was
    called after dbug was initialized
  */
  _dbug_on_= 1;
  cs= code_state();

  if (cs)
  {
    while (cs->stack && cs->stack != &init_settings)
      FreeState(cs, 1);
  }
  else
  {
    cs= &dummy_cs;
    bzero(cs, sizeof(*cs));
  }

  cs->stack= &init_settings;
  FreeState(cs, 0);
  pthread_mutex_destroy(&THR_LOCK_dbug);
  init_done= 0;
  _dbug_on_= 0;
}


/*
 *  FUNCTION
 *
 *      DoTrace    check to see if tracing is current enabled
 *
 *  DESCRIPTION
 *
 *      Checks to see if dbug in this function is enabled based on
 *      whether the maximum trace depth has been reached, the current
 *      function is selected, and the current process is selected.
 *
 */

static int DoTrace(CODE_STATE *cs)
{
  int res= DONT_TRACE;
  if ((cs->stack->maxdepth == 0 || cs->level <= cs->stack->maxdepth) &&
      InList(cs->stack->processes, cs->process, 0) & (MATCHED|INCLUDE))
  {
    switch(InList(cs->stack->functions, cs->func, 0)) {
    case INCLUDE|SUBDIR:
      res= ENABLE_TRACE;
      break;
    case INCLUDE:
      res= DO_TRACE;
      break;
    case MATCHED|SUBDIR:
    case NOT_MATCHED|SUBDIR:
    case MATCHED:
      res= (framep_trace_flag(cs, cs->framep) ? DO_TRACE : DONT_TRACE);
      break;
    case EXCLUDE:
    case NOT_MATCHED:
      res= DONT_TRACE;
      break;
    case EXCLUDE|SUBDIR:
      res= DISABLE_TRACE;
      break;
    }
  }
  return res;
}


FILE *_db_fp_(void)
{
  CODE_STATE *cs;
  get_code_state_or_return NULL;
  return cs->stack->out_file->file;
}

/*
 *  FUNCTION
 *
 *      _db_keyword_    test keyword for member of keyword list
 *
 *  DESCRIPTION
 *
 *      Test a keyword to determine if it is in the currently active
 *      keyword list.  If strict=0, a keyword is accepted
 *      if the list is null, otherwise it must match one of the list
 *      members.  When debugging is not on, no keywords are accepted.
 *      After the maximum trace level is exceeded, no keywords are
 *      accepted (this behavior subject to change).  Additionally,
 *      the current function and process must be accepted based on
 *      their respective lists.
 *
 *      Returns TRUE if keyword accepted, FALSE otherwise.
 *
 */

BOOLEAN _db_keyword_(CODE_STATE *cs, const char *keyword, int strict)
{
  int match= strict ? INCLUDE : INCLUDE|MATCHED;
  int res;
  get_code_state_if_not_set_or_return FALSE;

  if (!(DEBUGGING && (DoTrace(cs) & DO_TRACE)))
    return 0;
  LockIfInitSettings(cs);
  res= (InList(cs->stack->keywords, keyword, strict) & match);
  UnlockIfInitSettings(cs);
  return res != 0;
}

/*
 *  FUNCTION
 *
 *      Indent    indent a line to the given indentation level
 *
 *  SYNOPSIS
 *
 *      static VOID Indent(indent)
 *      int indent;
 *
 *  DESCRIPTION
 *
 *      Indent a line to the given level.  Note that this is
 *      a simple minded but portable implementation.
 *      There are better ways.
 *
 *      Also, the indent must be scaled by the compile time option
 *      of character positions per nesting level.
 *
 */

static void Indent(CODE_STATE *cs, int indent)
{
  int count;

  indent= MY_MAX(indent-1-cs->stack->sub_level,0)*INDENT;
  for (count= 0; count < indent ; count++)
  {
    if ((count % INDENT) == 0)
      fputc('|',cs->stack->out_file->file);
    else
      fputc(' ',cs->stack->out_file->file);
  }
}


/*
 *  FUNCTION
 *
 *      FreeList    free all memory associated with a linked list
 *
 *  SYNOPSIS
 *
 *      static VOID FreeList(linkp)
 *      struct link *linkp;
 *
 *  DESCRIPTION
 *
 *      Given pointer to the head of a linked list, frees all
 *      memory held by the list and the members of the list.
 *
 */

static void FreeList(struct link *linkp)
{
  struct link *old;

  while (linkp != NULL)
  {
    old= linkp;
    linkp= linkp->next_link;
    free((void*) old);
  }
}


/*
 *  FUNCTION
 *
 *      DoPrefix    print debugger line prefix prior to indentation
 *
 *  SYNOPSIS
 *
 *      static VOID DoPrefix(_line_)
 *      int _line_;
 *
 *  DESCRIPTION
 *
 *      Print prefix common to all debugger output lines, prior to
 *      doing indentation if necessary.  Print such information as
 *      current process name, current source file name and line number,
 *      and current function nesting depth.
 *
 */

static void DoPrefix(CODE_STATE *cs, uint _line_)
{
  cs->lineno++;
  if (cs->stack->flags & PID_ON)
  {
    (void) fprintf(cs->stack->out_file->file, "%-7s: ", my_thread_name());
  }
  if (cs->stack->flags & NUMBER_ON)
    (void) fprintf(cs->stack->out_file->file, "%5d: ", cs->lineno);
  if (cs->stack->flags & TIMESTAMP_ON)
  {
#ifdef __WIN__
    /* FIXME This doesn't give microseconds as in Unix case, and the resolution is
       in system ticks, 10 ms intervals. See my_getsystime.c for high res */
    SYSTEMTIME loc_t;
    GetLocalTime(&loc_t);
    (void) fprintf (cs->stack->out_file->file,
                    /* "%04d-%02d-%02d " */
                    "%02d:%02d:%02d.%06d ",
                    /*tm_p->tm_year + 1900, tm_p->tm_mon + 1, tm_p->tm_mday,*/
                    loc_t.wHour, loc_t.wMinute, loc_t.wSecond, loc_t.wMilliseconds);
#else
    struct timeval tv;
    struct tm *tm_p;
    if (gettimeofday(&tv, NULL) != -1)
    {
      if ((tm_p= localtime((const time_t *)&tv.tv_sec)))
      {
        (void) fprintf (cs->stack->out_file->file,
                        /* "%04d-%02d-%02d " */
                        "%02d:%02d:%02d.%06d ",
                        /*tm_p->tm_year + 1900, tm_p->tm_mon + 1, tm_p->tm_mday,*/
                        tm_p->tm_hour, tm_p->tm_min, tm_p->tm_sec,
                        (int) (tv.tv_usec));
      }
    }
#endif
  }
  if (cs->stack->flags & PROCESS_ON)
    (void) fprintf(cs->stack->out_file->file, "%s: ", cs->process);
  if (cs->stack->flags & FILE_ON)
    (void) fprintf(cs->stack->out_file->file, "%14s: ", BaseName(cs->file));
  if (cs->stack->flags & LINE_ON)
    (void) fprintf(cs->stack->out_file->file, "%5d: ", _line_);
  if (cs->stack->flags & DEPTH_ON)
    (void) fprintf(cs->stack->out_file->file, "%4d: ", cs->level);
}


/*
 *  FUNCTION
 *
 *      DBUGOpenFile    open new output stream for debugger output
 *
 *  SYNOPSIS
 *
 *      static VOID DBUGOpenFile(name)
 *      char *name;
 *
 *  DESCRIPTION
 *
 *      Given name of a new file (or "-" for stdout) opens the file
 *      and sets the output stream to the new file.
 *
 */

static void DBUGOpenFile(CODE_STATE *cs,
                         const char *name,const char *end,int append)
{
  FILE *fp;

  if (name != NULL)
  {
    if (end)
    {
      size_t len=end-name;
      memcpy(cs->stack->name, name, len);
      cs->stack->name[len]=0;
    }
    else
      strmov(cs->stack->name,name);
    name=cs->stack->name;
    if (strcmp(name, "-") == 0)
    {
      DBUGCloseFile(cs, sstdout);
      cs->stack->flags |= FLUSH_ON_WRITE;
      cs->stack->name[0]=0;
    }
    else
    {
      if (!Writable(name))
      {
        (void) fprintf(stderr, ERR_OPEN, cs->process, name);
        perror("");
        fflush(stderr);
      }
      else
      {
        if (!(fp= fopen(name, append ? "a+" : "w")))
        {
          (void) fprintf(stderr, ERR_OPEN, cs->process, name);
          perror("");
          fflush(stderr);
        }
        else
        {
          sFILE *sfp= (sFILE *)DbugMalloc(sizeof(sFILE));
          sfp->file= fp;
          sfp->used= 1;
          DBUGCloseFile(cs, sfp);
        }
      }
    }
  }
}

/*
 *  FUNCTION
 *
 *      DBUGCloseFile    close the debug output stream
 *
 *  SYNOPSIS
 *
 *      static VOID DBUGCloseFile(fp)
 *      FILE *fp;
 *
 *  DESCRIPTION
 *
 *      Closes the debug output stream unless it is standard output
 *      or standard error.
 *
 */

static void DBUGCloseFile(CODE_STATE *cs, sFILE *new_value)
{
  sFILE *fp;
  if (!cs || !cs->stack || !(fp= cs->stack->out_file))
    return;

  if (fp != sstdout && fp != sstderr && --fp->used == 0)
  {
    if (fclose(fp->file) == EOF)
    {
      LockMutex(cs);
      (void) fprintf(stderr, ERR_CLOSE, cs->process);
      perror("");
      UnlockMutex(cs);
    }
    else
    {
      free(fp);
    }
  }
  cs->stack->out_file= new_value;
}


/*
 *  FUNCTION
 *
 *      DbugExit    print error message and exit
 *
 *  SYNOPSIS
 *
 *      static VOID DbugExit(why)
 *      char *why;
 *
 *  DESCRIPTION
 *
 *      Prints error message using current process name, the reason for
 *      aborting (typically out of memory), and exits with status 1.
 *      This should probably be changed to use a status code
 *      defined in the user's debugger include file.
 *
 */

static void DbugExit(const char *why)
{
  CODE_STATE *cs=code_state();
  (void) fprintf(stderr, ERR_ABORT, cs ? cs->process : "(null)", why);
  (void) fflush(stderr);
  DBUG_ABORT();
}


/*
 *  FUNCTION
 *
 *      DbugMalloc    allocate memory for debugger runtime support
 *
 *  SYNOPSIS
 *
 *      static long *DbugMalloc(size)
 *      int size;
 *
 *  DESCRIPTION
 *
 *      Allocate more memory for debugger runtime support functions.
 *      Failure to to allocate the requested number of bytes is
 *      immediately fatal to the current process.  This may be
 *      rather unfriendly behavior.  It might be better to simply
 *      print a warning message, freeze the current debugger cs,
 *      and continue execution.
 *
 */

static char *DbugMalloc(size_t size)
{
  char *new_malloc;

  if (!(new_malloc= (char*) malloc(size)))
    DbugExit("out of memory");
  return new_malloc;
}


/*
 *     strtok lookalike - splits on ':', magically handles ::, :\ and :/
 */

static const char *DbugStrTok(const char *s)
{
  while (s[0] && (s[0] != ':' ||
                  (s[1] == '\\' || s[1] == '/' || (s[1] == ':' && s++))))
    s++;
  return s;
}


/*
 *  FUNCTION
 *
 *      BaseName    strip leading pathname components from name
 *
 *  SYNOPSIS
 *
 *      static char *BaseName(pathname)
 *      char *pathname;
 *
 *  DESCRIPTION
 *
 *      Given pointer to a complete pathname, locates the base file
 *      name at the end of the pathname and returns a pointer to
 *      it.
 *
 */

static const char *BaseName(const char *pathname)
{
  const char *base;

  base= strrchr(pathname, FN_LIBCHAR);
  if (base++ == NullS)
    base= pathname;
  return base;
}


/*
 *  FUNCTION
 *
 *      Writable    test to see if a pathname is writable/creatable
 *
 *  SYNOPSIS
 *
 *      static BOOLEAN Writable(pathname)
 *      char *pathname;
 *
 *  DESCRIPTION
 *
 *      Because the debugger might be linked in with a program that
 *      runs with the set-uid-bit (suid) set, we have to be careful
 *      about opening a user named file for debug output.  This consists
 *      of checking the file for write access with the real user id,
 *      or checking the directory where the file will be created.
 *
 *      Returns TRUE if the user would normally be allowed write or
 *      create access to the named file.  Returns FALSE otherwise.
 *
 */


#ifndef Writable

static BOOLEAN Writable(const char *pathname)
{
  BOOLEAN granted;
  char *lastslash;

  granted= FALSE;
  if (EXISTS(pathname))
  {
    if (WRITABLE(pathname))
      granted= TRUE;
  }
  else
  {
    lastslash= strrchr(pathname, '/');
    if (lastslash != NULL)
      *lastslash= '\0';
    else
      pathname= ".";
    if (WRITABLE(pathname))
      granted= TRUE;
    if (lastslash != NULL)
      *lastslash= '/';
  }
  return granted;
}
#endif

/*
  flush dbug-stream, free mutex lock & wait delay
  This is because some systems (MSDOS!!) doesn't flush fileheader
  and dbug-file isn't readable after a system crash !!
*/

static void DbugFlush(CODE_STATE *cs)
{
  if (cs->stack->flags & FLUSH_ON_WRITE)
  {
    (void) fflush(cs->stack->out_file->file);
    if (cs->stack->delay)
      (void) Delay(cs->stack->delay);
  }
} /* DbugFlush */


/* For debugging */

void _db_flush_()
{
  CODE_STATE *cs;
  get_code_state_or_return;
  if (DEBUGGING)
  {
    (void) fflush(cs->stack->out_file->file);
  }
}


#ifndef __WIN__
void _db_suicide_()
{
  int retval;
  sigset_t new_mask;
  sigfillset(&new_mask);

  fprintf(stderr, "SIGKILL myself\n");
  fflush(stderr);

  retval= kill(getpid(), SIGKILL);
  assert(retval == 0);
  retval= sigsuspend(&new_mask);
  fprintf(stderr, "sigsuspend returned %d errno %d \n", retval, errno);
  assert(FALSE); /* With full signal mask, we should never return here. */
}
#endif  /* ! __WIN__ */


void _db_lock_file_()
{
  CODE_STATE *cs;
  get_code_state_or_return;
  LockMutex(cs);
}

void _db_unlock_file_()
{
  CODE_STATE *cs;
  get_code_state_or_return;
  UnlockMutex(cs);
}

const char* _db_get_func_(void)
{
  CODE_STATE *cs;
  get_code_state_or_return NULL;
  return cs->func;
}


static int default_my_dbug_sanity(void)
{
  return 0;
}

extern my_bool my_assert;
ATTRIBUTE_COLD
my_bool _db_my_assert(const char *file, int line, const char *msg)
{
  my_bool a = my_assert;
  _db_flush_();
  if (!a)
    fprintf(stderr, "%s:%d: assert: %s\n", file, line, msg);
  return a;
}
#else

/*
 * Dummy function, workaround for build failure on a platform where linking
 * with an empty archive fails.
 */
int i_am_a_dummy_function() {
  return 0;
}

#endif /* DBUG_OFF */

/*
  This function is called by default on DBUG_ASSERT() when compiled with
  DBUG_ASSERT_AS_PRINTF
*/

#ifdef DBUG_ASSERT_AS_PRINTF

static void default_my_dbug_assert_failed(const char *assert_expr,
                                          const char *file,
                                          unsigned long line)
{
  fprintf(stderr, "Warning: assertion failed: %s at %s line %lu\n",
          assert_expr, file, line);
}

void (*my_dbug_assert_failed)(const char *assert_expr, const char* file,
                              unsigned long line)= default_my_dbug_assert_failed;

#endif /* DBUG_ASSERT_AS_PRINTF */
