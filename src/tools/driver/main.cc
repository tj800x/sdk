// Copyright (c) 2015, the Dartino project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#include <arpa/inet.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/shared/assert.h"
#include "src/shared/globals.h"
#include "src/shared/native_socket.h"
#include "src/shared/platform.h"
#include "src/shared/utils.h"
#include "src/shared/version.h"
#include "src/tools/driver/connection.h"
#include "src/tools/driver/platform.h"

// Fast front-end for persistent compiler process.
//
// To obtain the required performance of command line tools, the dartino
// compiler based on dart2js needs to stay persistent in memory. The start-up
// time of the Dart VM, and its performance of unoptimized code make this
// necessary.
//
// An alternative would be to have a small Dart program connect to the VM, but
// measurements show this C++ program to be 10-20 times faster than a
// hello-world program in Dart.
//
// If the persistent process isn't running, it will be started by this program.
//
// Consequently, this process always communicates with a server process that
// isn't considered a child of itself.
//
// Details about starting the server: to avoid starting multiple servers, this
// program attempts obtain an exclusive lock during the initial handshake with
// the server. If the server doesn't respond, it is started, and the lock isn't
// released until the server is ready.

namespace dartino {

static const int COMPILER_CRASHED = 253;

static char* program_name = NULL;

static bool is_batch_command = false;

// The file where this program looks for the TCP/IP port for talking
// to the persistent process. Controlled by user by setting
// environment variable DARTINO_PORT_FILE.
static char dartino_config_file[MAXPATHLEN];

// The port that was read from [dartino_config_file].
static int dartino_socket_port;

static const char dartino_config_name[] = ".dartino";

static const char dartino_config_env_name[] = "DARTINO_PORT_FILE";

static const char* dartino_config_location = NULL;

static int dartino_config_fd;

static const char dart_vm_env_name[] = "DART_VM";

static const char interactive_token[] = "interactive";

static const char detached_token[] = "detached";

static int exit_code = COMPILER_CRASHED;

static int daemon_stderr = -1;

static pid_t daemon_pid = -1;

static void WriteFully(int fd, uint8* data, ssize_t length);

void Die(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\n");
  exit(255);
}

static char* StrAlloc(size_t size) {
  char* result = static_cast<char*>(calloc(size, sizeof(char)));
  if (result == NULL) {
    Die("%s: malloc failed: %s", program_name, strerror(errno));
  }
  return result;
}

static void StrCpy(void* destination, size_t destination_size,
                   const void* source, size_t source_size) {
  const void* source_end = memchr(source, '\0', source_size);
  if (source_end == NULL) {
    Die("%s: source isn't zero-terminated.", program_name);
  }
  size_t source_length = reinterpret_cast<const uint8*>(source_end) -
                         reinterpret_cast<const uint8*>(source);
  // Increment source_length to include '\0'.
  source_length++;

  if (source_length > destination_size) {
    Die("%s: not enough room in destination (%i) for %i bytes.", program_name,
        destination_size, source_length);
  }

  memcpy(destination, source, source_length);
}

static char* StrDup(const void* source, size_t source_size) {
  char* destination = StrAlloc(source_size);
  StrCpy(destination, source_size, source, source_size);
  return destination;
}

static void StrCat(void* destination, size_t destination_size,
                   const void* source, size_t source_size) {
  void* destination_end = memchr(destination, '\0', destination_size);
  if (destination_end == NULL) {
    Die("%s: destination isn't zero-terminated.", program_name);
  }
  size_t destination_length = reinterpret_cast<uint8*>(destination_end) -
                              reinterpret_cast<uint8*>(destination);

  StrCpy(destination_end, destination_size - destination_length, source,
         source_size);
}

static void Close(int fd) {
  if (TEMP_FAILURE_RETRY(close(fd)) == -1) {
    Die("%s: close failed: %s", program_name, strerror(errno));
  }
}

dev_t GetDevice(const char* name) {
  struct stat info;
  if (stat(name, &info) != 0) {
    Die("%s: Unable to stat '%s': %s", program_name, name, strerror(errno));
  }
  return info.st_dev;
}

bool FileExists(const char* name) {
  struct stat info;
  if (stat(name, &info) == 0) {
    return (info.st_mode & S_IFREG) != 0;
  }
  return false;
}

void DartinoConfigFile(char* result, const char* directory) {
  // TODO(ahe): Use StrCat or StrCpy instead.
  char* ptr = stpncpy(result, directory, MAXPATHLEN);
  if (ptr[-1] != '/') {
    ptr[0] = '/';
    ptr++;
  }
  // TODO(ahe): Use StrCat or StrCpy instead.
  strncpy(ptr, dartino_config_name, sizeof(dartino_config_name));
}

void ParentDir(char* directory) {
  char copy[MAXPATHLEN + 1];
  // On Linux, dirname's argument may be modified. On Mac OS X, it returns a
  // pointer to internal memory. Probably not thread safe. So we first copy
  // directory to a place we don't mind getting modified.
  // TODO(ahe): Use StrCat or StrCpy instead.
  strncpy(copy, directory, MAXPATHLEN);
  char* parent = dirname(copy);
  if (parent == NULL) {
    Die("%s: Unable to compute parent directory of '%s': %s", program_name,
        directory, strerror(errno));
  }
  // TODO(ahe): Use StrCat or StrCpy instead.
  strncpy(directory, parent, MAXPATHLEN);
}

// Detect the configuration and initialize the following variables:
//
// * dartino_config_file
//
// We first look for an environment variable named DARTINO_PORT_FILE. If
// defined, it gives the value of dartino_config_file.
//
// If DARTINO_PORT_FILE isn't defined, we look for the environment variable
// HOME, if defined, the value of dartino_config_file becomes
// "${HOME}/.dartino".
//
// If HOME isn't defined, we find the user's home directory via getpwuid_r.
static void DetectConfiguration() {
  // First look for the environment variable DARTINO_PORT_FILE.
  char* dartino_config_env = getenv(dartino_config_env_name);
  if (dartino_config_env != NULL) {
    dartino_config_location = dartino_config_env_name;
    StrCpy(dartino_config_file, sizeof(dartino_config_file), dartino_config_env,
           strlen(dartino_config_env) + 1);
    return;
  }

  // Then look for the environment variable HOME.
  char* home_env = getenv("HOME");
  if (home_env != NULL) {
    dartino_config_location = "HOME";
    DartinoConfigFile(dartino_config_file, home_env);
    return;
  }

  // Fall back to getpwuid_r for obtaining the home directory.
  int pwd_buffer_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (pwd_buffer_size == -1) {
    // On Linux, we can't guarantee a sensible return value. So let's assume
    // that each char* in struct passwd are less than MAXPATHLEN. There are 5
    // of those, and then one extra for null-termination and good measure.
    pwd_buffer_size = MAXPATHLEN * 6;
  }
  char* pwd_buffer = StrAlloc(pwd_buffer_size);

  struct passwd pwd;
  struct passwd* result = NULL;
  int error_code =
      getpwuid_r(getuid(), &pwd, pwd_buffer, pwd_buffer_size, &result);
  if (error_code != 0) {
    Die("%s: Unable to determine home directory: %s", program_name,
        strerror(error_code));
  }
  if (result == NULL) {
    Die("%s: Unable to determine home directory: Entry for user not found.",
        program_name);
  }
  dartino_config_location = "/etc/passwd";
  DartinoConfigFile(dartino_config_file, pwd.pw_dir);
  free(pwd_buffer);
}

// Opens and locks the config file named by dartino_config_file and initialize
// the variable dartino_config_fd. If use_blocking is true, this method will
// block until the lock is obtained.
static void LockConfigFile(bool use_blocking) {
  int fd = TEMP_FAILURE_RETRY(
      open(dartino_config_file, O_RDONLY | O_CREAT, S_IRUSR | S_IWUSR));
  if (fd == -1) {
    Die("%s: Unable to open '%s' failed: %s.\nTry checking the value of '%s'.",
        program_name, dartino_config_file, strerror(errno),
        dartino_config_location);
  }

  int operation = LOCK_EX;
  if (!use_blocking) {
    operation |= LOCK_NB;
  }
  if (TEMP_FAILURE_RETRY(flock(fd, operation)) == -1) {
    if (use_blocking || errno != EWOULDBLOCK) {
      Die("%s: flock '%s' failed: %s.", program_name, dartino_config_file,
          strerror(errno));
    }
  }

  dartino_config_fd = fd;
}

// Release the lock on dartino_config_fd.
static void UnlockConfigFile() {
  // Closing the file descriptor will release the lock.
  Close(dartino_config_fd);
}

static void ReadDriverConfig() {
  char buffer[80];
  size_t offset = 0;
  size_t length = sizeof(buffer) - 1;
  while (offset < length) {
    ssize_t bytes = TEMP_FAILURE_RETRY(
        read(dartino_config_fd, buffer + offset, length - offset));
    if (bytes < 0) {
      Die("%s: Unable to read from '%s'. Failed with error: %s", program_name,
          dartino_config_file, strerror(errno));
    } else if (bytes == 0) {
      break;  // End of file.
    }
    offset += bytes;
  }
  buffer[offset] = '\0';
  dartino_socket_port = atoi(buffer);
}

static void ComputeDartinoRoot(char* buffer, size_t buffer_length) {
  // TODO(ahe): Fix lint problem: Do not use variable-length arrays.
  char resolved[buffer_length];  // NOLINT
  GetPathOfExecutable(buffer, buffer_length);
  if (realpath(buffer, resolved) == NULL) {
    Die("%s: realpath of '%s' failed: %s", program_name, buffer,
        strerror(errno));
  }
  StrCpy(buffer, buffer_length, resolved, sizeof(resolved));

  // 'buffer' is now the absolute path of this executable (with symlinks
  // resolved). When running from dartino-repo, this executable will be in
  // "dartino-repo/dartino/out/$CONFIGURATION/dartino".
  ParentDir(buffer);
  // 'buffer' is now, for example, "dartino-repo/dartino/out/$CONFIGURATION".

  // DARTINO_ROOT_DISTANCE gives the number of directories up that we find the
  // root of the dartino checkout or sdk bundle.
  for (int i = 0; i < DARTINO_ROOT_DISTANCE; i++) {
    ParentDir(buffer);
  }

  size_t length = strlen(buffer);
  if (length > 0 && buffer[length - 1] != '/') {
    // Append trailing slash.
    StrCat(buffer, buffer_length, "/", 2);
  }
}

static void GetExecutableDir(char* buffer, size_t buffer_length) {
  // TODO(ahe): Fix lint problem: Do not use variable-length arrays.
  char resolved[buffer_length];  // NOLINT
  GetPathOfExecutable(buffer, buffer_length);
  if (realpath(buffer, resolved) == NULL) {
    Die("%s: realpath of '%s' failed: %s", program_name, buffer,
        strerror(errno));
  }
  StrCpy(buffer, buffer_length, resolved, sizeof(resolved));

  // 'buffer' is now the absolute path of this executable (with symlinks
  // resolved). When running from dartino-repo, this executable will be in
  // "dartino-repo/dartino/out/$CONFIGURATION/dartino".
  ParentDir(buffer);
  // 'buffer' is now, for example, "dartino-repo/dartino/out/$CONFIGURATION".

  size_t length = strlen(buffer);
  if (length > 0 && buffer[length - 1] != '/') {
    // Append trailing slash.
    StrCat(buffer, buffer_length, "/", 2);
  }
}

// Stores the location of the Dart VM in 'buffer'.
static void ComputeDartVmPath(char* buffer, size_t buffer_length) {
  char* dart_vm_env = getenv(dart_vm_env_name);
  if (dart_vm_env != NULL) {
    if (realpath(dart_vm_env, buffer) == NULL) {
      Die("%s: realpath of '%s' failed: %s", program_name, dart_vm_env,
          strerror(errno));
    }
    return;
  }

  GetExecutableDir(buffer, buffer_length);
  StrCat(buffer, buffer_length, DART_VM_NAME, sizeof(DART_VM_NAME));
  // 'buffer' is now, for example,
  // "dartino-repo/dartino/out/$CONFIGURATION/dart".
}

// Stores the location of the Dartino VM in 'buffer'.
static void ComputeDartinoVmPath(char* buffer, size_t buffer_length) {
  GetExecutableDir(buffer, buffer_length);

  StrCat(buffer, buffer_length, "dartino-vm", sizeof("dartino-vm"));
  // 'buffer' is now, for example,
  // "dartino-repo/dartino/out/$CONFIGURATION/dart".
}

// Stores the package root in 'buffer'. The value of 'dartino_root' must be the
// absolute path of '.../dartino-repo/dartino/' (including trailing slash).
static void ComputePackageSpec(char* buffer, size_t buffer_length,
                               const char* dartino_root,
                               size_t dartino_root_length) {
  StrCpy(buffer, buffer_length, dartino_root, dartino_root_length);
  StrCat(buffer, buffer_length, DARTINOC_PKG_FILE, sizeof(DARTINOC_PKG_FILE));
  // 'buffer' is now, for example, "dartino-repo/dartino/package/".
}

// Flush all open streams (FILE objects). This is needed before forking
// (otherwise, buffered data will get duplicated in the children leading to
// duplicated output). It is also needed before using file descriptors, as I/O
// based on file descriptors bypass any buffering in streams.
void FlushAllStreams() {
  if (fflush(NULL) != 0) {
    Die("%s: fflush failed: %s", program_name, strerror(errno));
  }
}

pid_t Fork() {
  FlushAllStreams();

  pid_t pid = fork();
  if (pid == -1) {
    Die("%s: fork failed: %s", program_name, strerror(errno));
  }
  return pid;
}

const int kMaxArgc = 10;

static void InitializeDaemonArguments(char** argv, const char* program) {
  char dartino_root[MAXPATHLEN + 1];
  ComputeDartinoRoot(dartino_root, sizeof(dartino_root));

  char vm_path[MAXPATHLEN + 1];
  ComputeDartVmPath(vm_path, sizeof(vm_path));

  char dartino_vm_path[MAXPATHLEN + 1];
  ComputeDartinoVmPath(dartino_vm_path, sizeof(dartino_vm_path));

  char dartino_vm_option[sizeof("-Ddartino-vm=") + MAXPATHLEN + 1];
  StrCpy(dartino_vm_option, sizeof(dartino_vm_option), "-Ddartino-vm=",
         sizeof("-Ddartino-vm="));
  StrCat(dartino_vm_option, sizeof(dartino_vm_option), dartino_vm_path,
         sizeof(dartino_vm_path));

  char package_spec[MAXPATHLEN + 1];
  ComputePackageSpec(package_spec, sizeof(package_spec), dartino_root,
                     sizeof(dartino_root));
  char package_option[sizeof("--packages=") + MAXPATHLEN + 1];
  StrCpy(package_option, sizeof(package_option), "--packages=",
         sizeof("--packages="));
  StrCat(package_option, sizeof(package_option), package_spec,
         sizeof(package_spec));

  const char library_root[] =
      "-Ddartino_compiler-library-root=" DARTINOC_LIBRARY_ROOT;
  const char define_version[] = "-Ddartino.version=";
  const char* version = GetVersion();
  int version_option_length = sizeof(define_version) + strlen(version) + 1;
  char* version_option = StrAlloc(version_option_length);
  StrCpy(version_option, version_option_length, define_version,
         sizeof(define_version));
  StrCat(version_option, version_option_length, version, strlen(version) + 1);

  const char checked_mode[] = "-c";
  const char batch_mode_true[] = "-Ddartino-batch-mode=true";
  const char batch_mode_false[] = "-Ddartino-batch-mode=false";

  // Below we use StrDup to ensure all entries in argv are allocated. This
  // makes it easier to free them all later, which allows us to use ASAN.
  int argc = 0;
  argv[argc++] = StrDup(vm_path, sizeof(vm_path));
  argv[argc++] = StrDup(checked_mode, sizeof(checked_mode));
  argv[argc++] = StrDup(dartino_vm_option, sizeof(dartino_vm_option));
  argv[argc++] = StrDup(package_option, sizeof(package_option));
  argv[argc++] = version_option;
  argv[argc++] = StrDup(library_root, sizeof(library_root));
  if (is_batch_command) {
    argv[argc++] = StrDup(batch_mode_true, sizeof(batch_mode_true));
  } else {
    argv[argc++] = StrDup(batch_mode_false, sizeof(batch_mode_false));
  }
  argv[argc++] = StrDup(program, strlen(program) + 1);
  argv[argc++] = StrDup(dartino_config_file, sizeof(dartino_config_file));
  argv[argc++] = NULL;
  if (argc > kMaxArgc) Die("Internal error: increase kMaxArgc");
}

static void FreeDaemonArguments(char** argv) {
  for (int i = 0; argv[i] != NULL; i++) {
    free(argv[i]);
    argv[i] = NULL;
  }
}

static void WaitForDaemonHandshake(pid_t pid, int parent_stdout,
                                   int parent_stderr);

static void ExecDaemon(int child_stdout, int child_stderr, char** argv);

static void StartDriverDaemon() {
  char* argv[kMaxArgc];

  InitializeDaemonArguments(argv,
                            "package:dartino_compiler/src/hub/hub_main.dart");

  int file_descriptors[2];
  if (pipe(file_descriptors) != 0) {
    Die("%s: pipe failed: %s", program_name, strerror(errno));
  }
  int parent_stdout = file_descriptors[0];
  int child_stdout = file_descriptors[1];

  int parent_stderr = -1;
  int child_stderr = -1;
  if (!is_batch_command) {
    if (pipe(file_descriptors) != 0) {
      Die("%s: pipe failed: %s", program_name, strerror(errno));
    }
    parent_stderr = file_descriptors[0];
    child_stderr = file_descriptors[1];
  }

  pid_t pid = Fork();
  if (pid == 0) {
    // In child.
    Close(parent_stdout);
    if (!is_batch_command) {
      Close(parent_stderr);
      Close(dartino_config_fd);
    }
    ExecDaemon(child_stdout, child_stderr, argv);
    UNREACHABLE();
  } else {
    FreeDaemonArguments(argv);
    Close(child_stdout);
    if (!is_batch_command) Close(child_stderr);
    WaitForDaemonHandshake(pid, parent_stdout, parent_stderr);
  }
}

static void NewProcessSession() {
  pid_t sid = setsid();
  if (sid < 0) {
    Die("%s: setsid failed: %s", program_name, strerror(errno));
  }
}

static void Dup2(int source, int destination) {
  if (TEMP_FAILURE_RETRY(dup2(source, destination)) == -1) {
    Die("%s: dup2 failed: %s", program_name, strerror(errno));
  }
}

static void ExecDaemon(int child_stdout, int child_stderr, char** argv) {
  Close(STDIN_FILENO);

  // Change directory to '/' to ensure that we use the client's working
  // directory.
  if (TEMP_FAILURE_RETRY(chdir("/")) == -1) {
    Die("%s: 'chdir(\"/\")' failed: %s", program_name, strerror(errno));
  }

  if (!is_batch_command) {
    // Calling fork one more time to create an indepent processs. This prevents
    // zombie processes, and ensures the server can continue running in the
    // background independently of the parent process.
    if (Fork() > 0) {
      // This process exits and leaves the new child as an independent process.
      exit(0);
    }

    // Create a new session (to avoid getting killed by SIGHUP, etc.).
    NewProcessSession();
  }

  // This is the child process that will exec the persistent server. We don't
  // want this server to be associated with the current terminal, so we must
  // close stdin (handled above), stdout, and stderr. This is accomplished by
  // redirecting stdout and stderr to the pipes to the parent process.
  Dup2(child_stdout, STDOUT_FILENO);  // Closes stdout.
  Close(child_stdout);

  if (!is_batch_command) {
    Dup2(child_stderr, STDERR_FILENO);  // Closes stderr.
    Close(child_stderr);
  }

  execv(argv[0], const_cast<char**>(argv));
  Die("%s: exec '%s' failed: %s", program_name, argv[0], strerror(errno));
}

static ssize_t Read(int fd, uint8* buffer, size_t buffer_length) {
  ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd, buffer, buffer_length));
  if (bytes_read < 0) {
    Die("%s: read failed: %s", program_name, strerror(errno));
  }
  return bytes_read;
}

// Forwards data on file descriptor "from" to "to" using the buffer. Errors are
// fatal. Returns true if "from" was closed.
static bool ForwardWithBuffer(int from, int to, uint8* buffer,
                              ssize_t* buffer_length) {
  ssize_t bytes_read = Read(from, buffer, *buffer_length);
  *buffer_length = bytes_read;
  if (bytes_read == 0) return true;

  // Flushing all streams in case one of them has buffered data for "to" file
  // descriptor.
  FlushAllStreams();

  WriteFully(to, buffer, bytes_read);
  return false;
}

static void WaitForDaemon(pid_t pid) {
  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status)) {
    Die("%s: child process failed.", program_name);
  }
  status = WEXITSTATUS(status);
  if (status != 0) {
    Die("%s: child process exited with non-zero exit code %i.", program_name,
        status);
  }
}

static void WaitForDaemonHandshake(pid_t pid, int parent_stdout,
                                   int parent_stderr) {
  if (is_batch_command) {
    daemon_pid = pid;
  } else {
    WaitForDaemon(pid);
  }

  char stdout_buffer[4096];
  stdout_buffer[0] = '\0';
  bool stdout_is_closed = false;
  bool stderr_is_closed = parent_stderr == -1;
  while (!stdout_is_closed || !stderr_is_closed) {
    uint8 buffer[4096];
    fd_set readfds;
    int max_fd = 0;
    FD_ZERO(&readfds);
    if (!stdout_is_closed) {
      FD_SET(parent_stdout, &readfds);
      max_fd = Utils::Maximum(max_fd, parent_stdout);
    }
    if (!stderr_is_closed) {
      FD_SET(parent_stderr, &readfds);
      max_fd = Utils::Maximum(max_fd, parent_stderr);
    }
    int ready_count =
        TEMP_FAILURE_RETRY(select(max_fd + 1, &readfds, NULL, NULL, NULL));
    if (ready_count < 0) {
      fprintf(stderr, "%s: select error: %s\n", program_name, strerror(errno));
      break;
    } else if (ready_count == 0) {
      // Timeout, shouldn't happen.
    } else {
      if (!stderr_is_closed && FD_ISSET(parent_stderr, &readfds)) {
        ssize_t bytes_read = sizeof(buffer);
        stderr_is_closed = ForwardWithBuffer(parent_stderr, STDERR_FILENO,
                                             buffer, &bytes_read);
      }
      if (FD_ISSET(parent_stdout, &readfds)) {
        ssize_t bytes_read = Read(parent_stdout, buffer, sizeof(buffer) - 1);
        stdout_is_closed = bytes_read == 0;
        buffer[bytes_read] = '\0';
        StrCat(stdout_buffer, sizeof(stdout_buffer), buffer, sizeof(buffer));
        // At this point, stdout_buffer contains all the data we have
        // received from the server process via its stdout. We're looking for
        // a handshake which is a port number on the first line. So we look for
        // a newline character.
        char* match = strchr(stdout_buffer, '\n');
        if (match != NULL) {
          // If we do not have a precise match the VM is printing
          // something unexpected and we print it out to make
          // debugging easier.
          if (match[1] != '\0') {
            FlushAllStreams();
            WriteFully(STDOUT_FILENO, reinterpret_cast<uint8*>(buffer),
                       bytes_read);
          }
          match[0] = '\0';
          dartino_socket_port = atoi(stdout_buffer);
          // We got the server handshake (the port). So we break to
          // eventually return from this function.
          break;
        }
      }
    }
  }
  if (!is_batch_command) {
    Close(parent_stdout);
    Close(parent_stderr);
  } else {
    daemon_stderr = parent_stderr;
  }
}

static void WriteFully(int fd, uint8* data, ssize_t length) {
  ssize_t offset = 0;
  while (offset < length) {
    int bytes = TEMP_FAILURE_RETRY(write(fd, data + offset, length - offset));
    if (bytes < 0) {
      Die("%s: write failed: %s", program_name, strerror(errno));
    }
    offset += bytes;
  }
}

static void SendArgv(DriverConnection* connection, int argc, char** argv,
                     bool is_interactive) {
  WriteBuffer buffer;
  buffer.WriteInt(argc + 3);  // Also version and current directory
                              // and whether the app is run interactively

  buffer.WriteInt(strlen(GetVersion()));
  buffer.WriteString(GetVersion());

  char* path = StrAlloc(MAXPATHLEN + 1);

  if (getcwd(path, MAXPATHLEN + 1) == NULL) {
    Die("%s: getcwd failed: %s", path, strerror(errno));
  }
  buffer.WriteInt(strlen(path));
  buffer.WriteString(path);

  free(path);
  path = NULL;

  if (is_interactive) {
    buffer.WriteInt(strlen(interactive_token));
    buffer.WriteString(interactive_token);
  } else {
    buffer.WriteInt(strlen(detached_token));
    buffer.WriteString(detached_token);
  }

  for (int i = 0; i < argc; i++) {
    buffer.WriteInt(strlen(argv[i]));
    buffer.WriteString(argv[i]);
  }
  connection->Send(DriverConnection::kArguments, buffer);
}

static int CommandFileDescriptor(DriverConnection::Command command) {
  switch (command) {
    case DriverConnection::kStdin:
    case DriverConnection::kStdout:
    case DriverConnection::kStderr:
      return command;

    default:
      Die("%s: No file descriptor for command: %i\n", program_name, command);
      return -1;
  }
}

static DriverConnection::Command HandleCommand(DriverConnection* connection) {
  DriverConnection::Command command = connection->Receive();
  switch (command) {
    case DriverConnection::kExitCode:
      exit_code = connection->ReadInt();
      return command;

    case DriverConnection::kStdout:
    case DriverConnection::kStderr: {
      int size = 0;
      uint8* bytes = connection->ReadBytes(&size);
      WriteFully(CommandFileDescriptor(command), bytes, size);
      free(bytes);
      return command;
    }

    case DriverConnection::kDriverConnectionError:
    case DriverConnection::kDriverConnectionClosed:
      return command;

    default:
      break;
  }

  Die("%s: Unhandled command code: %i\n", program_name, command);
  return DriverConnection::kDriverConnectionError;
}

Socket* Connect() {
  struct sockaddr_in address;

  address.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  address.sin_port = htons(dartino_socket_port);

  int fd = socket(PF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    Die("%s: socket failed: %s", program_name, strerror(errno));
  }
  Socket* socket = Socket::FromFd(fd);

  int connect_result = TEMP_FAILURE_RETRY(connect(
      fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)));
  if (connect_result != 0) {
    delete socket;
    return NULL;
  } else {
    return socket;
  }
}

static void HandleSignal(int signal_pipe, DriverConnection* connection) {
  WriteBuffer buffer;
  int signal = ReadSignal(signal_pipe);
  buffer.WriteInt(signal);
  connection->Send(DriverConnection::kSignal, buffer);
}

static int CheckedSystem(const char* command) {
  int exit_status = system(command);
  if (exit_status == -1) {
    Die("%s: system(%s) failed with error: %s", program_name, command,
        strerror(errno));
  }
  if (WIFSIGNALED(exit_status)) {
    // command exited due to signal, for example, the user pressed Ctrl-C. In
    // that case, we should also exit.
    Exit(-WTERMSIG(exit_status));
  }
  return exit_status;
}

// Kills the persistent process. First tries SIGTERM, then SIGKILL if the
// process hasn't exited after 2 seconds.
// The process is identified using lsof on the Dart VM binary.
static int QuitCommand() {
  const int MAX_COMMAND_LENGTH = 256;
  char command[MAX_COMMAND_LENGTH];
  // We used exec to avoid having pkill the /bin/sh parent process it is running
  // as a child of when redirecting to /dev/null.
  const char pkill[] = "exec pkill -f ";
  const char pkill_force[] = "exec pkill -KILL -f ";
  const char driver_arguments[] =
      "package:dartino_compiler/src/driver/driver_main > /dev/null";
  const char hub_arguments[] =
      "package:dartino_compiler/src/hub/hub_main > /dev/null";

  StrCpy(command, MAX_COMMAND_LENGTH, pkill, sizeof(pkill));
  StrCat(command, MAX_COMMAND_LENGTH, hub_arguments, sizeof(hub_arguments));

  const char* current_arguments = hub_arguments;
  // pkill -f package:dartino_compiler/src/hub/hub_main
  if (CheckedSystem(command) != 0) {
    // pkill returns 0 if it killed any processes, so in this case it didn't
    // find/kill any active persistent processes
    // Try with the legacy driver_main path to see if an old persistent process
    // was running.
    StrCpy(command, MAX_COMMAND_LENGTH, pkill, sizeof(pkill));
    StrCat(command, MAX_COMMAND_LENGTH, driver_arguments,
        sizeof(driver_arguments));
    // pkill -f package:dartino_compiler/src/driver/driver_main
    if (CheckedSystem(command) != 0) {
      // No legacy persistent process. Just remove the socket location file.
      unlink(dartino_config_file);
      printf("Background process wasn't running\n");
      return 0;
    }
    current_arguments = driver_arguments;
  }

  // Wait two seconds for the process to exit gracefully.
  sleep(2);

  // Remove the socket location file.
  unlink(dartino_config_file);

  // To check if the process exited gracefully we try to kill it again
  // (this time with SIGKILL). If that command doesn't find any running
  // processes it will return 1. If it finds one or more running instance
  // it returns 0 in which case we know it didn't shutdown gracefully above.
  // We use the return value to decide what to report to the user.
  StrCpy(command, MAX_COMMAND_LENGTH, pkill_force, sizeof(pkill_force));
  StrCat(command, MAX_COMMAND_LENGTH, current_arguments,
     strlen(current_arguments) + 1);

  // pkill -KILL -f package:dartino_compiler/src/hub/hub_main or
  // pkill -KILL -f package:dartino_compiler/src/driver/driver_main depending
  // on the above pkill.
  if (CheckedSystem(command) != 0) {
    // We assume it didn't find any processes to kill when returning a
    // non-zero value and hence just report the process gracefully exited.
    printf("Background process exited\n");
  } else {
    printf(
        "The background process didn't exit after 2 seconds. "
        "Forcefully quit the background process.\n");
  }
  return 0;
}

// Detects if [argv] is a batch command. A batch command is recognized by the
// absense of "session name" on the command line.
static bool IsBatchCommand(int argc, char** argv) {
  for (int i = 1; i < argc - 1; i++) {
    if (strcmp("session", argv[i]) == 0) {
      return false;
    }
  }
  return true;
}

static int Main(int argc, char** argv) {
  program_name = argv[0];
  is_batch_command = IsBatchCommand(argc, argv);
  DetectConfiguration();
  bool is_quit_command = (argc == 2 && strcmp("quit", argv[1]) == 0);
  if (!is_batch_command || is_quit_command) {
    LockConfigFile(!is_quit_command);
  }
  if (!is_batch_command) ReadDriverConfig();

  if (is_quit_command) {
    return QuitCommand();
  }

  Socket* control_socket = NULL;
  if (!is_batch_command) {
    control_socket = Connect();
  }
  if (control_socket == NULL) {
    StartDriverDaemon();

    control_socket = Connect();
    if (control_socket == NULL) {
      Die(
          "%s: Failed to start dartino server (%s).\n"
          "Use DART_VM environment variable to override location of Dart VM.",
          program_name, strerror(errno));
    }
  }

  if (!is_batch_command) {
    UnlockConfigFile();
  }

  int signal_pipe = SignalFileDescriptor();

  DriverConnection* connection = new DriverConnection(control_socket);

  // Determine if the app is being run interactively.
  int stdintty = isatty(fileno(stdin));
  int stdouttty = isatty(fileno(stdout));
  bool is_interactive = stdintty && stdouttty;

  SendArgv(connection, argc, argv, is_interactive);

  FlushAllStreams();

  int max_fd = Utils::Maximum(STDIN_FILENO, control_socket->FileDescriptor());
  max_fd = Utils::Maximum(max_fd, signal_pipe);
  max_fd = Utils::Maximum(max_fd, daemon_stderr);
  bool stdin_closed = false;
  uint8 buffer[4096];
  while (true) {
    fd_set readfds;
    FD_ZERO(&readfds);
    if (!stdin_closed) {
      FD_SET(STDIN_FILENO, &readfds);
    }
    if (control_socket != NULL) {
      FD_SET(control_socket->FileDescriptor(), &readfds);
    }
    FD_SET(signal_pipe, &readfds);

    int ready_count =
        TEMP_FAILURE_RETRY(select(max_fd + 1, &readfds, NULL, NULL, NULL));
    if (ready_count < 0) {
      fprintf(stderr, "%s: select error: %s\n", program_name, strerror(errno));
      break;
    } else if (ready_count == 0) {
      // Timeout, shouldn't happen.
    } else {
      if (FD_ISSET(signal_pipe, &readfds)) {
        HandleSignal(signal_pipe, connection);
      }
      if ((daemon_stderr != -1) && FD_ISSET(daemon_stderr, &readfds)) {
        ssize_t bytes_read = sizeof(buffer);
        if (ForwardWithBuffer(daemon_stderr, STDERR_FILENO,
                              buffer, &bytes_read)) {
          daemon_stderr = -1;
        }
      }

      if (FD_ISSET(STDIN_FILENO, &readfds)) {
        ssize_t bytes_count =
            TEMP_FAILURE_RETRY(read(STDIN_FILENO, &buffer, sizeof(buffer)));
        if (bytes_count >= 0) {
          WriteBuffer write_buffer;
          write_buffer.WriteBytes(buffer, bytes_count);
          connection->Send(DriverConnection::kStdin, write_buffer);
          if (bytes_count == 0) {
            close(STDIN_FILENO);
            stdin_closed = true;
          }
        } else if (bytes_count < 0) {
          Die("%s: Error reading from stdin: %s", program_name,
              strerror(errno));
        }
      }
      if (control_socket != NULL &&
          FD_ISSET(control_socket->FileDescriptor(), &readfds)) {
        DriverConnection::Command command = HandleCommand(connection);
        if (command == DriverConnection::kDriverConnectionError) {
          Die("%s: lost connection to persistent process: %s", program_name,
              strerror(errno));
        } else if (command == DriverConnection::kDriverConnectionClosed) {
          // Connection was closed.
          delete control_socket;
          control_socket = NULL;
        }
      }
      if (control_socket == NULL && daemon_stderr == -1) break;
    }
  }

  // Analytics (among other things) cause dartino daemon to take an additional
  // 800 to 1500ms after the loop above completes and before it shuts down.
  // Because of this delay, waiting for the daemon to exit
  // makes the command line feel sluggish.
  // If this program is being run interactively, then
  // do not wait for the daemon to exit so that the cmdline feels responsive.
  if (!is_interactive && daemon_pid != -1) WaitForDaemon(daemon_pid);

  Exit(exit_code);
  return exit_code;
}

}  // namespace dartino

// Forward main calls to dartino::Main.
int main(int argc, char** argv) { return dartino::Main(argc, argv); }
