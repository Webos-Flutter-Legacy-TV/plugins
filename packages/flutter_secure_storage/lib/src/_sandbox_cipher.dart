import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:pointycastle/export.dart';

/// Fallback encryption for firmware without a usable system keystore.
///
/// webOS releases before keymanager3 (for example webOS 6) give apps no
/// working key service: keymanager3 is absent and keymanager2 returns the
/// plaintext. There, values are encrypted with AES-256-GCM using a random key
/// stored in the app's private data folder. This protects against other apps
/// and casual copies of the database, but not against someone with root
/// access to the TV.
class SandboxCipher {
  static const String prefix = 'sbx1:';
  static const int _nonceLength = 12;
  static const int _tagBits = 128;

  Uint8List? _key;
  bool _warned = false;

  static bool owns(String stored) => stored.startsWith(prefix);

  Future<String> encrypt(String plaintext) async {
    final key = await _loadKey();
    final nonce = _randomBytes(_nonceLength);
    final cipher = GCMBlockCipher(AESEngine())
      ..init(true, AEADParameters(KeyParameter(key), _tagBits, nonce, Uint8List(0)));
    final sealed = cipher.process(Uint8List.fromList(utf8.encode(plaintext)));
    return prefix + base64.encode(Uint8List.fromList([...nonce, ...sealed]));
  }

  /// Returns '' when [stored] is not ours, is corrupt, or fails authentication.
  Future<String> decrypt(String stored) async {
    if (!owns(stored)) return '';
    try {
      final key = await _loadKey();
      final raw = base64.decode(stored.substring(prefix.length));
      if (raw.length <= _nonceLength) return '';
      final nonce = Uint8List.sublistView(raw, 0, _nonceLength);
      final sealed = Uint8List.sublistView(raw, _nonceLength);
      final cipher = GCMBlockCipher(AESEngine())
        ..init(false, AEADParameters(KeyParameter(key), _tagBits, nonce, Uint8List(0)));
      return utf8.decode(cipher.process(sealed));
    } catch (_) {
      return '';
    }
  }

  Future<Uint8List> _loadKey() async {
    if (_key != null) return _key!;
    if (!_warned) {
      _warned = true;
      debugPrint('flutter_secure_storage_webos: no system keystore on this '
          'firmware; using app-sandbox AES-GCM storage (not hardware-backed).');
    }
    final file = File(_keyPath());
    if (await file.exists()) {
      final bytes = base64.decode((await file.readAsString()).trim());
      if (bytes.length == 32) return _key = Uint8List.fromList(bytes);
    }
    final key = _randomBytes(32);
    await file.parent.create(recursive: true);
    await file.writeAsString(base64.encode(key), flush: true);
    try {
      await Process.run('chmod', ['600', file.path]);
    } catch (_) {}
    return _key = key;
  }

  /// Next to the plugin's database: $FLUTTER_APPDATA_HOME/$FLUTTER_APP_ID.
  static String _keyPath() {
    final env = Platform.environment;
    final base = env['FLUTTER_APPDATA_HOME'];
    final appId = env['FLUTTER_APP_ID'];
    final dir = (base != null && base.isNotEmpty && appId != null && appId.isNotEmpty)
        ? '$base/$appId'
        : '${File(Platform.resolvedExecutable).parent.path}/appdata';
    return '$dir/.flutter_secure_storage.key';
  }

  static Uint8List _randomBytes(int n) {
    final r = Random.secure();
    return Uint8List.fromList(List<int>.generate(n, (_) => r.nextInt(256)));
  }
}
