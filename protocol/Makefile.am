EXTRA_DIST =					\
	desktop-shell.xml			\
	screenshooter.xml			\
	tablet-shell.xml			\
	xserver.xml				\
	text.xml				\
	input-method.xml			\
	workspaces.xml				\
	subsurface.xml				\
	text-cursor-position.xml		\
	wayland-test.xml

if HAVE_XMLLINT
validate: $(EXTRA_DIST)
	$(AM_V_GEN)$(XMLLINT) --noout --dtdvalid $(dtddir)/wayland.dtd $^

all-local: validate

endif
