// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "subprocess.h"

#include <sys/select.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <spawn.h>

extern char** environ;

#include "util.h"

Subprocess::Subprocess(bool use_console) : fd_(-1), pid_(-1),
                                           use_console_(use_console) {
}

Subprocess::~Subprocess() {
  if (fd_ >= 0)
    close(fd_);
  // Reap child if forgotten.
  if (pid_ != -1)
    Finish();
}

bool Subprocess::Start(SubprocessSet* set, const string& command) {
  int output_pipe[2];
  if (pipe(output_pipe) < 0)
    Fatal("pipe: %s", strerror(errno));
  fd_ = output_pipe[0];
#if !defined(USE_PPOLL)
  // If available, we use ppoll in DoWork(); otherwise we use pselect
  // and so must avoid overly-large FDs.
  if (fd_ >= static_cast<int>(FD_SETSIZE))
    Fatal("pipe: %s", strerror(EMFILE));
#endif  // !USE_PPOLL
  SetCloseOnExec(fd_);

  posix_spawn_file_actions_t action;
  int err = posix_spawn_file_actions_init(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_init: %s", strerror(err));

  err = posix_spawn_file_actions_addclose(&action, output_pipe[0]);
  if (err != 0)
    Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));

  posix_spawnattr_t attr;
  err = posix_spawnattr_init(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_init: %s", strerror(err));

  short flags = 0;

  flags |= POSIX_SPAWN_SETSIGMASK;
  err = posix_spawnattr_setsigmask(&attr, &set->old_mask_);
  if (err != 0)
    Fatal("posix_spawnattr_setsigmask: %s", strerror(err));
  // Signals which are set to be caught in the calling process image are set to
  // default action in the new process image, so no explicit
  // POSIX_SPAWN_SETSIGDEF parameter is needed.

  if (!use_console_) {
    // Put the child in its own process group, so ctrl-c won't reach it.
    flags |= POSIX_SPAWN_SETPGROUP;
    // No need to posix_spawnattr_setpgroup(&attr, 0), it's the default.

    // Open /dev/null over stdin.
    err = posix_spawn_file_actions_addopen(&action, 0, "/dev/null", O_RDONLY,
          0);
    if (err != 0) {
      Fatal("posix_spawn_file_actions_addopen: %s", strerror(err));
    }

    err = posix_spawn_file_actions_adddup2(&action, output_pipe[1], 1);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_adddup2(&action, output_pipe[1], 2);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_addclose(&action, output_pipe[1]);
    if (err != 0)
      Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));
    // In the console case, output_pipe is still inherited by the child and
    // closed when the subprocess finishes, which then notifies ninja.
  }
#ifdef POSIX_SPAWN_USEVFORK
  flags |= POSIX_SPAWN_USEVFORK;
#endif

  err = posix_spawnattr_setflags(&attr, flags);
  if (err != 0)
    Fatal("posix_spawnattr_setflags: %s", strerror(err));

  const char* spawned_args[] = { "/bin/sh", "-c", command.c_str(), NULL };
  err = posix_spawn(&pid_, "/bin/sh", &action, &attr,
        const_cast<char**>(spawned_args), environ);
  if (err != 0)
    Fatal("posix_spawn: %s", strerror(err));

  err = posix_spawnattr_destroy(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_destroy: %s", strerror(err));
  err = posix_spawn_file_actions_destroy(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_destroy: %s", strerror(err));

  close(output_pipe[1]);
  return true;
}

void Subprocess::OnPipeReady() {
  char buf[4 << 10];
  ssize_t len = read(fd_, buf, sizeof(buf));
  if (len > 0) {
    buf_.append(buf, len);
  } else {
    if (len < 0)
      Fatal("read: %s", strerror(errno));
    close(fd_);
    fd_ = -1;
  }
}

ExitStatus Subprocess::Finish() {
  assert(pid_ != -1);
  int status;
  if (waitpid(pid_, &status, 0) < 0)
    Fatal("waitpid(%d): %s", pid_, strerror(errno));
  pid_ = -1;

  if (WIFEXITED(status)) {
    int exit = WEXITSTATUS(status);
    if (exit == 0)
      return ExitSuccess;
  } else if (WIFSIGNALED(status)) {
    if (WTERMSIG(status) == SIGINT || WTERMSIG(status) == SIGTERM
        || WTERMSIG(status) == SIGHUP)
      return ExitInterrupted;
  }
  return ExitFailure;
}

bool Subprocess::Done() const {
  return fd_ == -1;
}

const string& Subprocess::GetOutput() const {
  return buf_;
}

struct TokenStore {
  TokenStore();
  ~TokenStore();

  bool Setup();
  bool Acquire();
  void Reserve();
  void Release();
  void Clear();

 private:
  int available_;
  int used_;
  int rfd_;
  int wfd_;

  bool CheckFd(int fd);
  void Return();
};

TokenStore::TokenStore() : available_(0), used_(0), rfd_(-1), wfd_(-1) {
}

TokenStore::~TokenStore() {
  Clear();
}

bool TokenStore::CheckFd(int fd) {
  if (fd < 0)
    return false;
  int ret = fcntl(fd, F_GETFD);
  if (ret < 0)
    return false;
  return true;
}

bool TokenStore::Setup() {
  const char *value = getenv("MAKEFLAGS");
  if (value) {
    const char *jobserver = strstr(value, "--jobserver-fds=");
    if (jobserver) {
      int rfd = -1;
      int wfd = -1;
      if ((sscanf(jobserver, "--jobserver-fds=%d,%d", &rfd, &wfd) == 2) &&
          CheckFd(rfd) &&
          CheckFd(wfd)) {
        fprintf(stderr, "FOUND JOBSERVER %d %d\n", rfd, wfd);
        rfd_ = rfd;
        wfd_ = wfd;
        return true;
      }
    }
  }

  return false;
}

bool TokenStore::Acquire() {
  if (available_ > 0)
    return true;

  pollfd pollfds[] = {{rfd_, POLLIN, 0}};
  int ret = poll(pollfds, 1, 0);
  if (ret > 0) {
    char buf;
    int ret = read(rfd_, &buf, 1);
    if (ret > 0) {
      available_++;
      return true;
    }
  }
  return false;
}

void TokenStore::Reserve() {
  available_--;
  used_++;
}

void TokenStore::Return() {
  const char buf = '+';
  write(wfd_, &buf, 1);
}

void TokenStore::Release() {
  used_--;
  Return();
}

void TokenStore::Clear() {
  available_ += used_;
  used_       = 0;
  while (available_-- > 0)
    Return();
}

int SubprocessSet::interrupted_;

void SubprocessSet::SetInterruptedFlag(int signum) {
  interrupted_ = signum;
}

void SubprocessSet::HandlePendingInterruption() {
  sigset_t pending;
  sigemptyset(&pending);
  if (sigpending(&pending) == -1) {
    perror("ninja: sigpending");
    return;
  }
  if (sigismember(&pending, SIGINT))
    interrupted_ = SIGINT;
  else if (sigismember(&pending, SIGTERM))
    interrupted_ = SIGTERM;
  else if (sigismember(&pending, SIGHUP))
    interrupted_ = SIGHUP;
}

SubprocessSet::SubprocessSet() : tokens_(NULL) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  if (sigprocmask(SIG_BLOCK, &set, &old_mask_) < 0)
    Fatal("sigprocmask: %s", strerror(errno));

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SetInterruptedFlag;
  if (sigaction(SIGINT, &act, &old_int_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &act, &old_term_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &act, &old_hup_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));

  TokenStore *tokenstore = new TokenStore;
  if (tokenstore->Setup())
    tokens_ = tokenstore;
  else
    delete tokenstore;
}

SubprocessSet::~SubprocessSet() {
  Clear();

  if (sigaction(SIGINT, &old_int_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &old_term_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &old_hup_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigprocmask(SIG_SETMASK, &old_mask_, 0) < 0)
    Fatal("sigprocmask: %s", strerror(errno));
}

Subprocess *SubprocessSet::Add(const string& command, bool use_console) {
  Subprocess *subprocess = new Subprocess(use_console);
  if (!subprocess->Start(this, command)) {
    delete subprocess;
    return 0;
  }
  if (tokens_ && !running_.empty())
    tokens_->Reserve();
  running_.push_back(subprocess);
  return subprocess;
}

#ifdef USE_PPOLL
bool SubprocessSet::DoWork() {
  vector<pollfd> fds;
  nfds_t nfds = 0;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    int fd = (*i)->fd_;
    if (fd < 0)
      continue;
    pollfd pfd = { fd, POLLIN | POLLPRI, 0 };
    fds.push_back(pfd);
    ++nfds;
  }

  interrupted_ = 0;
  int ret = ppoll(&fds.front(), nfds, NULL, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: ppoll");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  nfds_t cur_nfd = 0;
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    int fd = (*i)->fd_;
    if (fd < 0)
      continue;
    assert(fd == fds[cur_nfd].fd);
    if (fds[cur_nfd++].revents) {
      (*i)->OnPipeReady();
      if ((*i)->Done()) {
        finished_.push(*i);
        i = running_.erase(i);
        if (tokens_ && !running_.empty())
          tokens_->Release();
        continue;
      }
    }
    ++i;
  }

  return IsInterrupted();
}

#else  // !defined(USE_PPOLL)
bool SubprocessSet::DoWork() {
  fd_set set;
  int nfds = 0;
  FD_ZERO(&set);

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    int fd = (*i)->fd_;
    if (fd >= 0) {
      FD_SET(fd, &set);
      if (nfds < fd+1)
        nfds = fd+1;
    }
  }

  interrupted_ = 0;
  int ret = pselect(nfds, &set, 0, 0, 0, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: pselect");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    int fd = (*i)->fd_;
    if (fd >= 0 && FD_ISSET(fd, &set)) {
      (*i)->OnPipeReady();
      if ((*i)->Done()) {
        finished_.push(*i);
        i = running_.erase(i);
        if (tokens_ && !running_.empty())
          tokens_->Release();
        continue;
      }
    }
    ++i;
  }

  return IsInterrupted();
}
#endif  // !defined(USE_PPOLL)

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

void SubprocessSet::Clear() {
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    // Since the foreground process is in our process group, it will receive
    // the interruption signal (i.e. SIGINT or SIGTERM) at the same time as us.
    if (!(*i)->use_console_)
      kill(-(*i)->pid_, interrupted_);
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    delete *i;
  running_.clear();
  if (tokens_)
    tokens_->Clear();
}

bool SubprocessSet::CanRunMore() {
  return !tokens_ || tokens_->Acquire();
}
