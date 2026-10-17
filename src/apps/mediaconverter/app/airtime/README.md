# airTime's controller

`ControlBar` and `Look` are airTime's movie controller and its drawing
(jmgasper/airTime, `src/ui`, commit a6d464f), so that the audio preview in
MediaConverter plays songs with the same controls as airTime and its Quick
Look panel. Changes from airTime's copy: `Messages.h` is `AirTimeMessages.h`
(MediaConverter has a `Messages.h` of its own), and `TimeFormat.h` holds the
one helper of airTime's engine they use (`format_time`). Take fixes from
airTime rather than changing these here.
