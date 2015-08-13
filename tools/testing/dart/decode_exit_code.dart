// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

library test.decode_exit_code;

import 'status_file_parser.dart' show
    Expectation;

import 'test_runner.dart' show
    TestCase;

import '../../../pkg/fletchc/lib/src/driver/exit_codes.dart' show
    COMPILER_EXITCODE_CRASH,
    DART_VM_EXITCODE_COMPILE_TIME_ERROR,
    DART_VM_EXITCODE_UNCAUGHT_EXCEPTION,
    MEMORY_LEAK_EXITCODE;

abstract class DecodeExitCode {
  int get exitCode;

  Expectation decodeExitCode() {
    switch (exitCode) {
      case 0:
        return Expectation.PASS;

      case COMPILER_EXITCODE_CRASH:
        return Expectation.CRASH;

      case DART_VM_EXITCODE_COMPILE_TIME_ERROR:
        return Expectation.COMPILETIME_ERROR;

      case DART_VM_EXITCODE_UNCAUGHT_EXCEPTION:
        return Expectation.RUNTIME_ERROR;

      case MEMORY_LEAK_EXITCODE:
        return Expectation.MEMORY_LEAK;

      default:
        return Expectation.FAIL;
    }
  }

  Expectation result(TestCase testCase) {
    Expectation outcome = decodeExitCode();

    if (testCase.hasRuntimeError) {
      if (!outcome.canBeOutcomeOf(Expectation.RUNTIME_ERROR)) {
        if (outcome == Expectation.PASS) {
          return Expectation.MISSING_RUNTIME_ERROR;
        } else {
          return outcome;
        }
      }
    }

    if (testCase.hasCompileError) {
      if (!outcome.canBeOutcomeOf(Expectation.COMPILETIME_ERROR)) {
        if (outcome == Expectation.PASS) {
          return Expectation.MISSING_COMPILETIME_ERROR;
        } else {
          return outcome;
        }
      }
    }

    if (testCase.isNegative) {
      return outcome.canBeOutcomeOf(Expectation.FAIL)
          ? Expectation.PASS
          : Expectation.FAIL;
    }

    return outcome;
  }
}