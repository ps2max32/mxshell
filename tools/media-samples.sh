#!/bin/sh
# Sample files for Media's tests: photos, videos and music under <dir>/Media,
# downloaded (Wikimedia Commons, Blender's Big Buck Bunny) and made into the
# kinds Media plays with ffmpeg and Python's PIL.
#
#   tools/media-samples.sh <dir>        (make media-shot uses build/pcsx2-test)
#   BBB=<a copy of Big Buck Bunny> tools/media-samples.sh <dir>   (no 121 MB download)
#
# The files and their licences:
#   Photos: Wikimedia Commons, CC BY-SA 3.0 / 4.0 (their pages name the
#           authors); "Tower (portrait).png" is a part of one as a PNG,
#           "Progressive.jpg" one saved as a progressive JPEG (Media says it
#           cannot show it)
#   Videos: Big Buck Bunny, (c) Blender Foundation, CC BY 3.0: 30 s at 640 x
#           384 with MP2 sound, at 720 x 576 with P and B pictures and LPCM,
#           and 6 s of I pictures
#   Music:  Satie's Gymnopedies (public domain recordings, CC BY 3.0 for the
#           MP3) as MP2 and WAV; the MP3 itself (Media lists it, does not play it)
set -eu
OUT=${1:?usage: media-samples.sh <dir>}/Media
UA="ps2max-media-samples/1.0"
mkdir -p "$OUT/Photos" "$OUT/Videos" "$OUT/Music"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
get() { curl -fsSL -A "$UA" -o "$2" "$1"; sleep 1; }

C=https://upload.wikimedia.org/wikipedia/commons
T=https://upload.wikimedia.org/wikipedia/commons/thumb
get "$T/7/79/Panoramaweg_tussen_Waltensburg-Vuorz_en_Breil-Brigels_%28actm%29_10.jpg/1920px-Panoramaweg_tussen_Waltensburg-Vuorz_en_Breil-Brigels_%28actm%29_10.jpg" "$OUT/Photos/Alps path.jpg"
get "$T/a/a5/Ponta_de_S%C3%A3o_Louren%C3%A7o_north_north_east.jpg/1920px-Ponta_de_S%C3%A3o_Louren%C3%A7o_north_north_east.jpg" "$OUT/Photos/Ponta de Sao Lourenco.jpg"
get "$T/9/94/Reformierte_Kirche_Scuol._11-10-2024._%28actm.%29_02.jpg/1920px-Reformierte_Kirche_Scuol._11-10-2024._%28actm.%29_02.jpg" "$OUT/Photos/Church in Scuol.jpg"
get "$T/b/bf/Vihorlat_%28v_zime%29_046.jpg/1920px-Vihorlat_%28v_zime%29_046.jpg" "$OUT/Photos/Vihorlat in winter.jpg"
python3 -I - "$OUT/Photos" <<'EOF'
import sys
from PIL import Image
d = sys.argv[1]
Image.open(d + "/Church in Scuol.jpg").crop((600, 0, 1453, 1280)).save(d + "/Tower (portrait).png")
Image.open(d + "/Alps path.jpg").save(d + "/Progressive.jpg", progressive=True, quality=85)
EOF

if [ -z "${BBB:-}" ]; then
  get https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_640x360.m4v.zip "$TMP/bbb.zip"
  python3 -I -m zipfile -e "$TMP/bbb.zip" "$TMP/bbb"
  BBB=$(find "$TMP/bbb" -name '*.m4v' | head -n 1)
fi
ffmpeg -v error -y -ss 60 -t 30 -i "$BBB" -vf scale=640:384 -c:v mpeg2video -q:v 4 -g 12 \
  -c:a mp2 -b:a 192k -ar 48000 -ac 2 -f vob "$OUT/Videos/Big Buck Bunny.mpg"
ffmpeg -v error -y -ss 60 -t 30 -i "$BBB" -vf scale=720:576 -c:v mpeg2video -q:v 5 -g 12 -bf 2 \
  -c:a pcm_s16be -ar 48000 -ac 2 -f vob -muxrate 40000000 "$OUT/Videos/Big Buck Bunny 576.mpg"
ffmpeg -v error -y -ss 120 -t 6 -i "$BBB" -vf scale=640:384 -c:v mpeg2video -g 1 -bf 0 -q:v 5 \
  -c:a pcm_s16be -ar 48000 -ac 2 -f vob -muxrate 40000000 "$OUT/Videos/Short clip.mpg"

get "$C/9/90/Erik_Satie_-_gymnopedies_-_la_1_ere._lent_et_douloureux.ogg" "$TMP/satie1.ogg"
get "$C/0/05/Debussy_Gymnopedie_1%2C_arrangement_of_Satie%E2%80%99s_Gymnopedie_3.wav" "$OUT/Music/Debussy - Gymnopedie 1.wav"
get "$C/2/20/Gymnopedie_No._2_%28ISRC_USUAN1100786%29.mp3" "$OUT/Music/Satie - Gymnopedie 2 (MP3).mp3"
ffmpeg -v error -y -i "$TMP/satie1.ogg" -c:a mp2 -b:a 192k -ar 48000 "$OUT/Music/Satie - Gymnopedie 1.mp2"
ffmpeg -v error -y -i "$OUT/Music/Satie - Gymnopedie 2 (MP3).mp3" -t 60 -c:a mp2 -b:a 192k -ar 44100 \
  "$OUT/Music/Satie - Gymnopedie 2.mp2"
echo "media-samples: $OUT"
ls -la "$OUT"/*
