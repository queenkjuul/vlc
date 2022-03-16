emscriptendir = $(pluginsdir)/emscripten

libglconverter_emscripten_plugin_la_SOURCES = hw/emscripten/converter.cpp
libglconverter_emscripten_plugin_la_CPPFLAGS = $(AM_CPPFLAGS)
libglconverter_emscripten_plugin_la_CXXFLAGS = $(AM_CXXFLAGS)
libglconverter_emscripten_plugin_la_LIBADD = $(AM_LIBADD) 

if HAVE_EMSCRIPTEN
emscripten_LTLIBRARIES = libglconverter_emscripten_plugin.la
endif
