#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#ifdef PLATFORM_WINDOWS
  #include <fcntl.h>
  #include <io.h>
#else
  #include <unistd.h>
#endif

static FILE* debug_adapter_log;
static const char* debug_adapter_path;

static inline char* get_absolute_path(const char* s) {
  return realpath(s, NULL);
}
static inline void free_path(const char* s) {
  free((char*)s);
}

// Check if i'th argument is followed by a value. The value must not start with "--".
static inline bool has_arg_value(int i, const int argc, char* const argv[]) {
  if (++i >= argc) return false;
  const char* s = argv[i];
  if (s[0] == '-' && s[1] == '-') return false;
  return true;
}

// Report an error if i'th argument is not followed by a value. The value must not start with "--".
static inline void expect_arg_value(int i, const int argc, char* const argv[]) {
  if (!has_arg_value(i, argc, argv)) {
    fprintf(stderr, "Argument %s must be followed by a value\n", argv[i]);
    exit(EXIT_FAILURE);
  }
}

// Search argv backwards for the option starting with --name.
// Return the index of the option or 0 if the option is not found.
// Note: argv[0] is the program path.
static inline int find_last_arg(const char* name, int argc, char* const argv[]) {
  while (--argc > 0) {
    const char* s = argv[argc];
    if (s[0] == '-' && s[1] == '-' && strcmp(s+2, name) == 0)
      break;
  }
  return argc;
}

// Search for the arg by name. Verify that it is followed by a value.
static int find_last_arg_with_value(const char* name, int argc, char* const argv[]) {
  const int i = find_last_arg(name, argc, argv);
  if (i) expect_arg_value(i, argc, argv);
  return i;
}

typedef enum {
  CONSOLE,
  STDOUT,
  STDERR,
  TELEMETRY
} OutputType;

static inline const char* get_output_category(const OutputType type) {
  static const char* const names[] = {
    "console",
    "stdout",
    "stderr",
    "telemetry"
  };
  return names[type];
}

static void send_output(const OutputType o, const char* data, unsigned length) {
  fprintf(stderr, "Redirected: ");
  fwrite(data, 1, length, stderr);
  //Not implemented yet
}
static void send_stdout(const char* data, unsigned length) {
  send_output(STDOUT, data, length);
}
static void send_stderr(const char* data, unsigned length) {
  send_output(STDERR, data, length);
}

static void* redirect_output_loop(void* args) {
  const int read_fd = ((const int*)args)[0];
  const OutputType out = (OutputType)(((const int*)args)[1]);
  char buffer[4096];
  while (true) {
    const int bytes_read = read(read_fd, &buffer, sizeof buffer);
    if (bytes_read > 0)
      send_output(out, buffer, bytes_read);
  }
  return NULL;
}
static const char* redirect_fd(int fd, const OutputType out) {
  static char error_buffer[256];
  int new_fd[2];
#ifdef PLATFORM_WINDOWS
  if (_pipe(new_fd, 4096, O_TEXT) == -1) {
#else
  if (pipe(new_fd) == -1) {
#endif
    const int error = errno;
    snprintf(error_buffer, sizeof error_buffer, "Couldn't create new pipe for fd %d. %s", fd, strerror(error));
    return error_buffer;
  }
  if (dup2(new_fd[1], fd) == -1) {
    const int error = errno;
    snprintf(error_buffer, sizeof error_buffer, "Couldn't override the fd %d. %s", fd, strerror(error));
    return error_buffer;
  }

  int args[2] = {new_fd[0], (int)out};
  pthread_t thread;
  if (pthread_create(&thread, NULL, &redirect_output_loop, &args)) {
    const int error = errno;
    snprintf(error_buffer, sizeof error_buffer, "Couldn't create the redirect thread for fd %d. %s", fd, strerror(error));
    return error_buffer;
  }
  return NULL;
}

static void redirect_output(FILE* file, const OutputType out) {
  const char* error = redirect_fd(fileno(file), out);
  if (error) {
    if (debug_adapter_log)
      fprintf(debug_adapter_log, "%s\n", error);
    send_output(STDERR, error, strlen(error));
  }
}

static inline void launch_target_in_terminal(const char* comm_file, const int argc, char* const argv[]) {
  fprintf(stderr, "launch_target_in_terminal is not implemented\n");
  exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
  // setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

  // Allocate and compute absolute path to the adapter
  debug_adapter_path = get_absolute_path(argv[0]);

  int launch_target_pos = find_last_arg_with_value("launch-target", argc, argv);
  if (launch_target_pos) {
    const int comm_file_pos = find_last_arg_with_value("comm-file", launch_target_pos, argv);
    if (!comm_file_pos) {
      fprintf(stderr, "--launch-target option requires --comm-file to be specified\n");
      return EXIT_FAILURE;
    }
    ++launch_target_pos;
    const char* comm_path = argv[comm_file_pos + 1];
    char* const* launch_target_argv = argv + launch_target_pos;
    const int launch_target_argc = argc - launch_target_pos;
    launch_target_in_terminal(comm_path, launch_target_argc, launch_target_argv);
  }

  redirect_output(stdout, STDOUT);
//  redirect_output(stderr, STDERR);

  for (int i = 0; i < 1000; i++)
    printf("abc %d\n", i);

  fflush(stdout);
  sleep(1);
  free_path(debug_adapter_path);
  return EXIT_SUCCESS;
}