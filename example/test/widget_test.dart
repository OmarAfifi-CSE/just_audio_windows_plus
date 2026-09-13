import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:just_audio_windows_plus_example/main.dart';

void main() {
  testWidgets('AudioPlayerExample renders with ExcludeSemantics on sliders', (tester) async {
    await tester.pumpWidget(const MaterialApp(
      home: AudioPlayerExample(),
    ));

    // Verify title and app bar
    expect(find.text('just_audio_windows_plus'), findsOneWidget);

    // Verify sliders exist and are wrapped inside ExcludeSemantics
    final sliders = find.byType(Slider);
    expect(sliders, findsNWidgets(2)); // Seek slider + Volume slider

    // Verify both sliders are children of ExcludeSemantics
    final excludedSemantics = find.byType(ExcludeSemantics);
    expect(excludedSemantics, findsWidgets);

    // Verify control icons exist
    expect(find.byIcon(Icons.skip_previous), findsOneWidget);
    expect(find.byIcon(Icons.skip_next), findsOneWidget);
    expect(find.byIcon(Icons.volume_up), findsOneWidget);
  });
}
