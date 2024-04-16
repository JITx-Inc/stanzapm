#include <stdio.h>
#include <sys/types.h>
#include <stanza/types.h>
#include <spawn.h>
#include "process.h"

//============================================================
//================= ChildProcess Registration ================
//============================================================

// Holds all metadata for a spawned child process.
// - pid: The process id of the child process itself. 
// - fin/fout/ferr: The stdin/stdout/stderr streams for communicating with the child.
//   NULL if child uses stdin/stdout/stderr directly.
// - status: Pointer to location to write status code when child is terminated.
//   Used by signal handler.
typedef struct ChildProcess {
  pid_t pid;
  FILE* fin;
  FILE* fout;
  FILE* ferr;
  stz_int* status;
} ChildProcess;

// Represents a linked list of ChildProcess.
// - proc: The pointer to the ChildProcess itself.
// - next: The next link in the list. NULL if end of list.
// NOTE: volatile declarations on 'next' forced by compiler.
typedef struct ChildProcessList {
  ChildProcess* proc;
  volatile struct ChildProcessList * volatile next;
} ChildProcessList;

// Linked list of live child processes.
// Will be read from by signal handler.
volatile ChildProcessList * volatile child_processes = NULL;

// Add a new ChildProcess to the global 'child_processes' list.
// Precondition: SIGCHLD is blocked
void add_child_process (ChildProcess* child) {
  ChildProcessList * new_node = (ChildProcessList*)malloc(sizeof(ChildProcessList));
  new_node->proc = child;
  new_node->next = child_processes;
  child_processes = new_node;
}

// Return the ChildProcess with the given process id.
// Returns NULL if there is none.
static ChildProcess* get_child_process (pid_t pid){
  volatile ChildProcessList * volatile curr = child_processes;
  while(curr != NULL && curr->proc->pid != pid)
    curr = curr->next;
  return curr->proc;
}

// Remove the ChildProcess with the given process id from the
// 'child_processes' list if it exists in the list.
// Precondition: SIGCHLD is blocked
static void remove_child_process (pid_t pid) {

  // Find the ChildProcess with matching pid.
  // After this loop, either:
  // 1. curr is NULL: No matching ChildProcess was found.
  // 2. curr is ChildProcessList*: The matching ChildProcess was found.
  volatile ChildProcessList * volatile curr = child_processes;
  volatile ChildProcessList * volatile prev = NULL;
  while(curr != NULL && curr->proc->pid != pid) {
    prev = curr;
    curr = curr->next;
  }
  
  // Remove child process from list if one was found.
  if(curr != NULL){
    if(prev == NULL)
      child_processes = curr->next;
    else
      prev->next = curr->next;
    free((void*) curr);
  }
}

// After a child process is spawned, this function creates a
// ChildProcess struct for recording the child's metadata
// and stores it in the 'child_processes' list.
// Precondition: Assumes that SIGCHLD is blocked.
// - status (output): A newly initialized ProcessStatus
//   is written to the provided pointer.
static void register_child_process (
              pid_t pid,
              FILE* fin,
              FILE* fout,
              FILE* ferr,
              ProcessStatus** status) {

  // Initialize and save ProcessStatus struct.
  ProcessStatus* st = (ProcessStatus*)malloc(sizeof(ProcessStatus));
  st->status_code = -1;
  *status = st;
  
  // Initialize ChildProcess struct.
  ChildProcess* child = (ChildProcess*)malloc(sizeof(ChildProcess));
  child->pid = pid;
  child->fin = fin;
  child->fout = fout;
  child->ferr = ferr;
  child->status = &(st->status_code);
  
  // Store child in ChildProcessList
  add_child_process(child);
}

//============================================================
//================= ChildProcess Operations ==================
//============================================================

// Helper: Return true if the given status code
// represents a process that was terminated, either
// normally (WIFEXITED) or abruptly (WIFSIGNALED).
static bool is_dead_status (stz_int status_code) {
  return WIFSIGNALED(status_code) || WIFEXITED(status_code);
}

// Update the status code for the registered child process
// with the given process id.
// Precondition: SIGCHLD is blocked
static void set_child_status (pid_t pid, stz_int status_code) {
  ChildProcess* proc = get_child_process(pid);
  if(proc != NULL)
    *(proc->status) = status_code;
}

// Update the current status of the given registered child process.
// If the status has changed, then record it in its ChildProcess
// metadata struct.
// Precondition: SIGCHLD is blocked
static void update_child_status (pid_t pid) {
  //Retrieve the status of the given process.
  int status;
  int ret_pid = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);

  //waitpid returns a positive integer if the process status has changed.
  if(ret_pid > 0) {
    set_child_status(pid, status);
    // Remove the child's metadata from list if child is dead.
    if(is_dead_status(status))
      remove_child_process(pid);
  }
}

// Update the current status of all registered child processes.
// Precondition: SIGCHLD is blocked
static void update_all_child_statuses () {
  volatile ChildProcessList * volatile curr = child_processes;
  while(curr != NULL) {
    //NOTE: curr->next is retrieved preemptively because
    //update_child_status may call free() on curr, and make
    //curr->next inaccessible.
    volatile ChildProcessList* next = curr->next;
    update_child_status(curr->proc->pid);
    curr = next;
  }
}

//============================================================
//==================== Autoreap Handler ======================
//============================================================

// When the new sigchild handler is installed,
// the existing handler is stored here.
struct sigaction old_sigchild_action;

// This handler automatically uses waitpid to reap registered child processes
// when they terminate.
// Note: This SIGCHLD handler is not re-entrant. It cannot
// be executed if it is already in the middle of executing. SIGCHILD
// must be blocked.
void autoreaping_sigchld_handler(int sig){
  //Update the statuses of all registered child processes.
  update_all_child_statuses();

  //Test whether we are able to forward the signal
  //to the previous signal handler. It must:
  //1. Not be using the sigaction system. (SA_SIGINFO)
  //2. Not be the default signal handler. (SIG_DFL)
  //3. Not be explicitly ignored. (SIG_IGN)
  if(!(old_sigchild_action.sa_flags & SA_SIGINFO) &&
     old_sigchild_action.sa_handler != SIG_DFL &&
     old_sigchild_action.sa_handler != SIG_IGN){
    old_sigchild_action.sa_handler(sig);
  }
}

// This installs the autoreaping signal handler.
void install_autoreaping_sigchld_handler () {
  //Create signal mask containing on SIGCHLD.
  sigset_t sigchld_mask;
  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  //Setup SIGCHLD handler  .
  //SIGCHLD is blocked during execution of the handler.
  struct sigaction sa = {
    .sa_handler = autoreaping_sigchld_handler,
    .sa_mask = sigchld_mask,
    .sa_flags = SA_RESTART
  };
  sigaction(SIGCHLD, &sa, &old_sigchild_action);
}

//============================================================
//================= Signal Handler Utilities =================
//============================================================

//Blocks the SIGCHILD signal by updating the signal mask.
//Returns the previous signal mask.
static sigset_t block_sigchild () {
  //Create signal mask containing only SIGCHLD.
  sigset_t sigchld_mask;
  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  //Install the new mask. Exit with error if unsuccessful.
  sigset_t old_mask;
  if(sigprocmask(SIG_BLOCK, &sigchld_mask, &old_mask))
    exit_with_error();

  //Return the previous signal mask.
  return old_mask;
}

//Suspends the current thread until a SIGCHLD
//signal is encountered.
static void suspend_until_sigchild () {
  //Create a signal mask that blocks everything
  //except SIGCHLD.
  sigset_t allow_sigchld;
  sigfillset(&allow_sigchld);
  sigdelset(&allow_sigchld, SIGCHLD);

  //Call sigsuspend, which always returns -1,
  //and which wakes up with errno == EINTR on
  //normal wakeup.
  sigsuspend(&allow_sigchld);
  if(errno != EINTR) exit_with_error();
}

//Restore the signal mask to the given mask.
//Exits program with error if unsuccessful.
static void restore_signal_mask (sigset_t* old_mask)  {
  if(sigprocmask(SIG_SETMASK, old_mask, NULL))
    exit_with_error();
}

//============================================================
//================== Retrieve Process State ==================
//============================================================

//Create a Stanza ProcessState struct from a POSIX status code.
ProcessState make_process_state (stz_int status_code){
  if(WIFEXITED(status_code))
    return (ProcessState){PROCESS_DONE, WEXITSTATUS(status_code)};
  else if(WIFSIGNALED(status_code))
    return (ProcessState){PROCESS_TERMINATED, WTERMSIG(status_code)};
  else if(WIFSTOPPED(status_code))
    return (ProcessState){PROCESS_STOPPED, WSTOPSIG(status_code)};
  else
    return (ProcessState){PROCESS_RUNNING, 0};  
}

// Retrieve the state of the given process.
// - process: The process to retrieve the state of.
// - s: Write out the process state here.
// - wait_for_termination: True if this function should block
//   until the process is terminated.
stz_int retrieve_process_state (Process* process,
                                ProcessState* s,
                                stz_int wait_for_termination){
  
  //Block SIGCHLD while we're reading the process state.
  sigset_t old_signal_mask = block_sigchild();

  //Read the current status code of the process.
  stz_int status = process->status->status_code;

  //If we need to wait for termination, then
  //call suspend_until_sigchild repeatedly until
  //process is dead.
  if(wait_for_termination){
    while(!is_dead_status(status)){
      suspend_until_sigchild();
      status = process->status->status_code;
    }    
  }

  //Store the status code as a Stanza ProcessState.
  *s = make_process_state(status);
  
  //End Block SIGCHLD.
  restore_signal_mask(&old_signal_mask);

  //Return 0 to indicate success.
  //TODO: In this implementation failures halt the program
  //instead of being communicated to caller.
  return 0;
}

//============================================================
//================= OS-X Process Launching ===================
//============================================================
#ifdef PLATFORM_OS_X

//Useful for testing linux implementation on OSX
//extern char **environ;
//int execvpe(const char *program, char **argv, char **envp){
//  char **saved = environ;
//  environ = envp;
//  int rc = execvp(program, argv);
//  environ = saved;
//  return rc;
//}

stz_int launch_process(stz_byte* file, stz_byte** argvs,
                       stz_int input, stz_int output, stz_int error, 
                       stz_byte* working_dir, stz_byte** env_vars, Process* process) {
  //block sigchld
  sigset_t old_signal_mask = block_sigchild();
  
  //Compute which pipes to create for the process.
  //has_pipes[PROCESS_IN] = 1, indicates that a process input pipe
  //needs to be created.
  int has_pipes[NUM_STREAM_SPECS];
  for(int i=0; i<NUM_STREAM_SPECS; i++)
    has_pipes[i] = 0;
  has_pipes[input] = 1;
  has_pipes[output] = 1;
  has_pipes[error] = 1;
  has_pipes[STANDARD_IN] = 0;
  has_pipes[STANDARD_OUT] = 0;
  has_pipes[STANDARD_ERR] = 0;

  //Setup file actions.
  //Will be deleted on clean up.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);

  //Generate pipes for PROCESS_IN, PROCESS_OUT, PROCESS_ERR.
  int pipes[NUM_STREAM_SPECS][2];
  for(int i=0; i<NUM_STREAM_SPECS; i++)
    if(has_pipes[i])
      if(pipe(pipes[i])) goto return_error;

  //Connect process input pipe if necessary.
  if(has_pipes[PROCESS_IN]){
    if(posix_spawn_file_actions_addclose(&actions, pipes[PROCESS_IN][1]))
      goto return_error;
    if(posix_spawn_file_actions_adddup2(&actions, pipes[PROCESS_IN][0], STDIN_FILENO))
      goto return_error;
    if(posix_spawn_file_actions_addclose(&actions, pipes[PROCESS_IN][0]))
      goto return_error;
  }
  //Connect process output pipe if necessary.
  if(has_pipes[PROCESS_OUT]){
    if(posix_spawn_file_actions_addclose(&actions, pipes[PROCESS_OUT][0]))
      goto return_error;
    if(output == PROCESS_OUT)
      if(posix_spawn_file_actions_adddup2(&actions, pipes[PROCESS_OUT][1], STDOUT_FILENO))
        goto return_error;
    if(error == PROCESS_OUT)
      if(posix_spawn_file_actions_adddup2(&actions, pipes[PROCESS_OUT][1], STDERR_FILENO))
        goto return_error;      
    if(posix_spawn_file_actions_addclose(&actions, pipes[PROCESS_OUT][1]))
      goto return_error;    
  }
  //Connect process error pipe if necessary.
  if(has_pipes[PROCESS_ERR]){
    if(posix_spawn_file_actions_addclose(&actions, pipes[PROCESS_ERR][0]))
      goto return_error;
    if(error == PROCESS_ERR)
      if(posix_spawn_file_actions_adddup2(&actions, pipes[PROCESS_ERR][1], STDERR_FILENO))
        goto return_error;
    if(output == PROCESS_ERR)
      if(posix_spawn_file_actions_adddup2(&actions, pipes[PROCESS_ERR][1], STDOUT_FILENO))
        goto return_error;
    if(posix_spawn_file_actions_addclose(&actions, pipes[PROCESS_ERR][1]))
      goto return_error;        
  }

  //Setup working directory
  if(working_dir) {
    if(posix_spawn_file_actions_addchdir_np(&actions, C_CSTR(working_dir)))
      goto return_error;
  }

  //Spawn the child process.
  pid_t pid = -1;
  int spawn_ret = posix_spawnp(&pid, C_CSTR(file), &actions, NULL, (char**)argvs, (char**)env_vars);
  if(spawn_ret){
    errno = spawn_ret;
    goto return_error;
  }

  //Set up the pipes in the parent process.
  FILE* fin = NULL;
  if(has_pipes[PROCESS_IN]) {
    close(pipes[PROCESS_IN][0]);
    fin = fdopen(pipes[PROCESS_IN][1], "w");
    if(fin == NULL) goto return_error;
  }
  FILE* fout = NULL;
  if(has_pipes[PROCESS_OUT]) {
    close(pipes[PROCESS_OUT][1]);
    fout = fdopen(pipes[PROCESS_OUT][0], "r");
    if(fout == NULL) goto return_error;
  }
  FILE* ferr = NULL;
  if(has_pipes[PROCESS_ERR]) {
    close(pipes[PROCESS_ERR][1]);
    ferr = fdopen(pipes[PROCESS_ERR][0], "r");
    if(ferr == NULL) goto return_error;
  }

  //Store the process details in the process structure.
  process->pid = pid;
  process->in = fin;
  process->out = fout;
  process->err = ferr;

  //Register the child process so that it is automatically
  //reaped by the autoreap handler.
  register_child_process(pid, fin, fout, ferr, &(process->status));

  //Process spawn was successful.
  goto return_success;

  // Perform cleanup and return -1 to indicate error.
  return_error: {
    posix_spawn_file_actions_destroy(&actions);
    restore_signal_mask(&old_signal_mask);
    return -1;
  }

  // Perform cleanup and return 0 to indicate success.
  return_success: {
    posix_spawn_file_actions_destroy(&actions);
    restore_signal_mask(&old_signal_mask);
    return 0;
  }
}

#endif

//============================================================
//================= Linux Process Launching ==================
//============================================================
#ifdef PLATFORM_LINUX

stz_int launch_process(stz_byte* file, stz_byte** argvs, stz_int input,
                       stz_int output, stz_int error, 
                       stz_byte* working_dir, stz_byte** env_vars, Process* process) {
  
  //Compute which pipes to create for the process.
  //has_pipes[PROCESS_IN] = 1, indicates that a process input pipe
  //needs to be created.
  int has_pipes[NUM_STREAM_SPECS];
  for(int i=0; i<NUM_STREAM_SPECS; i++)
    has_pipes[i] = 0;
  has_pipes[input] = 1;
  has_pipes[output] = 1;
  has_pipes[error] = 1;
  has_pipes[STANDARD_IN] = 0;
  has_pipes[STANDARD_OUT] = 0;
  has_pipes[STANDARD_ERR] = 0;

  //Generate pipes for PROCESS_IN, PROCESS_OUT, PROCESS_ERR.
  int pipes[NUM_STREAM_SPECS][2];
  for(int i=0; i<NUM_STREAM_SPECS; i++)
    if(has_pipes[i])
      if(pipe(pipes[i])) return -1;

  // Fork child process
  stz_long pid = (stz_long)vfork();
  if(pid < 0) return -1;
  
  // Parent: if exec succeeded, open files, register with signal handler
  if(pid > 0) {

    //Block SIGCHLD until setup is finished
    sigset_t old_signal_mask = block_sigchild();

    //Set up the pipes in the parent process.
    FILE* fin = NULL;
    if(has_pipes[PROCESS_IN]) {
      close(pipes[PROCESS_IN][0]);
      fin = fdopen(pipes[PROCESS_IN][1], "w");
      if(fin == NULL) goto return_error;
    }
    FILE* fout = NULL;
    if(has_pipes[PROCESS_OUT]) {
      close(pipes[PROCESS_OUT][1]);
      fout = fdopen(pipes[PROCESS_OUT][0], "r");
      if(fout == NULL) goto return_error;
    }
    FILE* ferr = NULL;
    if(has_pipes[PROCESS_ERR]) {
      close(pipes[PROCESS_ERR][1]);
      ferr = fdopen(pipes[PROCESS_ERR][0], "r");
      if(ferr == NULL) goto return_error;
    }

    //Store the process details in the process structure.
    process->pid = pid;
    process->in = fin;
    process->out = fout;
    process->err = ferr;

    //Register the child process so that it is automatically
    //repeated by the autoreap handler.
    register_child_process(pid, fin, fout, ferr, &(process->status));

    //Child process successfully launched and registered.
    goto return_success;

    //Perform cleanup and return -1 to indicate error.
    return_error: {
      restore_signal_mask(&old_signal_mask);
      return -1;
    }
    
    //Perform cleanup and return 0 to indicate success.
    return_success: {
      restore_signal_mask(&old_signal_mask);
      return 0;
    }
  }
  
  // Child: setup pipes, exec
  else {
    //Connect process input pipe if necessary.
    if(has_pipes[PROCESS_IN]){
      if(close(pipes[PROCESS_IN][1]) < 0) exit(-1);
      if(dup2(pipes[PROCESS_IN][0], STDIN_FILENO) < 0) exit(-1);
      if(close(pipes[PROCESS_IN][0]) < 0) exit(-1);
    }
    //Connect process output pipe if necessary.
    if(has_pipes[PROCESS_OUT]){
      if(close(pipes[PROCESS_OUT][0]) < 0) exit(-1);
      if(output == PROCESS_OUT)
        if(dup2(pipes[PROCESS_OUT][1], STDOUT_FILENO) < 0) exit(-1);
      if(error == PROCESS_OUT)
        if(dup2(pipes[PROCESS_OUT][1], STDERR_FILENO) < 0) exit(-1);
      if(close(pipes[PROCESS_OUT][1]) < 0) exit(-1);
    }
    //Connect process error pipe if necessary.
    if(has_pipes[PROCESS_ERR]){
      if(close(pipes[PROCESS_ERR][0]) < 0) exit(-1);
      if(output == PROCESS_ERR)
        if(dup2(pipes[PROCESS_ERR][1], STDOUT_FILENO) < 0) exit(-1);
      if(error == PROCESS_ERR)
        if(dup2(pipes[PROCESS_ERR][1], STDERR_FILENO) < 0) exit(-1);
      if(close(pipes[PROCESS_ERR][1]) < 0) exit(-1);
    }
    
    //Setup working directory
    if(working_dir) {
      if(chdir(C_CSTR(working_dir)) < 0) exit(-1);
    }

    //Launch child process.
    //If an environment is supplied then call execvpe, otherwise call execvp.
    if(env_vars == NULL)
      execvp(C_CSTR(file), (char**)argvs);
    else
      execvpe(C_CSTR(file), (char**)argvs, (char**)env_vars);

    //Exit to indicate that execvp did not work.
    exit(-1);
  }
}

#endif
