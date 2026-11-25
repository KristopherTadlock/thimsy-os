// Every build, clean, prepare, flash, unlock and driver install leaves a
// log file; the engine says where (`log_file`) and an error from such a
// run shows the path with "Open log folder".
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';
import 'package:thimsy_installer/engine/engine.dart';
import 'package:thimsy_installer/engine/fake_engine.dart';
import 'package:thimsy_installer/l10n/app_localizations.dart';
import 'package:thimsy_installer/main.dart';
import 'package:thimsy_installer/state/app_state.dart';
import 'package:thimsy_installer/widgets/common.dart';

Finder key(String k) => find.byKey(ValueKey(k));

const cube = 'anbernic-rg-cube';
final en = lookupAppLocalizations(const Locale('en'));

/// Replaces the folder opener for one test; returns what it was asked to
/// open.
List<String> stubOpenFolder() {
  final opened = <String>[];
  final original = openFolder;
  openFolder = (dir) async => opened.add(dir);
  addTearDown(() => openFolder = original);
  return opened;
}

Future<AppState> start(WidgetTester tester, FakeEngine engine) async {
  tester.view.physicalSize = const Size(1600, 1400);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  final state = AppState(engine, onAndroid: false);
  await tester.pumpWidget(ThimsyInstallerApp(state: state));
  await tester.pumpAndSettle();
  return state;
}

Future<void> tap(WidgetTester tester, Finder f) async {
  await tester.ensureVisible(f);
  await tester.pumpAndSettle();
  await tester.tap(f);
  await tester.pumpAndSettle();
}

Future<void> next(WidgetTester tester) => tap(tester, key('next'));

/// Device → … → Connect, built in `/ws`.
Future<void> toConnect(WidgetTester tester) async {
  await tap(tester, key('device-$cube'));
  await next(tester);
  await next(tester);
  await tap(tester, key('journey-first_time'));
  await next(tester);
  await tester.enterText(key('workspace-field'), '/ws');
  await tester.pumpAndSettle();
  await tap(tester, key('start-build'));
  expect(key('build-complete'), findsOneWidget);
  await next(tester);
  await tap(tester, key('backed-up'));
  await next(tester);
  await tap(tester, key('unlock-confirm'));
  await next(tester);
}

Future<void> install(WidgetTester tester) async {
  await toConnect(tester);
  await next(tester);
  await tap(tester, key('install-button'));
  await tap(tester, key('confirm-install-yes'));
}

/// The path the "Log: …" line inside [of] shows (selectable on its own).
String logLine(WidgetTester tester, Finder of) => tester
    .widget<SelectableText>(
      find.descendant(of: of, matching: key('log-file-path')),
    )
    .data!;

Widget notice(ErrorNotice child, {Locale locale = const Locale('en')}) =>
    MaterialApp(
      locale: locale,
      localizationsDelegates: const [
        AppLocalizations.delegate,
        ...GlobalMaterialLocalizations.delegates,
      ],
      supportedLocales: AppLocalizations.supportedLocales,
      home: Scaffold(body: child),
    );

const failure = ErrorEvent(
  code: 'tool_failed',
  message: 'spd_dump: write super failed',
  hint: 'Reconnect the device and try again.',
);

void main() {
  group('the log_file event', () {
    test('is parsed with its path', () {
      final e = EngineEvent.fromJson({
        'type': 'log_file',
        'path': '/ws/logs/20261124-101500Z-flash-anbernic-rg-cube-medium.log',
      });
      expect(
        (e as LogFileEvent).path,
        '/ws/logs/20261124-101500Z-flash-anbernic-rg-cube-medium.log',
      );
    });

    test('comes through the JSON Lines decoder', () async {
      final events = await Stream.fromIterable([
        '{"type":"log_file","path":"C:\\\\ws\\\\logs\\\\a.log"}\n',
        '{"type":"stage","stage":"check","title":"Checking"}\n',
      ]).transform(const JsonLinesDecoder()).toList();
      expect((events.first as LogFileEvent).path, r'C:\ws\logs\a.log');
      expect(events.last, isA<StageEvent>());
    });
  });

  group('RunState.logPath', () {
    test('is kept from log_file, not added to the run log, and cleared by '
        'reset', () {
      final run = RunState();
      expect(run.logPath, isNull);
      run.apply(const LogFileEvent('/ws/logs/x.log'));
      run.apply(const StageEvent('check', 'Checking'));
      run.apply(failure);
      run.apply(const ResultEvent(false, {'type': 'result', 'ok': false}));
      expect(run.logPath, '/ws/logs/x.log');
      expect(run.log.where((l) => l.contains('x.log')), isEmpty);
      run.reset();
      expect(run.logPath, isNull);
    });
  });

  test('logFolderOf takes either separator and keeps a root', () {
    expect(logFolderOf('/ws/logs/a.log'), '/ws/logs');
    expect(
      logFolderOf(r'C:\Users\u\Thimsy Installer\logs\a.log'),
      r'C:\Users\u\Thimsy Installer\logs',
    );
    expect(logFolderOf('/a.log'), '/');
    expect(logFolderOf(r'C:\a.log'), r'C:\');
    expect(logFolderOf('a.log'), '.');
  });

  group('ErrorNotice', () {
    testWidgets('shows the run\'s log and opens its folder', (tester) async {
      final opened = stubOpenFolder();
      final run = RunState()
        ..apply(const LogFileEvent('/ws/logs/20261124-101500Z-flash-x.log'))
        ..apply(failure);
      await tester.pumpWidget(notice(ErrorNotice(error: failure, run: run)));
      expect(
        logLine(tester, find.byType(ErrorNotice)),
        '/ws/logs/20261124-101500Z-flash-x.log',
      );
      // The translated label next to it.
      expect(
        find.text(en.errorLogLine('\u{FFFC}').split('\u{FFFC}').first),
        findsOneWidget,
      );
      expect(find.text(en.openLogFolder), findsOneWidget);
      await tester.tap(key('open-log-folder'));
      await tester.pump();
      expect(opened, ['/ws/logs']);
    });

    testWidgets('shows no log line or button without a log', (tester) async {
      final opened = stubOpenFolder();
      await tester.pumpWidget(
        notice(ErrorNotice(error: failure, run: RunState()..apply(failure))),
      );
      expect(key('log-file-line'), findsNothing);
      expect(key('open-log-folder'), findsNothing);
      expect(key('copy-details'), findsOneWidget);
      await tester.pumpWidget(notice(const ErrorNotice(error: failure)));
      expect(key('log-file-line'), findsNothing);
      expect(key('open-log-folder'), findsNothing);
      expect(opened, isEmpty);
    });

    testWidgets('keeps the path left to right in Hebrew', (tester) async {
      final run = RunState()..apply(const LogFileEvent('/ws/logs/a.log'));
      await tester.pumpWidget(
        notice(
          ErrorNotice(error: failure, run: run),
          locale: const Locale('he'),
        ),
      );
      final he = lookupAppLocalizations(const Locale('he'));
      // Copying the path gives just the path: no direction marks in it.
      expect(logLine(tester, find.byType(ErrorNotice)), '/ws/logs/a.log');
      final path = tester.widget<SelectableText>(key('log-file-path'));
      expect(path.textDirection, TextDirection.ltr);
      final label = he.errorLogLine('\u{FFFC}').split('\u{FFFC}').first;
      expect(label, isNot(contains('\u2066')));
      expect(find.text(label), findsOneWidget);
      expect(find.text(he.openLogFolder), findsOneWidget);
    });
  });

  group('in the app', () {
    testWidgets('a build keeps its log path in the workspace', (tester) async {
      final s = await start(tester, FakeEngine(step: Duration.zero));
      await toConnect(tester);
      expect(s.buildRun.logPath, startsWith('/ws/logs/'));
      expect(s.buildRun.logPath, endsWith('Z-build-$cube-medium.log'));
      // "Copy details" names it too.
      expect(
        s.diagnostics(s.buildRun),
        contains('log file: ${s.buildRun.logPath}'),
      );
      // The background preparation of that build logs there too.
      expect(s.prepareRun.logPath, startsWith('/ws/logs/'));
      expect(s.prepareRun.logPath, endsWith('Z-prepare-$cube-medium.log'));
    });

    testWidgets('a failed install shows its log; Open log folder opens the '
        'workspace\'s logs', (tester) async {
      final opened = stubOpenFolder();
      final engine = FakeEngine(step: Duration.zero)..flashErrors.add(failure);
      final s = await start(tester, engine);
      await install(tester);
      final path = s.flashRun.logPath!;
      expect(path, startsWith('/ws/logs/'));
      expect(
        RegExp(r'/\d{8}-\d{6}Z-flash-anbernic-rg-cube-medium\.log$')
            .hasMatch(path),
        isTrue,
        reason: path,
      );
      expect(logLine(tester, key('run-error')), path);
      await tap(
        tester,
        find.descendant(of: key('run-error'), matching: key('open-log-folder')),
      );
      expect(opened, ['/ws/logs']);
    });

    testWidgets('an install stopped by free space shows its log in the space '
        'dialog', (tester) async {
      final opened = stubOpenFolder();
      final engine = FakeEngine(step: Duration.zero, flashSpaceFailures: 1);
      final s = await start(tester, engine);
      await install(tester);
      expect(key('space-dialog'), findsOneWidget);
      final path = s.flashRun.logPath!;
      expect(path, startsWith('/ws/logs/'));
      expect(logLine(tester, key('space-dialog')), path);
      await tap(
        tester,
        find.descendant(
          of: key('space-dialog'),
          matching: key('open-log-folder'),
        ),
      );
      expect(opened, ['/ws/logs']);
      await tap(tester, key('space-cancel'));
      expect(key('space-dialog'), findsNothing);
    });

    testWidgets('a failed clean shows its log', (tester) async {
      final opened = stubOpenFolder();
      final engine = FakeEngine(step: Duration.zero)
        ..cleanFailure = const ErrorEvent(
          code: 'internal',
          message: 'Permission denied (os error 13)',
        );
      final s = await start(tester, engine);
      await install(tester);
      await next(tester);
      await next(tester);
      await tap(tester, key('run-clean'));
      expect(key('clean-error'), findsOneWidget);
      final path = s.cleanRun.logPath!;
      expect(path, matches(RegExp(r'^/ws/logs/\d{8}-\d{6}Z-clean\.log$')));
      expect(logLine(tester, key('clean-error')), path);
      await tap(
        tester,
        find.descendant(
          of: key('clean-error'),
          matching: key('open-log-folder'),
        ),
      );
      expect(opened, ['/ws/logs']);
    });

    testWidgets('a failed driver install shows its log', (tester) async {
      final opened = stubOpenFolder();
      final engine = FakeEngine(step: Duration.zero, host: 'windows')
        ..driversFailure = failure;
      final s = await start(tester, engine);
      await toConnect(tester);
      expect(key('log-file-line'), findsNothing);
      await tap(tester, key('install-driver'));
      final path = s.driverLogPath!;
      expect(
        path,
        startsWith(r'C:\Users\you\AppData\Local\Thimsy Installer\logs\'),
      );
      expect(path, endsWith('Z-drivers-$cube.log'));
      expect(logLine(tester, key('driver-notice')), path);
      await tap(
        tester,
        find.descendant(
          of: key('driver-notice'),
          matching: key('open-log-folder'),
        ),
      );
      expect(opened, [r'C:\Users\you\AppData\Local\Thimsy Installer\logs']);
      // Installing again (accepted this time) clears it.
      engine.driversFailure = null;
      await tap(tester, key('install-driver'));
      expect(s.driverLogPath, isNull);
      expect(key('log-file-line'), findsNothing);
    });

    test('an unlock run keeps its log path', () async {
      final state = RunState();
      final run = FakeEngine(step: Duration.zero).unlock(device: cube);
      await for (final e in run.events) {
        state.apply(e);
      }
      expect(
        state.logPath,
        startsWith('/home/you/.local/state/thimsy-installer/logs/'),
      );
      expect(state.logPath, endsWith('Z-unlock-$cube.log'));
    });
  });
}
