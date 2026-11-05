audio_ss = ss.source_set()
audio_ss.add(files(
  'audio.c',
  'audio-be.c',
  'audio-mixeng-be.c',
  'mixeng.c',
  'noaudio.c',
  'wavaudio.c',
))

# deprecated since v10.2, to be removed
if have_hmp
endif

audio_modules = {}
foreach m : [
  ['alsa', alsa, files()],
  ['coreaudio', coreaudio, files()],
  ['dsound', dsound, files( )],
  ['oss', oss, files()],
  ['pa', pulse, files()],
  ['sdl', sdl, files()],
  ['jack', jack, files()],
  ['sndio', sndio, files()],
  ['pipewire', pipewire, files()],
  ['spice', spice, files()]
]
  if m[1].found()
    module_ss = ss.source_set()
    module_ss.add(m[1], m[2])
    audio_modules += {m[0] : module_ss}
  endif
endforeach

if dbus_display
    module_ss = ss.source_set()
    module_ss.add(when: [gio, pixman],
                  if_true: [dbus_display1_h, files()])
    audio_modules += {'dbus': module_ss}
endif

modules += {'audio': audio_modules}
