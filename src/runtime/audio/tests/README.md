# Streaming music checks

The host-driven fixture calls the guest AVAudioPlayer and ExtAudioFile entry
points, renders the same mixer used by the platform, and checks time, audio and
memory in phone mode. MP3 PCM is compared to a standalone upstream minimp3
decode. Ogg seeks are compared to FFmpeg's Vorbis decoder. No audio fixture is
stored in Git.

Build `hvm` and `hvm_tests` with the Linux CMake configuration and the FFmpeg
development libraries enabled. Compile the focused test with the libraries
selected by your build. For a Linux build with pkg-config, run this from the
repository root.

```bash
clang++ -std=c++20 -O2 -ffp-contract=off -Isrc \
  -x c++ src/runtime/audio/tests/StreamingAudioTests.inc -x none \
  build/libhvm_core.a -o build/streaming-audio-tests -pthread \
  $(pkg-config --libs zlib sqlite3 icu-uc icu-i18n openssl libavformat libavcodec libavutil)
```

Generate the tracks and the independent Ogg PCM with FFmpeg.

```bash
fixtures=$(mktemp -d)
signal='aevalsrc=0.16*sin(2*PI*(311*t+0.125*t*t))|0.12*sin(2*PI*(571*t+0.125*t*t)):s=44100:d=240'
ffmpeg -v error -f lavfi -i "$signal" -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0 "$fixtures/long.mp3"
ffmpeg -v error -f lavfi -i "$signal" -c:a libvorbis -q:a 4 "$fixtures/long.ogg"
ffmpeg -v error -f lavfi -i "$signal" -c:a aac -b:a 128k "$fixtures/long.m4a"
ffmpeg -v error -f lavfi -i "$signal" -c:a pcm_s16le "$fixtures/long.wav"
ffmpeg -v error -f lavfi -i "$signal" -c:a pcm_s16le "$fixtures/long.caf"
ffmpeg -v error -i "$fixtures/long.ogg" -c:a pcm_s16le -f s16le "$fixtures/ogg-reference.pcm"
for format in mp3 ogg m4a wav caf; do
  mkdir "$fixtures/home-$format"
  build/streaming-audio-tests player "$fixtures/long.$format" "$fixtures/home-$format"
done
build/streaming-audio-tests mp3-reference "$fixtures/long.mp3"
build/streaming-audio-tests ogg-reference "$fixtures/long.ogg" "$fixtures/ogg-reference.pcm"
```

Run each player in a fresh process for its RSS measurement. The test resets
Linux's process high-water mark immediately before opening the URL.
