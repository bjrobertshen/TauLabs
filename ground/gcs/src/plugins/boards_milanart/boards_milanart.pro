TEMPLATE = lib
TARGET = MilanArt
include(../../taulabsgcsplugin.pri)
include(../../plugins/uavobjects/uavobjects.pri)
include(../../plugins/coreplugin/coreplugin.pri)

OTHER_FILES += Milanart.pluginspec

HEADERS += \
    Milanartplugin.h \
    qqflying.h

SOURCES += \
    milanartplugin.cpp \
    qqflying.cpp

RESOURCES += \
    milanart.qrc
