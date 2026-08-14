/* Definitions for boot script parser for Mach.  */

#ifndef _boot_script_h
#define _boot_script_h

/* Written by Shantanu Goel (goel@cs.columbia.edu).  */

/* Error codes returned by boot_script_parse_line()
   and boot_script_exec_cmd().  */
#define BOOT_SCRIPT_NOMEM		1
#define BOOT_SCRIPT_SYNTAX_ERROR	2
#define BOOT_SCRIPT_INVALID_ASG		3
#define BOOT_SCRIPT_MACH_ERROR		4
#define BOOT_SCRIPT_UNDEF_SYM		5
#define BOOT_SCRIPT_EXEC_ERROR		6
#define BOOT_SCRIPT_INVALID_SYM		7
#define BOOT_SCRIPT_BAD_TYPE		8

/* Legal values for argument `type' to function
   boot_script_set_variable and boot_script_define_function.  */
#define VAL_NONE	0	/* none -- function runs at exec time */
#define VAL_STR		1	/* string */
#define VAL_PORT	2	/* port */
#define VAL_TASK	3	/* task port */

/* The value a symbol or an argument carries.  The `type' above says which of
   these it is at any moment, so one storage class has to be wide enough for
   all of them: an integer, a string, a port, a task, a pointer to another
   symbol, and -- for VAL_FUNC -- a pointer to a function (#453).

   This was `int', which held every one of those on i386 because a pointer is
   four bytes there.  On x86-64 it holds none of the pointers.  The three
   builtin symbols made that visible at once, because a truncated function
   address is not a constant the linker can place and so the initialiser
   would not even compile -- but the same truncation applies to every string
   and every symbol pointer this script passes around, and those would have
   compiled quietly.  */
typedef long boot_script_val_t;

/* This structure describes a command.  */
struct cmd
{
  /* Cookie passed in to boot_script_parse_line.  */
    vm_offset_t start;
    vm_size_t size;

  /* Path of executable.  */
  char *path;

  /* Task port.  */
  task_t task;

  /* Argument list.  */
  struct arg **args;

  /* Amount allocated for `args'.  */
  int args_alloc;

  /* Next available slot in `args'.  */
  int args_index;

  /* List of functions that want to be run on command execution.  */
  struct sym **exec_funcs;

  /* Amount allocated for `exec_funcs'.  */
  int exec_funcs_alloc;

  /* Next available slot in `exec_funcs'.  */
  int exec_funcs_index;
};


/* The user must define these functions, we work like malloc and free.  */
void *boot_script_malloc (unsigned int);
void boot_script_free (void *, unsigned int);

/* The user must define this function.  Load the image of the
   executable specified by PATH in TASK.  Create a thread
   in TASK and point it at the executable's entry point.  Initialize
   TASK's stack with argument vector ARGV of length ARGC whose
   strings are STRINGS.  STRINGS has length STRINGLEN.
   Return 0 for success, non-zero otherwise.  */
int boot_script_exec_cmd (vm_offset_t start, vm_size_t size,
			  task_t task, char *path, int argc,
			  char **argv, char *strings, int stringlen);

/* The user must define this function.  Load the contents of FILE
   into a fresh anonymous memory object and return the memory object port.  */
mach_port_t boot_script_read_file (const char *file);

/* The user must define this functions to perform the corresponding
   Mach task manipulations.  */
int boot_script_task_create (struct cmd *); /* task_create + task_suspend */
int boot_script_task_resume (struct cmd *);
int boot_script_prompt_task_resume (struct cmd *);
/*
 * ⚠️ The right is a boot_script_val_t and not a mach_port_t (#415).  What the
 * caller holds is a port POINTER -- #453 widened boot_script_val_t to a long
 * for exactly this, saying `an integer, or a pointer of any kind' -- and
 * narrowing it here truncated it on the way in and the callee widened it back
 * on the way out, losing the top half of every port the bootstrap task is
 * given.  The compiler said so on every x86-64 build.
 */
int boot_script_insert_right (struct cmd *, boot_script_val_t, mach_port_t *namep);
int boot_script_insert_task_port (struct cmd *, task_t, mach_port_t *namep);

/* The user must define this function to clean up the `task_t'
   returned by boot_script_task_create.  */
void boot_script_free_task (task_t task, int aborting);


/* Parse the command line LINE.  This causes the command line to be
   converted into an internal format.  Returns 0 for success, non-zero
   otherwise.

   NOTE: The parser writes into the line so it must not be a string constant.
   It is also the responsibility of the caller not to deallocate the line
   across calls to the parser.  */
int boot_script_parse_line (vm_offset_t start, vm_size_t size, char *cmdline);

/* Execute the command lines prevously parsed.
   Returns 0 for success, non-zero otherwise.  */
int boot_script_exec (void);

/* Create an entry in the symbol table for variable NAME,
   whose type is TYPE and value is VAL.  Returns 0 on success,
   non-zero otherwise.  */
int boot_script_set_variable (const char *name, int type,
			      boot_script_val_t val);

/* Define the function NAME, which will return type RET_TYPE.  */
int boot_script_define_function (const char *name, int ret_type,
				 int (*func) (const struct cmd *cmd,
					      boot_script_val_t *val));

/* Returns a string describing the error ERR.  */
char *boot_script_error_string (int err);


#endif /* _boot_script_h */
