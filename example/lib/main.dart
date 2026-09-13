import 'package:flutter/material.dart';
import 'package:just_audio/just_audio.dart';

void main() {
  runApp(const ExampleApp());
}

class ExampleApp extends StatefulWidget {
  const ExampleApp({super.key});

  @override
  State<ExampleApp> createState() => _ExampleAppState();
}

class _ExampleAppState extends State<ExampleApp> {
  late final AudioPlayer _player;
  int _switchCount = 0;

  @override
  void initState() {
    super.initState();
    _player = AudioPlayer();
  }

  @override
  void dispose() {
    _player.dispose();
    super.dispose();
  }

  Future<void> _testRapidSwitch() async {
    setState(() => _switchCount++);
    try {
      await _player.stop();
      await _player.setUrl('https://everyayah.com/data/Alafasy_128kbps/001001.mp3');
      await _player.play();
    } catch (e) {
      debugPrint('Playback error: $e');
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('just_audio_windows_plus Example'),
        ),
        body: Center(
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              const Text(
                'Thread-Safe Windows Audio Player',
                style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
              ),
              const SizedBox(height: 16),
              Text('Rapid switch cycles: $_switchCount'),
              const SizedBox(height: 24),
              ElevatedButton.icon(
                onPressed: _testRapidSwitch,
                icon: const Icon(Icons.play_arrow),
                label: const Text('Test Track Switch'),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
