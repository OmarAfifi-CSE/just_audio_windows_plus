import 'package:flutter/material.dart';
import 'package:just_audio/just_audio.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const MaterialApp(
    title: 'just_audio_windows_plus',
    debugShowCheckedModeBanner: false,
    home: AudioPlayerExample(),
  ));
}

class AudioPlayerExample extends StatefulWidget {
  const AudioPlayerExample({super.key});

  @override
  State<AudioPlayerExample> createState() => _AudioPlayerExampleState();
}

class _AudioPlayerExampleState extends State<AudioPlayerExample> {
  late final AudioPlayer _player;
  double _volume = 1.0;

  final _playlist = [
    AudioSource.uri(
      Uri.parse('https://server10.mp3quran.net/minsh/001.mp3'),
      tag: 'Surah Al-Fatihah',
    ),
    AudioSource.uri(
      Uri.parse('https://server10.mp3quran.net/minsh/112.mp3'),
      tag: 'Surah Al-Ikhlas',
    ),
    AudioSource.uri(
      Uri.parse('https://server10.mp3quran.net/minsh/113.mp3'),
      tag: 'Surah Al-Falaq',
    ),
  ];

  @override
  void initState() {
    super.initState();
    _player = AudioPlayer()..setAudioSources(_playlist);
  }

  @override
  void dispose() {
    _player.dispose();
    super.dispose();
  }

  String _format(Duration d) {
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$m:$s';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('just_audio_windows_plus'),
        centerTitle: true,
      ),
      body: Center(
        child: SizedBox(
          width: 360,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              // Track Title
              StreamBuilder<SequenceState?>(
                stream: _player.sequenceStateStream,
                builder: (context, snapshot) {
                  final title = snapshot.data?.currentSource?.tag as String? ?? 'Loading...';
                  return Text(
                    title,
                    style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold),
                  );
                },
              ),
              const SizedBox(height: 20),

              // Seek Bar & Timestamps
              StreamBuilder<Duration?>(
                stream: _player.durationStream,
                initialData: _player.duration,
                builder: (context, durSnap) {
                  final total = durSnap.data ?? _player.duration ?? Duration.zero;

                  return StreamBuilder<Duration>(
                    stream: _player.positionStream,
                    initialData: _player.position,
                    builder: (context, posSnap) {
                      final pos = posSnap.data ?? _player.position;
                      final max = total > Duration.zero ? total.inMilliseconds.toDouble() : 1.0;
                      final value = total > Duration.zero
                          ? pos.inMilliseconds.toDouble().clamp(0.0, max)
                          : 0.0;

                      return Column(
                        children: [
                          ExcludeSemantics(
                            child: Slider(
                              value: value,
                              max: max,
                              onChanged: total > Duration.zero
                                  ? (v) => _player.seek(Duration(milliseconds: v.round()))
                                  : null,
                            ),
                          ),
                          Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 16),
                            child: Row(
                              mainAxisAlignment: MainAxisAlignment.spaceBetween,
                              children: [
                                Text(_format(pos)),
                                Text(_format(total)),
                              ],
                            ),
                          ),
                        ],
                      );
                    },
                  );
                },
              ),
              const SizedBox(height: 12),

              // Playback Controls
              StreamBuilder<PlayerState>(
                stream: _player.playerStateStream,
                builder: (context, playerSnapshot) {
                  final state = playerSnapshot.data;
                  final isPlaying = state?.playing ?? false;
                  final processing = state?.processingState ?? ProcessingState.idle;
                  final isLoading = processing == ProcessingState.loading || processing == ProcessingState.buffering;

                  return StreamBuilder<SequenceState?>(
                    stream: _player.sequenceStateStream,
                    builder: (context, seqSnapshot) {
                      return Row(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          IconButton(
                            iconSize: 36,
                            icon: const Icon(Icons.skip_previous),
                            onPressed: (_player.hasPrevious && !isLoading)
                                ? _player.seekToPrevious
                                : null,
                          ),
                          const SizedBox(width: 8),
                          isLoading
                              ? const SizedBox(
                                  width: 52,
                                  height: 52,
                                  child: Padding(
                                    padding: EdgeInsets.all(12),
                                    child: CircularProgressIndicator(strokeWidth: 2.5),
                                  ),
                                )
                              : IconButton(
                                  iconSize: 52,
                                  icon: Icon(isPlaying ? Icons.pause_circle_filled : Icons.play_circle_filled),
                                  onPressed: isPlaying ? _player.pause : _player.play,
                                ),
                          const SizedBox(width: 8),
                          IconButton(
                            iconSize: 36,
                            icon: const Icon(Icons.skip_next),
                            onPressed: (_player.hasNext && !isLoading)
                                ? _player.seekToNext
                                : null,
                          ),
                        ],
                      );
                    },
                  );
                },
              ),
              const SizedBox(height: 16),

              // Volume Slider
              Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  const Icon(Icons.volume_up, size: 20),
                  SizedBox(
                    width: 140,
                    child: ExcludeSemantics(
                      child: Slider(
                        value: _volume,
                        onChanged: (v) {
                          setState(() => _volume = v);
                          _player.setVolume(v);
                        },
                      ),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}
