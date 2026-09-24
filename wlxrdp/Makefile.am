# wlxrdp: the xup backend for wlroots compositors (Wayland sessions).
# Started by sesexec for SCP_SESSION_TYPE_WAYLAND; the encoding is
# xrdp_accel_assist's (-w), which wlxrdp starts per connection.

AM_CPPFLAGS = \
  -DXRDP_LIBEXEC_PATH=\"${pkglibexecdir}\" \
  -I$(top_srcdir)/common \
  -I$(top_srcdir)/xrdp_accel_assist \
  $(XRDP_WAYLAND_CFLAGS)

pkglibexec_PROGRAMS = \
  wlxrdp

PROTOCOL_SOURCES = \
  ext-foreign-toplevel-list-v1-client-protocol.h \
  ext-foreign-toplevel-list-v1-protocol.c \
  ext-image-capture-source-v1-client-protocol.h \
  ext-image-capture-source-v1-protocol.c \
  ext-image-copy-capture-v1-client-protocol.h \
  ext-image-copy-capture-v1-protocol.c \
  linux-dmabuf-v1-client-protocol.h \
  linux-dmabuf-v1-protocol.c \
  wlr-virtual-pointer-unstable-v1-client-protocol.h \
  wlr-virtual-pointer-unstable-v1-protocol.c \
  virtual-keyboard-unstable-v1-client-protocol.h \
  virtual-keyboard-unstable-v1-protocol.c \
  wlr-output-management-unstable-v1-client-protocol.h \
  wlr-output-management-unstable-v1-protocol.c

BUILT_SOURCES = $(PROTOCOL_SOURCES)
CLEANFILES = $(PROTOCOL_SOURCES)

nodist_wlxrdp_SOURCES = $(PROTOCOL_SOURCES)
wlxrdp_SOURCES = \
  wlxrdp.c \
  wla_wlr.c \
  wlxrdp_cpu.c \
  wlxrdp_cpu.h \
  wla.h

wlxrdp_LDADD = \
  $(top_builddir)/common/libcommon.la \
  $(XRDP_WAYLAND_LIBS)

# the compositor contract; the wlroots protocols, from wlr-protocols /
# wlroots (MIT)
EXTRA_DIST = \
  COMPOSITOR.md \
  protocols/virtual-keyboard-unstable-v1.xml \
  protocols/wlr-virtual-pointer-unstable-v1.xml \
  protocols/wlr-output-management-unstable-v1.xml

WP_STAGING = $(WAYLAND_PROTOCOLS_DIR)/staging
FOREIGN_TOPLEVEL_XML = $(WP_STAGING)/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
CAPTURE_SOURCE_XML = $(WP_STAGING)/ext-image-capture-source/ext-image-capture-source-v1.xml
COPY_CAPTURE_XML = $(WP_STAGING)/ext-image-copy-capture/ext-image-copy-capture-v1.xml
LINUX_DMABUF_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml
VIRTUAL_POINTER_XML = $(srcdir)/protocols/wlr-virtual-pointer-unstable-v1.xml
VIRTUAL_KEYBOARD_XML = $(srcdir)/protocols/virtual-keyboard-unstable-v1.xml
OUTPUT_MANAGEMENT_XML = $(srcdir)/protocols/wlr-output-management-unstable-v1.xml

ext-foreign-toplevel-list-v1-client-protocol.h: $(FOREIGN_TOPLEVEL_XML)
	$(WAYLAND_SCANNER) client-header $< $@
ext-foreign-toplevel-list-v1-protocol.c: $(FOREIGN_TOPLEVEL_XML)
	$(WAYLAND_SCANNER) private-code $< $@
ext-image-capture-source-v1-client-protocol.h: $(CAPTURE_SOURCE_XML)
	$(WAYLAND_SCANNER) client-header $< $@
ext-image-capture-source-v1-protocol.c: $(CAPTURE_SOURCE_XML)
	$(WAYLAND_SCANNER) private-code $< $@
ext-image-copy-capture-v1-client-protocol.h: $(COPY_CAPTURE_XML)
	$(WAYLAND_SCANNER) client-header $< $@
ext-image-copy-capture-v1-protocol.c: $(COPY_CAPTURE_XML)
	$(WAYLAND_SCANNER) private-code $< $@
linux-dmabuf-v1-client-protocol.h: $(LINUX_DMABUF_XML)
	$(WAYLAND_SCANNER) client-header $< $@
linux-dmabuf-v1-protocol.c: $(LINUX_DMABUF_XML)
	$(WAYLAND_SCANNER) private-code $< $@
wlr-virtual-pointer-unstable-v1-client-protocol.h: $(VIRTUAL_POINTER_XML)
	$(WAYLAND_SCANNER) client-header $< $@
wlr-virtual-pointer-unstable-v1-protocol.c: $(VIRTUAL_POINTER_XML)
	$(WAYLAND_SCANNER) private-code $< $@
virtual-keyboard-unstable-v1-client-protocol.h: $(VIRTUAL_KEYBOARD_XML)
	$(WAYLAND_SCANNER) client-header $< $@
virtual-keyboard-unstable-v1-protocol.c: $(VIRTUAL_KEYBOARD_XML)
	$(WAYLAND_SCANNER) private-code $< $@
wlr-output-management-unstable-v1-client-protocol.h: $(OUTPUT_MANAGEMENT_XML)
	$(WAYLAND_SCANNER) client-header $< $@
wlr-output-management-unstable-v1-protocol.c: $(OUTPUT_MANAGEMENT_XML)
	$(WAYLAND_SCANNER) private-code $< $@
