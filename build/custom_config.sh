#!/bin/bash

cd ../contrib &&  mkdir -p build && cd build && ../bootstrap --disable-a52 --disable-aribb24 --disable-bitstream --disable-faad2 --disable-gettext --disable-glslang --disable-gme --disable-goom --disable-libebur128 --disable-live555 --disable-mpcdec --disable-mysofa --disable-nfs --disable-nvcodec --disable-protobuf --disable-rav1e --disable-rnnoise --disable-sidplay2 --disable-smb2 --enable-medialibrary --enable-upnp && make -j9 && cd ../../build

VLC_CONFIGURE_OPTIONS="--disable-mpc --disable-libplacebo --with-contrib=/var/home/asenat/workspaces/vlc/vlc2/contrib/x86_64-redhat-linux/ --disable-qt --enable-shared"
#VLC_CONFIGURE_OPTIONS="
#    --enable-debug
#    --enable-shared
#    --disable-dvbpsi
#    --disable-dav1d  --enable-vpx 
#    --enable-merge-ffmpeg --enable-avcodec --enable-avformat
#    --disable-opus --disable-faad 
#    --disable-gles2 
#    --disable-xcb  
#    --enable-sout --disable-lua --disable-vlm 
#    --disable-archive --disable-live555 --disable-dc1394 --disable-dv1394 
#    --disable-linsys --disable-dvdread --disable-dvdnav --disable-bluray 
#    --disable-opencv --disable-smbclient --disable-dsm
#    --disable-sftp --disable-nfs --disable-smb2 --disable-v4l2 --disable-nvdec --disable-decklink
#    --disable-vcd --disable-libcddb --disable-screen --disable-vnc
#    --disable-freerdp --disable-asdcp  --disable-gme --disable-sid
#    --disable-ogg --disable-shout --disable-matroska --disable-mod --disable-mpc --disable-shine 
#    --disable-omxil --disable-rpi-omxil --disable-mad --disable-mpg123 --disable-gst-decode --disable-libva 
#    --disable-dxva2 --disable-d3d11va --disable-swscale --disable-postproc --disable-aom --disable-twolame 
#    --disable-fdkaac --disable-a52 --disable-dca --disable-flac --disable-libmpeg2 --disable-vorbis --disable-tremor
#    --disable-speex --disable-spatialaudio --disable-theora --disable-oggspots --disable-daala --disable-schroedinger
#    --disable-png --disable-jpeg --disable-bpg --disable-x262 --disable-x265 --enable-x264 --disable-x26410b --disable-mfx 
#    --disable-fluidsynth --disable-fluidlite --disable-zvbi --disable-telx --disable-aribsub --disable-aribb25
#    --disable-kate --disable-tiger --disable-css --disable-libplacebo --disable-vulkan --disable-vdpau --enable-wayland 
#    --disable-sdl-image --disable-freetype --disable-fribidi --disable-harfbuzz --disable-fontconfig --disable-libass --disable-svg
#    --disable-svgdec --disable-directx--disable-kms --disable-caca --disable-kva --disable-mmal --disable-alsa --disable-oss 
#    --disable-sndio --disable-wasapi --disable-jack --disable-opensles --disable-samplerate --disable-soxr --disable-kai --disable-chromaprint
#    --disable-chromecast --enable-qt --enable-qt-qml-cache --disable-qt-qml-debug --disable-skins2 --disable-libtar --disable-sparkle
#    --disable-ncurses --disable-lirc --disable-srt --disable-goom --disable-projectm --disable-vsxu --disable-avahi
#    --disable-udev --disable-mtp --enable-upnp --disable-microdns --disable-libxml2 --disable-libgcrypt --disable-gnutls --disable-taglib
#    --disable-secret --disable-kwallet --disable-update-check --disable-notify --enable-medialibrary --enable-vlc --disable-addonmanagermodules 
#    --disable-ssp
#"

#CXX="g++" \
#CXXFLAGS="-O0 -g3 -fno-omit-frame-pointer -DDEBUG" \
#CC="gcc" \
#CFLAGS="-O0 -g3 -fno-omit-frame-pointer -DDEBUG" \
../configure $VLC_CONFIGURE_OPTIONS
#PKG_CONFIG_PATH="/var/home/asenat/workspaces/vlc/vlc/contrib/x86_64-redhat-linux/lib/pkgconfig" \
