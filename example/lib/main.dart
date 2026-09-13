import 'package:flutter/material.dart';
import 'package:just_audio/just_audio.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const JustAudioWindowsPlusApp());
}

class JustAudioWindowsPlusApp extends StatelessWidget {
  const JustAudioWindowsPlusApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'just_audio_windows_plus Showcase',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        colorSchemeSeed: Colors.teal,
        scaffoldBackgroundColor: const Color(0xFF121418),
      ),
      home: const AudioPlayerScreen(),
    );
  }
}

class AudioPlayerScreen extends StatefulWidget {
  const AudioPlayerScreen({super.key});

  @override
  State<AudioPlayerScreen> createState() => _AudioPlayerScreenState();
}

class _AudioPlayerScreenState extends State<AudioPlayerScreen> {
  late final AudioPlayer _player;
  double _volume = 1.0;
  double _speed = 1.0;

  final _playlist = ConcatenatingAudioSource(
    children: [
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
    ],
  );

  @override
  void initState() {
    super.initState();
    _player = AudioPlayer();
    _loadPlaylist();
  }

  Future<void> _loadPlaylist() async {
    try {
      await _player.setAudioSource(_playlist);
    } catch (e) {
      debugPrint('Error loading playlist: $e');
    }
  }

  @override
  void dispose() {
    _player.dispose();
    super.dispose();
  }

  String _formatDuration(Duration d) {
    final minutes = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final seconds = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$minutes:$seconds';
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Scaffold(
      appBar: AppBar(
        title: const Text('just_audio_windows_plus'),
        centerTitle: true,
        backgroundColor: Colors.transparent,
      ),
      body: Center(
        child: Container(
          constraints: const BoxConstraints(maxWidth: 460),
          margin: const EdgeInsets.all(24),
          padding: const EdgeInsets.symmetric(horizontal: 28, vertical: 32),
          decoration: BoxDecoration(
            color: const Color(0xFF1E222B),
            borderRadius: BorderRadius.circular(24),
            border: Border.all(color: Colors.white.withAlpha(20)),
            boxShadow: [
              BoxShadow(
                color: Colors.black.withAlpha(80),
                blurRadius: 24,
                offset: const Offset(0, 8),
              ),
            ],
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              // 1. Surah Icon & Track Title
              Container(
                width: 64,
                height: 64,
                decoration: BoxDecoration(
                  color: theme.colorScheme.primary.withAlpha(35),
                  shape: BoxShape.circle,
                ),
                child: Icon(
                  Icons.menu_book_rounded,
                  size: 32,
                  color: theme.colorScheme.primary,
                ),
              ),
              const SizedBox(height: 16),
              StreamBuilder<SequenceState?>(
                stream: _player.sequenceStateStream,
                builder: (context, snapshot) {
                  final title = snapshot.data?.currentSource?.tag as String? ?? 'Loading...';
                  return Column(
                    children: [
                      Text(
                        title,
                        style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.bold),
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 4),
                      Text(
                        'Sheikh Muhammad Siddiq Al-Minshawi',
                        style: theme.textTheme.bodyMedium?.copyWith(color: Colors.white60),
                      ),
                    ],
                  );
                },
              ),
              const SizedBox(height: 24),

              // 2. Seek Bar with Timestamps
              StreamBuilder<Duration>(
                stream: _player.positionStream,
                builder: (context, snapshot) {
                  final position = snapshot.data ?? Duration.zero;
                  final total = _player.duration ?? Duration.zero;
                  final maxMs = total.inMilliseconds > 0 ? total.inMilliseconds.toDouble() : 1.0;
                  final currentMs = position.inMilliseconds.toDouble().clamp(0.0, maxMs);

                  return Column(
                    children: [
                      Slider(
                        min: 0.0,
                        max: maxMs,
                        value: currentMs,
                        activeColor: theme.colorScheme.primary,
                        inactiveColor: Colors.white12,
                        onChanged: (val) => _player.seek(Duration(milliseconds: val.round())),
                      ),
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 14),
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.spaceBetween,
                          children: [
                            Text(_formatDuration(position), style: theme.textTheme.bodySmall?.copyWith(color: Colors.white54)),
                            Text(_formatDuration(total), style: theme.textTheme.bodySmall?.copyWith(color: Colors.white54)),
                          ],
                        ),
                      ),
                    ],
                  );
                },
              ),
              const SizedBox(height: 12),

              // 3. Primary Playback Controls
              Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  IconButton(
                    iconSize: 36,
                    icon: const Icon(Icons.skip_previous_rounded),
                    tooltip: 'Previous Track',
                    onPressed: () async {
                      if (_player.hasPrevious) await _player.seekToPrevious();
                    },
                  ),
                  const SizedBox(width: 12),
                  StreamBuilder<PlayerState>(
                    stream: _player.playerStateStream,
                    builder: (context, snapshot) {
                      final state = snapshot.data;
                      final isPlaying = state?.playing ?? false;
                      final processing = state?.processingState ?? ProcessingState.idle;

                      if (processing == ProcessingState.loading || processing == ProcessingState.buffering) {
                        return SizedBox(
                          width: 56,
                          height: 56,
                          child: Padding(
                            padding: const EdgeInsets.all(12),
                            child: CircularProgressIndicator(
                              strokeWidth: 3,
                              color: theme.colorScheme.primary,
                            ),
                          ),
                        );
                      }

                      return IconButton(
                        iconSize: 56,
                        color: theme.colorScheme.primary,
                        icon: Icon(
                          isPlaying ? Icons.pause_circle_filled_rounded : Icons.play_circle_fill_rounded,
                        ),
                        tooltip: isPlaying ? 'Pause' : 'Play',
                        onPressed: isPlaying ? _player.pause : _player.play,
                      );
                    },
                  ),
                  const SizedBox(width: 12),
                  IconButton(
                    iconSize: 36,
                    icon: const Icon(Icons.skip_next_rounded),
                    tooltip: 'Next Track',
                    onPressed: () async {
                      if (_player.hasNext) await _player.seekToNext();
                    },
                  ),
                ],
              ),
              const SizedBox(height: 16),
              const Divider(color: Colors.white10),
              const SizedBox(height: 12),

              // 4. Secondary Controls: Volume & Playback Speed
              Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  IconButton(
                    iconSize: 20,
                    icon: Icon(_volume == 0 ? Icons.volume_off_rounded : Icons.volume_up_rounded),
                    color: Colors.white70,
                    tooltip: _volume == 0 ? 'Unmute' : 'Mute',
                    onPressed: () {
                      final next = _volume == 0 ? 1.0 : 0.0;
                      setState(() => _volume = next);
                      _player.setVolume(next);
                    },
                  ),
                  SizedBox(
                    width: 120,
                    child: Slider(
                      value: _volume,
                      activeColor: Colors.white70,
                      inactiveColor: Colors.white12,
                      onChanged: (val) {
                        setState(() => _volume = val);
                        _player.setVolume(val);
                      },
                    ),
                  ),
                  const SizedBox(width: 20),
                  Tooltip(
                    message: 'Playback Speed',
                    child: OutlinedButton.icon(
                      style: OutlinedButton.styleFrom(
                        foregroundColor: Colors.white70,
                        side: const BorderSide(color: Colors.white24),
                        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                      ),
                      icon: const Icon(Icons.speed_rounded, size: 16),
                      label: Text('${_speed}x'),
                      onPressed: () {
                        final next = _speed == 1.0 ? 1.25 : (_speed == 1.25 ? 1.5 : 1.0);
                        setState(() => _speed = next);
                        _player.setSpeed(next);
                      },
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
