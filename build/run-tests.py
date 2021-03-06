#
# run-tests.py - run the tests in the regression test suite.
#

import os, sys


class TestHarness:
  '''Test harness for Subversion tests.
  '''

  def __init__(self, abs_srcdir, abs_builddir, python, shell, logfile):
    '''Construct a TestHarness instance.

    ABS_SRCDIR and ABS_BUILDDIR are the source and build directories.
    PYTHON is the name of the python interpreter.
    SHELL is the name of the shell.
    LOGFILE is the name of the log file.
    '''
    self.srcdir = abs_srcdir
    self.builddir = abs_builddir
    self.python = python
    self.shell = shell
    self.logfile = logfile
    self.log = None

  def run(self, list):
    'Run all test programs given in LIST.'
    self._open_log('w')
    failed = 0
    for prog in list:
      failed = self._run_test(prog) or failed
    if failed:
      print 'At least one test FAILED, checking ' + self.logfile
      self._open_log('r')
      map(sys.stdout.write, filter(lambda x: x[:4] == 'FAIL',
                                   self.log.readlines()))
    self._close_log()

  def _open_log(self, mode):
    'Open the log file with the required MODE.'
    self._close_log()
    self.log = open(self.logfile, mode)

  def _close_log(self):
    'Close the log file.'
    if not self.log is None:
      self.log.close()
      self.log = None

  def _run_test(self, prog):
    'Run a single test.'
    progdir, progbase = os.path.split(prog)
    # Using write here because we don't want even a trailing space
    sys.stdout.write('Running all tests in ' + progbase + '...')
    print >> self.log, 'START: ' + progbase

    if progbase[-3:] == '.py':
      cmdline = [self.python, os.path.join(self.srcdir, prog)]
    elif progbase[-3:] == '.sh':
      cmdline = [self.shell, os.path.join(self.srcdir, prog),
                 os.path.join(self.builddir, progdir),
                 os.path.join(self.srcdir, progdir)]
    elif os.access(prog, os.X_OK):
      cmdline = ['./' + progbase]
##### BEGIN Windows-specific
#   elif os.access(progdir + '/Debug/' + progbase, os.X_OK):
#     cmdline = ['./Debug/' + progbase]
#   elif os.access(progdir + '/Release/' + progbase, os.X_OK):
#     cmdline = ['./Release/' + progbase]
##### END Windows-specific
    else:
      print 'Don\'t know what to do about ' + progbase
      sys.exit(1)

    old_cwd = os.getcwd()
    try:
      os.chdir(progdir)
      failed = self._run_prog(cmdline)
    except:
      os.chdir(old_cwd)
      raise
    else:
      os.chdir(old_cwd)

    if failed:
      print 'FAILURE'
    else:
      print 'success'
    print >> self.log, 'END: ' + progbase + '\n'
    return failed

  def _run_prog(self, cmdline):
    'Execute COMMAND, redirecting standard output and error to the log file.'
    def restore_streams(stdout, stderr):
      os.dup2(stdout, 1)
      os.dup2(stderr, 2)
      os.close(stdout)
      os.close(stderr)

    sys.stdout.flush()
    sys.stderr.flush()
    self.log.flush()
    old_stdout = os.dup(1)
    old_stderr = os.dup(2)
    try:
      os.dup2(self.log.fileno(), 1)
      os.dup2(self.log.fileno(), 2)
      rv = os.spawnv(os.P_WAIT, cmdline[0], cmdline)
    except:
      restore_streams(old_stdout, old_stderr)
      raise
    else:
      restore_streams(old_stdout, old_stderr)
      return rv


def main():
  '''Argument parsing and test driver.

  Usage: run-tests.py <abs_srcdir> <abs_builddir> <python> <shell> <prog ...>

  The first four parameters are passed unchanged to the TestHarness
  constuctor.  All other parameters are names of test programs.
  '''
  if len(sys.argv) < 6:
    print 'Usage: run-tests.py <abs_srcdir> <abs_builddir>' \
          '<python> <shell> <prog ...>'
    sys.exit(2)

  th = TestHarness(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
                   os.path.abspath('tests.log'))
  th.run(sys.argv[5:])


# Run main if not imported as a module
if __name__ == '__main__':
  main()



#######################
# Examples:

# With srcdir==builddir, running from this directory:
# th = TestHarness('', '', '/usr/local/python', '/bin/sh', 'tests.log')
# th.run(['../subversion/tests/libsvn_delta/random-test',
#         '../subversion/tests/libsvn_fs/skel-test'])

# New 'check' target in Makefile.in:
# check: $(TEST_DEPS) @FS_TEST_DEPS@
#        @$(PYTHON) $(top_srcdir)/build/pycheck.py ; \
#        $(PYTHON) $(top_srcdir)/build/run-tests.py \
#             '$(abs_srcdir)' '$(abs_builddir)' '$(PYTHON)' '$(SHELL)' \
#             $(TEST_PROGRAMS) @FS_TEST_PROGRAMS@
