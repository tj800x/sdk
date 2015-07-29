// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

library fletchc.verbs.compile_verb;

import 'infrastructure.dart';

import '../../compiler.dart' show
    FletchCompiler;

import '../driver/exit_codes.dart' show
    COMPILER_EXITCODE_CRASH;

import 'documentation.dart' show
    compileDocumentation;

const Verb compileVerb = const Verb(
    compile, compileDocumentation, requiresSession: true,
    requiresTarget: true, supportsTarget: TargetKind.FILE);

Future<int> compile(AnalyzedSentence sentence, VerbContext context) {
  String script = sentence.targetName;

  // This is asynchronous, but we don't await the result so we can respond to
  // other requests.
  context.performTaskInWorker(new CompileTask(script));

  return new Future<int>.value(null);
}

class CompileTask extends SharedTask {
  // Keep this class simple, see note in superclass.

  final String script;

  const CompileTask(this.script);

  Future<int> call(
      CommandSender commandSender,
      StreamIterator<Command> commandIterator) {
    return compileTask(script);
  }
}

Future<int> compileTask(String script) async {
  // TODO(ahe): Allow user to specify dart2js options.
  List<String> compilerOptions = const bool.fromEnvironment("fletchc-verbose")
      ? <String>['--verbose'] : <String>[];
  FletchCompiler compiler =
      new FletchCompiler(
          options: compilerOptions, script: script,
          // TODO(ahe): packageRoot should be a user provided option.
          packageRoot: 'package/' );

  FletchDelta result;
  try {
    result = await compiler.run();
  } catch (error, stackTrace) {
    // Don't let a compiler crash bring down the session.
    print(error);
    if (stackTrace != null) {
      print(stackTrace);
    }
    return COMPILER_EXITCODE_CRASH;
  }
  SessionState.current.compilationResult = result;

  print("Compiled '$script' to ${result.commands.length} commands");

  return 0;
}