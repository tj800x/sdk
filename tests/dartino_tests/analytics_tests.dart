// Copyright (c) 2016, the Dartino project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:dartino_compiler/src/hub/analytics.dart';
import 'package:dartino_compiler/src/hub/hub_main.dart' show ClientVerbContext;
import 'package:dartino_compiler/src/verbs/infrastructure.dart';
import 'package:expect/expect.dart' show Expect;
import 'package:path/path.dart' show isAbsolute, join;

import 'message_tests.dart' show MockClientConnection;

main() async {
  bool receivedLogMessage = false;
  log(message) {
    receivedLogMessage = true;
    print(message);
  }
  MockServer mockServer = new MockServer();
  await mockServer.start();
  var tmpDir = Directory.systemTemp.createTempSync('DartinoAnalytics');
  var tmpUuidFile = new File(join(tmpDir.path, '.dartino_uuid_test'));
  final analytics = new Analytics(log,
      serverUrl: mockServer.localUrl, uuidUri: tmpUuidFile.uri);

  Expect.isNotNull(Analytics.defaultUuidUri);
  String defaultUuidPath = Analytics.defaultUuidUri.toFilePath();
  print(defaultUuidPath);
  Expect.isNotNull(defaultUuidPath);
  Expect.isTrue(isAbsolute(defaultUuidPath));

  Expect.isNotNull(analytics.uuidUri);
  var uuidPath = analytics.uuidUri.toFilePath();
  print(uuidPath);
  Expect.isNotNull(uuidPath);
  Expect.isTrue(isAbsolute(uuidPath));

  try {
    analytics.clearUuid();
    Expect.isNull(analytics.uuid);
    Expect.isTrue(analytics.shouldPromptForOptIn);
    Expect.isFalse(analytics.hasOptedIn);
    Expect.isFalse(analytics.hasOptedOut);
    Expect.isFalse(analytics.loadUuid());
    Expect.isTrue(analytics.shouldPromptForOptIn);
    Expect.isFalse(analytics.hasOptedIn);
    Expect.isFalse(analytics.hasOptedOut);
    Expect.isFalse(tmpUuidFile.existsSync());
    Expect.isFalse(receivedLogMessage);

    analytics.clearUuid();
    Expect.isTrue(analytics.shouldPromptForOptIn);
    Expect.isFalse(analytics.hasOptedIn);
    Expect.isFalse(analytics.hasOptedOut);
    analytics.writeOptOut();
    Expect.isFalse(analytics.shouldPromptForOptIn);
    Expect.isFalse(analytics.hasOptedIn);
    Expect.isTrue(analytics.hasOptedOut);
    Expect.isNull(analytics.uuid);
    Expect.isTrue(tmpUuidFile.existsSync());
    Expect.equals(Analytics.optOutValue, tmpUuidFile.readAsStringSync());
    Expect.isFalse(receivedLogMessage);

    analytics.clearUuid();
    Expect.isTrue(analytics.shouldPromptForOptIn);
    Expect.isFalse(analytics.hasOptedIn);
    Expect.isFalse(analytics.hasOptedOut);
    analytics.writeNewUuid();
    Expect.isFalse(analytics.shouldPromptForOptIn);
    Expect.isTrue(analytics.hasOptedIn);
    Expect.isFalse(analytics.hasOptedOut);
    Expect.isNotNull(analytics.uuid);
    Expect.isTrue(analytics.uuid.length > 5);
    Expect.notEquals(analytics.uuid, Analytics.optOutValue);
    Expect.isTrue(tmpUuidFile.existsSync());
    Expect.equals(analytics.uuid, tmpUuidFile.readAsStringSync());
    Expect.isFalse(receivedLogMessage);

    String expectedUuid = analytics.uuid;
    File uuidFileSav = new File('${analytics.uuidUri.toFilePath()}.sav');
    tmpUuidFile.renameSync(uuidFileSav.path);
    analytics.clearUuid();
    uuidFileSav.renameSync(tmpUuidFile.path);
    Expect.isNull(analytics.uuid);
    Expect.isTrue(analytics.shouldPromptForOptIn);
    Expect.isFalse(analytics.hasOptedIn);
    Expect.isFalse(analytics.hasOptedOut);
    Expect.isTrue(analytics.loadUuid());
    Expect.isFalse(analytics.shouldPromptForOptIn);
    Expect.isTrue(analytics.hasOptedIn);
    Expect.isFalse(analytics.hasOptedOut);
    Expect.equals(expectedUuid, analytics.uuid);
    Expect.isFalse(receivedLogMessage);

    analytics.clearUuid();
    Expect.isFalse(receivedLogMessage);

    analytics.clearUuid();
    analytics.logStartup();
    await analytics.shutdown();
    await mockServer.assertNoMessages('clearUuid');

    analytics.writeOptOut();
    analytics.logStartup();
    await analytics.shutdown();
    await mockServer.assertNoMessages('optOut');

    String dartPath = 'some/path/to/my.dart';
    String dartPathHash = analytics.hash(dartPath);
    Expect.isTrue(dartPathHash.startsWith('SHA('));
    Expect.isTrue(dartPathHash.endsWith(')'));
    Expect.isTrue(dartPathHash.length > 6);
    Expect.equals(-1, dartPathHash.indexOf('some'));
    Expect.equals(-1, dartPathHash.indexOf('dart'));
    Expect.equals(-1, dartPathHash.indexOf('hash'));
    Expect.equals(-1, dartPathHash.indexOf('encode'));
    Expect.equals(-1, dartPathHash.indexOf('BASE64'));

    Expect.equals(dartPathHash, analytics.hashUri(dartPath));
    Expect.equals('session', analytics.hashUri('session'));

    Expect.listEquals(['analyze', dartPathHash],
        analytics.hashAllUris(['analyze', dartPath]).toList());

    Expect.equals('null', analytics.hashUriWords(null));
    Expect.equals('', analytics.hashUriWords(''));
    Expect.equals(
        'analyze $dartPathHash', analytics.hashUriWords('analyze $dartPath'));

    analytics.writeNewUuid();
    analytics.logStartup();
    analytics.logRequest('1.2.3', dartPath, false, '123', []);
    analytics.logRequest('0.0.0', dartPath, true, '098765432', ['help']);
    analytics.logRequest('0.0.0', dartPath, true, '098765433', ['help', 'all']);
    analytics.logRequest(
        '0.0.0', dartPath, false, '098765434', ['analyze', dartPath]);
    analytics.logResponse('responseType45', ['analyze', dartPath]);
    analytics.logErrorMessage('$dartPath not found');
    analytics.logError('error1');
    analytics.logError('error2', null);
    try {
      throw 'test exception logging $dartPath';
    } catch (error, stackTrace) {
      analytics.logError(error, stackTrace);
    }
    analytics.logComplete(37);
    await analytics.shutdown();

    mockServer.expectedUuid = analytics.uuid;

    List<String> expectedStartupMsg = [
      TAG_STARTUP,
      'version information not available', // Dartino version
      Platform.version,
      Platform.operatingSystem,
    ];
    List<String> actualStartupMsg = await mockServer.nextMessage();
    String actualVersion = actualStartupMsg[1];
    Expect.isTrue(actualVersion.length > 2);
    expectedStartupMsg[1] = actualVersion;
    Expect.listEquals(expectedStartupMsg, actualStartupMsg,
        '\nexpected: $expectedStartupMsg\nactual:   $actualStartupMsg\n');

    await mockServer.expectMessage([TAG_REQUEST, '1.2.3',
          dartPathHash, '123', 'detached']);
    await mockServer.expectMessage([TAG_REQUEST, '0.0.0',
          dartPathHash, '098765432', 'interactive', 'help']);
    await mockServer.expectMessage([TAG_REQUEST, '0.0.0',
          dartPathHash, '098765433', 'interactive', 'help', 'all']);
    await mockServer.expectMessage([
      TAG_REQUEST,
      '0.0.0',
      dartPathHash,
      '098765434',
      'detached',
      'analyze',
      dartPathHash
    ]);
    await mockServer.expectMessage(
        [TAG_RESPONSE, 'responseType45', 'analyze', dartPathHash]);
    await mockServer.expectMessage([TAG_ERRMSG, '$dartPathHash not found']);
    await mockServer.expectMessage([TAG_ERROR, 'error1', 'null']);
    await mockServer.expectMessage([TAG_ERROR, 'error2', 'null']);
    List<String> errMsg = await mockServer.nextMessage();
    Expect.equals(TAG_ERROR, errMsg[0]);
    Expect.equals('test exception logging $dartPathHash', errMsg[1]);
    Expect.isTrue(errMsg[2].length > 10);
    await mockServer.expectMessage([TAG_COMPLETE, '37']);
    await mockServer.expectMessage([TAG_SHUTDOWN]);
    await mockServer.assertNoMessages('optIn');
  } finally {
    if (tmpUuidFile.existsSync()) tmpUuidFile.deleteSync();
    if (tmpDir.existsSync()) tmpDir.deleteSync();
    mockServer.shutdown();
  }

  MockClientConnection conn = await mockCmdline(['x-should-prompt-analytics']);
  Expect.listEquals(['false'], conn.stdoutMessages);
  conn = await mockCmdline(['x-should-prompt-analytics'],
      shouldPromptForOptIn: true);
  Expect.listEquals(['true'], conn.stdoutMessages);
}

Future<MockClientConnection> mockCmdline(List<String> arguments,
    {bool shouldPromptForOptIn: false}) async {
  MockClientConnection client =
      new MockClientConnection(shouldPromptForOptIn: shouldPromptForOptIn);
  client.stdoutMessages = <String>[];
  client.parseArguments(null, null, false, arguments);
  ClientVerbContext context = new ClientVerbContext(client, null, null);
  var exitCode = await client.sentence.performVerb(context);
  client.recordedExitCode = exitCode;
  return client;
}

class MockServer {
  HttpServer server;
  StreamSubscription<HttpRequest> subscription;
  List<dynamic> messages = <dynamic>[];

  /// If [messagesReceived] is not-`null`, then it is completed when a new
  /// message has been added to the collection of [messages].
  Completer<Null> messagesReceived;

  /// Messages are asserted/tested in numerical order, and [expectedMsgN]
  /// is the # of the next message to be asserted by [expectMessage].
  int expectedMsgN = 0;

  String expectedUuid;

  Future<Null> start() async {
    server = await HttpServer.bind(InternetAddress.LOOPBACK_IP_V4, 0);
    subscription = server.listen(processMessage);
  }

  int get localPort => server.port;
  String get localUrl => 'http://localhost:$localPort/rpc';

  void processMessage(HttpRequest event) {
    Expect.equals('POST', event.method);
    event.transform(UTF8.decoder).join().then((String msg) {
      messages.add(JSON.decode(msg));
      event.response.close();
      messagesReceived?.complete();
    });
  }

  void shutdown() {
    subscription.cancel();
    server.close();
  }

  Future<Null> assertNoMessages(String tag) async {
    /// Give the system a moment to process any incomming communication.
    await new Future.delayed(new Duration(milliseconds: 1));
    Expect.equals(messages.length, 0,
        'Expected no messages ($tag), but found ${messages.length}');
  }

  /// Assert that the received messages have the specified uuid.
  Future<Null> expectMessage(List<String> expectedParts) async {
    List<String> actualParts = await nextMessage();
    Expect.listEquals(expectedParts, actualParts,
        '\nexpected: $expectedParts\nactual:   $actualParts\n');
  }

  /// Return the next analytics message.
  Future<List<String>> nextMessage() async {
    // Messages may be received out of order... wait for msgN #[expectedMsgN].
    int messageIndex = 0;
    dynamic map;
    while (true) {
      if (messageIndex == messages.length) {
        messagesReceived = new Completer<Null>();
        await messagesReceived.future;
      }
      dynamic json = messages[messageIndex];

      // Messages consist of an outer layer containing session id and msgN
      // and defined in package:instrumentation_client/src/io_channels.dart.
      // This outer layer wrappers the data payload described below.
      if (json is! List) Expect.fail('Expected list, but found "$json"');
      if (json.length < 2)
        Expect.fail('Expected length > 2, but found "$json"');
      map = json[1];
      if (map is! Map) Expect.fail('Expected map, but found "$map"');
      if (expectedMsgN == map['msgN']) break;
      ++messageIndex;
    }
    messages.removeAt(messageIndex);
    ++expectedMsgN;

    var data = map['Data'];
    if (data is! String) Expect.fail('Expected string, but found "$data"');

    // The data payload encoding and compression can be found in
    // package:instrumentation_client/instrumentation_client.dart # _encode.
    List<int> unpacked = CryptoUtils.base64StringToBytes(data);
    List<int> unzipped = GZIP.decode(unpacked);
    String decoded = UTF8.decode(unzipped);
    Iterable<String> lines = LineSplitter.split(decoded);
    String message = lines.first;

    // The message's data payload contains a timestamp, a tag, followed
    // by a series of zero or more strings based upon the specific tag.
    // The format of this message is defined in
    // pkg/dartino_compiler/lib/src/hub/analytics.dart # _send(...).
    Expect.isTrue(
        message.startsWith('~'), 'Unexpected message start "$message"');
    List<String> parts = message
        .substring(1)
        .replaceAll('::', 'qqqq')
        .split(':')
        .map((String part) => part.replaceAll('qqqq', ':'))
        .toList();
    // parts[0] is a timestamp.
    // parts[1] is the UUID
    // parts[2] is the tag
    if (parts.length < 3) Expect.fail('Unexpected msg: $parts');
    int.parse(parts[0]);
    Expect.equals(expectedUuid, parts[1], 'Expected uuid');
    return parts.sublist(2);
  }
}
