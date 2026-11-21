import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path_provider_webos/path_provider_webos.dart';

void main() {
  late Directory root;
  setUp(() => root = Directory.systemTemp.createTempSync('ppw'));
  tearDown(() => root.deleteSync(recursive: true));

  PathProviderWebOS provider() => PathProviderWebOS.private(environment: <String, String>{
        'FLUTTER_HOME': root.path,
        'FLUTTER_TEMP_HOME': '${root.path}/appdata/.cache',
        'FLUTTER_APPDATA_HOME': '${root.path}/appdata/.data',
        'FLUTTER_APP_ID': 'com.example.app',
      });

  test('cache path is FLUTTER_TEMP_HOME/<app id> and exists', () async {
    final String? p = await provider().getApplicationCachePath();
    expect(p, '${root.path}/appdata/.cache/com.example.app');
    expect(Directory(p!).existsSync(), isTrue);
  });

  test('support path is FLUTTER_APPDATA_HOME/<app id>', () async {
    expect(await provider().getApplicationSupportPath(), '${root.path}/appdata/.data/com.example.app');
  });

  test('without FLUTTER_TEMP_HOME the cache falls back to FLUTTER_HOME/.cache', () async {
    final PathProviderWebOS p = PathProviderWebOS.private(
        environment: <String, String>{'FLUTTER_HOME': root.path, 'FLUTTER_APP_ID': 'a'});
    expect(await p.getApplicationCachePath(), '${root.path}/.cache/a');
  });
}
