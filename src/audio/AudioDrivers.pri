include($$PWD/base/Base.pri)

win32: include($$PWD/wasapi/WasapiCore.pri)
else: {
    USE_PULSEAUDIO: include($$PWD/pulseaudio/PulseaudioCore.pri)
    else: include($$PWD/pipewire/PipewireCore.pri)
}

