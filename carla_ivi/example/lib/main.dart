// Copyright 2026 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// In-shell example: drives the ihs_carla_bridge from Dart via carla_ivi and
// shows its live BridgeState. Intended to run inside the ivi-homescreen
// headless-vulkan shell, where the bridge's dlsym(RTLD_DEFAULT) resolves the
// shell's exported ihs_vk_export_get_api. Because the shell is headless, every
// state transition is also printed so it can be observed in the shell log.
//
// Configuration (runtime env, read by the shell process):
//   IHS_VK_BRIDGE_SOCKET  socket path to bind (default /tmp/ihs-vk-export.sock)
//   IHS_CB_LIB            absolute path to libihs_carla_bridge.so (else the
//                         platform search path resolves the default name)

import 'dart:async';
import 'dart:io';

import 'package:carla_ivi/carla_ivi.dart';
import 'package:flutter/material.dart';

void main() => runApp(const CarlaIviExampleApp());

void _log(String message) {
  // ignore: avoid_print
  print('[carla_ivi_example] $message');
}

class CarlaIviExampleApp extends StatelessWidget {
  const CarlaIviExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'carla_ivi example',
      home: BridgeStatusPage(),
    );
  }
}

class BridgeStatusPage extends StatefulWidget {
  const BridgeStatusPage({super.key});

  @override
  State<BridgeStatusPage> createState() => _BridgeStatusPageState();
}

class _BridgeStatusPageState extends State<BridgeStatusPage>
    with SingleTickerProviderStateMixin {
  CarlaBridge? _bridge;
  BridgeState _state = BridgeState.idle;
  String _detail = 'starting…';
  StreamSubscription<BridgeState>? _sub;

  // A continuously repeating animation keeps the UI repainting every vsync, so
  // the headless-vulkan backend produces a steady stream of distinct frames for
  // the consumer (a static UI would present nothing under consumer pacing).
  late final AnimationController _anim;

  @override
  void initState() {
    super.initState();
    _anim = AnimationController(
      vsync: this,
      duration: const Duration(seconds: 3),
    )..repeat();
    _startBridge();
  }

  void _startBridge() {
    final socket = Platform.environment['IHS_VK_BRIDGE_SOCKET'] ??
        '/tmp/ihs-vk-export.sock';
    final libName = Platform.environment['IHS_CB_LIB'] ?? kDefaultLibraryName;

    try {
      final bridge = CarlaBridge.open(socket, libraryName: libName);
      _bridge = bridge;
      _sub = bridge.states.listen((s) {
        _log('state=$s');
        if (mounted) {
          setState(() {
            _state = s;
            _detail = 'socket=$socket';
          });
        }
      });
      final started = bridge.start();
      _log('start() -> $started (socket=$socket, lib=$libName)');
      if (mounted) {
        setState(() {
          _state = bridge.state;
          _detail = started
              ? 'listening on $socket'
              : 'start() failed — is this the headless-vulkan shell?';
        });
      }
    } on CarlaBridgeException catch (e) {
      _log('open failed: $e');
      if (mounted) {
        setState(() => _detail = '$e');
      }
    }
  }

  @override
  void dispose() {
    _anim.dispose();
    _sub?.cancel();
    _bridge?.dispose();
    super.dispose();
  }

  Color _colorFor(BridgeState s) {
    switch (s) {
      case BridgeState.consumerConnected:
        return Colors.green;
      case BridgeState.listening:
        return Colors.blue;
      case BridgeState.stopped:
        return Colors.red;
      case BridgeState.idle:
        return Colors.grey;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: <Widget>[
            const Text(
              'ihs-vk-export bridge',
              style: TextStyle(color: Colors.white70, fontSize: 28),
            ),
            const SizedBox(height: 24),
            // Continuously rotating ring — forces a repaint every vsync so the
            // shared frames keep changing while the bridge streams.
            RotationTransition(
              turns: _anim,
              child: Icon(
                Icons.sync,
                size: 72,
                color: _colorFor(_state),
              ),
            ),
            const SizedBox(height: 24),
            DecoratedBox(
              decoration: BoxDecoration(
                color: _colorFor(_state),
                borderRadius: BorderRadius.circular(12),
              ),
              child: Padding(
                padding:
                    const EdgeInsets.symmetric(horizontal: 32, vertical: 16),
                child: Text(
                  _state.name,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 40,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
            ),
            const SizedBox(height: 24),
            Text(
              _detail,
              style: const TextStyle(color: Colors.white54, fontSize: 18),
            ),
          ],
        ),
      ),
    );
  }
}
