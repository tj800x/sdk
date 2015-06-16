#!/usr/bin/python

# Copyright (c) 2014, the Fletch project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""
Buildbot steps for fletch testing
"""

import os
import re
import subprocess
import shutil
import sys

import bot
import bot_utils

from os.path import dirname, join

utils = bot_utils.GetUtils()

DEBUG_LOG=".debug.log"

FLETCH_REGEXP = r'fletch-(linux|mac|windows)(-(debug|release|asan)-(x86))?'
CROSS_REGEXP = r'cross-fletch-(linux)-(arm)'
TARGET_REGEXP = r'target-fletch-(linux)-(debug|release)-(arm)'

FLETCH_PATH = dirname(dirname(dirname(os.path.abspath(__file__))))
GSUTIL = utils.GetBuildbotGSUtilPath()

GCS_BUCKET = 'gs://fletch-cross-compiled-binaries'

def Run(args):
  print "Running: %s" % ' '.join(args)
  sys.stdout.flush()
  bot.RunProcess(args)

def SetupEnvironment(system):
  if system != 'win32':
    os.environ['PATH'] = '%s/third_party/clang/%s/bin:%s' % (
        FLETCH_PATH, system, os.environ['PATH'])
  if system == 'macos':
    mac_library_path = "third_party/clang/mac/lib/clang/3.6.0/lib/darwin"
    os.environ['DYLD_LIBRARY_PATH'] = '%s/%s' % (FLETCH_PATH, mac_library_path)

def KillFletch(system):
  if system != 'windows':
    # Kill any lingering dart processes (from fletch_driver).
    subprocess.call("killall dart", shell=True)
    subprocess.call("killall fletch", shell=True)
    subprocess.call("killall fletch-vm", shell=True)

def Main():
  name, is_buildbot = bot.GetBotName()

  fletch_match = re.match(FLETCH_REGEXP, name)
  cross_match = re.match(CROSS_REGEXP, name)
  target_match = re.match(TARGET_REGEXP, name)

  if not fletch_match and not cross_match and not target_match:
    raise Exception('Invalid buildername')

  # Setup clang environment
  SetupEnvironment(utils.GuessOS())

  # Clobber build directory if the checkbox was pressed on the BB.
  with utils.ChangedWorkingDirectory(FLETCH_PATH):
    bot.Clobber()

  # Run either Cross&Target build for ARM or normal steps.
  # Accumulate daemon logs messages in '.debug.log' to be displayed on the
  # buildbot.Log
  with open(DEBUG_LOG, 'w') as debug_log:
    with utils.ChangedWorkingDirectory(FLETCH_PATH):

      if fletch_match:
        system = fletch_match.group(1)
        modes = ['debug', 'release']
        archs = ['ia32', 'x64']
        asans = [False, True]

        # Split configurations?
        if fletch_match.group(2):
          mode_or_asan = fletch_match.group(3)
          archs = {
              'x86' : ['ia32', 'x64'],
          }[fletch_match.group(4)]

          # We split our builders into:
          #    fletch-linux-debug
          #    fletch-linux-release
          #    fletch-linux-asan
          if mode_or_asan == 'asan':
            modes = ['debug', 'release']
            asans = [True]
          else:
            modes = [mode_or_asan]
            asans = [False]

        StepsNormal(debug_log, system, modes, archs, asans)
      elif cross_match:
        assert cross_match.group(1) == 'linux'
        assert cross_match.group(2) == 'arm'
        system = 'linux'
        modes = ['debug', 'release']
        arch = 'xarm'
        StepsCrossBuilder(debug_log, system, modes, arch)
      elif target_match:
        assert cross_match.group(1) == 'linux'
        system = 'linux'
        mode = cross_match.group(2)
        arch = 'xarm'
        StepsTargetRunner(debug_log, system, mode, arch)

  # Grep through the '.debug.log' and issue warnings for certain log messages.
  StepAnalyzeLog()


#### Buildbot steps

def StepsNormal(debug_log, system, modes, archs, asans):
  configurations = GetBuildConfigurations(system, modes, archs, asans)

  # Generate ninja files.
  StepGyp()

  # Build all necessary configurations.
  for configuration in configurations:
    StepBuild(configuration['build_conf'], configuration['build_dir']);

  # Run tests on all necessary configurations.
  for full_run in [True, False]:
    for configuration in configurations:
      if not ShouldSkipConfiguration(full_run, configuration):

        # Use a new persistent daemon for every test run.
        # Append it's stdout/stderr to the ".debug.log" file.
        with PersistentFletchDaemon(configuration, debug_log):
          StepTest(
            configuration['build_conf'],
            configuration['mode'],
            configuration['arch'],
            clang=configuration['clang'],
            asan=configuration['asan'],
            full_run=full_run)

def StepsCrossBuilder(debug_log, system, modes, arch):
  revision = os.environ['BUILDBOT_GOT_REVISION']
  assert revision

  for compiler_variant in GetCompilerVariants(system):
    for mode in modes:
      build_conf = GetBuildDirWithoutOut(mode, arch, compiler_variant, False)
      # TODO(kustermann): Once we have sorted out gyp/building issues with arm,
      # we should be able to build everything here.
      args = ['fletch-vm', 'fletch_driver', 'natives.json']
      StepBuild(build_conf, os.path.join('out', build_conf), args=args)

  tarball = TarballName(arch, revision)
  try:
    with bot.BuildStep('Create build tarball'):
      Run(['tar',
           '-cjf', tarball,
           '--exclude=**/obj',
           '--exclude=**/obj.host',
           '--exclude=**/obj.target',
           'out'])

    with bot.BuildStep('Upload build tarball'):
      uri = "%s/%s" % (GCS_BUCKET, tarball)
      Run([GSUTIL, 'cp', tarball, uri])
      Run([GSUTIL, 'setacl', 'public-read', uri])
  finally:
    if os.path.exists(tarball):
      os.remove(tarball)

def StepsTargetRunner(debug_log, system, mode, arch):
  revision = os.environ['BUILDBOT_GOT_REVISION']

  tarball = TarballName(arch, revision)
  try:
    with bot.BuildStep('Fetch build tarball'):
      Run([GSUTIL, 'cp', "%s/%s" % (GCS_BUCKET, tarball), tarball])

    with bot.BuildStep('Unpack build tarball'):
      Run(['tar', '-xjf', tarball])

    # Run tests on all necessary configurations.
    configurations = GetBuildConfigurations(system, [mode], [arch], [False])
    for full_run in [True, False]:
      for configuration in configurations:
        if not ShouldSkipConfiguration(full_run, configuration):
          build_dir = configuration['build_dir']

          # Sanity check we got build artifacts which we expect.
          assert os.path.exists(os.path.join(build_dir, 'fletch-vm'))

          # TODO(kustermann): This is hackisch, but our current copying of the
          # dart binary makes this a requirement.
          dart_arm = 'third_party/bin/linux/dart-arm'
          destination = os.path.join(build_dir, 'dart')
          shutil.copyfile(dart_arm, destination)

          # Use a new persistent daemon for every test run.
          # Append it's stdout/stderr to the ".debug.log" file.
          with PersistentFletchDaemon(configuration, debug_log):
            StepTest(
              configuration['build_conf'],
              configuration['mode'],
              configuration['arch'],
              clang=configuration['clang'],
              asan=configuration['asan'],
              full_run=full_run)
  finally:
    if os.path.exists(tarball):
      os.remove(tarball)

    # We always clobber this to save disk on the arm board.
    bot.Clobber(force=True)


#### Buildbot steps helper

def StepGyp():
  with bot.BuildStep('GYP'):
    Run(['ninja', '-v'])

def AnalyzeLog():
  # pkg/fletchc/lib/src/driver/driver_main.dart will (to .debug.log) print
  # "1234: Crash (..." when an exception is thrown after shutting down a
  # client.  In this case, there's no obvious place to report the exception, so
  # the build bot must look for these crashes.
  pattern=re.compile(r"^[0-9]+: Crash \(")
  with open(DEBUG_LOG) as debug_log:
    undiagnosed_crashes = False
    for line in debug_log:
      if pattern.match(line):
        undiagnosed_crashes = True
        # For information about build bot annotations below, see
        # https://chromium.googlesource.com/chromium/tools/build/+/c63ec51491a8e47b724b5206a76f8b5e137ff1e7/scripts/master/chromium_step.py#472
        print '@@@STEP_LOG_LINE@undiagnosed_crashes@%s@@@' % line.rstrip()
    if undiagnosed_crashes:
      print '@@@STEP_LOG_END@undiagnosed_crashes@@@'
      print '@@@STEP_WARNINGS@@@'
      sys.stdout.flush()

def StepBuild(build_config, build_dir, args=()):
  with bot.BuildStep('Build %s' % build_config):
    Run(['ninja', '-v', '-C', build_dir] + list(args))

def StepTest(name, mode, arch, clang=True, asan=False, full_run=False):
  step_name = '%s%s' % (name, '-full' if full_run else '')
  with bot.BuildStep('Test %s' % step_name, swallow_error=True):
    args = ['python', 'tools/test.py', '-m%s' % mode, '-a%s' % arch,
            '--time', '--report', '-pbuildbot',
            '--step_name=test_%s' % step_name,
            '--kill-persistent-process=0',
            '--run-gclient-hooks=0',
            '--build-before-testing=0',
            '--host-checked']
    if full_run:
      # We let package:fletchc/fletchc.dart compile tests to snapshots.
      # Afterwards we run the snapshot with
      #  - normal fletch VM
      #  - fletch VM with -Xunfold-program enabled
      args.extend(['-cfletchc', '-rfletchvm'])

    if asan:
      args.append('--asan')

    if clang:
      args.append('--clang')

    Run(args)

def StepAnalyzeLog():
  with bot.BuildStep('Fletch daemon log warnings.'):
    AnalyzeLog()


#### Helper functionality

class PersistentFletchDaemon(object):
  def __init__(self, configuration, debug_log):
    self._configuration = configuration
    self._debug_log = debug_log
    self._persistent = None

  def __enter__(self):
    print "Killing existing fletch processes"
    KillFletch(self._configuration['system'])

    print "Starting new persistent fletch daemon"
    self._persistent = subprocess.Popen(
      ['%s/dart' % self._configuration['build_dir'],
       '-c',
       '-p',
       './package/',
       'package:fletchc/src/driver/driver_main.dart',
       './.fletch'],
      stdout=self._debug_log,
      stderr=subprocess.STDOUT,
      close_fds=True)

  def __exit__(self, *_):
    print "Trying to wait for existing fletch daemon."
    self._persistent.terminate()
    self._persistent.wait()

    print "Killing existing fletch processes"
    KillFletch(self._configuration['system'])

def GetBuildConfigurations(system, modes, archs, asans):
  configurations = []

  for asan in asans:
    for compiler_variant in GetCompilerVariants(system):
      for mode in modes:
        for arch in archs:
          build_conf = GetBuildDirWithoutOut(mode, arch, compiler_variant, asan)
          configurations.append({
            'build_conf': build_conf,
            'build_dir': os.path.join('out', build_conf),
            'clang': bool(compiler_variant),
            'asan': asan,
            'mode': mode.lower(),
            'arch': arch.lower(),
            'system': system,
          })

  return configurations

def GetBuildDirWithoutOut(mode, arch, compiler_variant='', asan=False):
  assert mode in ['release', 'debug']
  return '%(mode)s%(arch)s%(clang)s%(asan)s' % {
    'mode': 'Release' if mode == 'release' else 'Debug',
    'arch': arch.upper(),
    'clang': compiler_variant,
    'asan': 'Asan' if asan else '',
  }

def ShouldSkipConfiguration(full_run, configuration):
  mac = configuration['system'] == 'mac'
  if mac and configuration['arch'] == 'x64' and configuration['asan']:
    # Asan/x64 takes a long time on mac.
    return True

  full_run_configurations = ['DebugIA32', 'DebugIA32ClangAsan']
  if full_run and (
     configuration['build_conf'] not in full_run_configurations):
    # We only do full runs on DebugIA32 and DebugIA32ClangAsan for now.
    # full_run = compile to snapshot &
    #            run shapshot &
    #            run shapshot with `-Xunfold-program`
    return True

  return False

def GetCompilerVariants(system):
  # gcc on mac is just an alias for clang.
  mac = system == 'mac'
  return ['Clang'] if mac else ['', 'Clang']

def TarballName(arch, revision):
  return 'fletch_cross_build_%s_%s.tar.bz2' % (arch, revision)


if __name__ == '__main__':
  try:
    Main()
  except OSError as e:
    sys.exit(e.errno)
  sys.exit(0)

