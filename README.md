## ASCI-Video-Play
ascii-video-play is a modified version of decode_filter_video.c from the ffmpeg examples in its source repo. It plays a video in the terminal using ascii characters for pixel grey scale levels.

## Installation (msys2\mingw64)
```bash
pacman -S mingw-w64-x86_64-ffmpeg
```

## Running (Tested with ffmpeg-7.1.1)
```bash
gcc -o ascii-video-play ascii-video-play.c $(pkg-config --cflags --libs libavformat libavcodec libavfilter libavutil)

./ascii-video-play.exe <video-file-path>
```
