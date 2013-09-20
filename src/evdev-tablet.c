/*
 * Copyright © 2013 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include "compositor.h"
#include "evdev.h"
#include "zalloc.h"

struct tablet_dispatch {
	struct evdev_dispatch base;
	struct evdev_device *device;

	struct weston_tablet *tablet;
};

static void
tablet_process(struct evdev_dispatch *dispatch,
	       struct evdev_device *device,
	       struct input_event *e,
	       uint32_t time)
{
	struct tablet_dispatch *tablet =
		(struct tablet_dispatch *) dispatch;
}


static void
tablet_destroy(struct evdev_dispatch *dispatch)
{
	struct tablet_dispatch *tablet =
		(struct tablet_dispatch *) dispatch;
	struct weston_tablet *t = tablet->tablet;

	wl_list_remove(&tablet->tablet->describe_listener.link);
	weston_tablet_destroy(t);

	free(dispatch);
}

static void
tablet_describe(struct wl_listener *listener, void *data)
{
	struct wl_resource *resource = data;
	struct weston_tablet *t = wl_resource_get_user_data(resource);
	struct wl_array buttons;
	uint32_t *p;

	/* FIXME: evdev.c doesn't cache this atm */
	notify_tablet_capability_axis(resource, ABS_X, 0, 1000, 0, 0, 0);
	notify_tablet_capability_axis(resource, ABS_Y, 0, 1000, 0, 0, 0);
	notify_tablet_capability_axis(resource, ABS_PRESSURE, 0, 1000, 0, 0, 0);

	wl_array_init(&buttons);
	p = wl_array_add(&buttons, sizeof *p);
	*p = BTN_STYLUS;

	notify_tablet_capability_button(resource, &buttons);
}

struct evdev_dispatch_interface tablet_interface = {
	tablet_process,
	tablet_destroy
};

static int
tablet_init(struct tablet_dispatch *tablet,
	    struct evdev_device *device)
{
	struct weston_tablet *t;

	tablet->base.interface = &tablet_interface;
	tablet->device = device;

	t = weston_tablet_create(device->devname,
				 device->ids.vendor,
				 device->ids.product);
	if (!tablet)
		return 1;

	weston_seat_init_tablet_manager(device->seat);
	if (!device->seat->tablet_manager) {
		weston_tablet_destroy(t);
		return 1;
	}

	tablet->tablet = t;

	weston_tablet_manager_add_device(device->seat->tablet_manager, t);

	t->describe_listener.notify = tablet_describe;
	wl_signal_add(&t->describe_signal, &t->describe_listener);

	return 0;
}

struct evdev_dispatch *
evdev_tablet_create(struct evdev_device *device)
{
	struct tablet_dispatch *tablet;

	tablet = zalloc(sizeof *tablet);

	if (tablet == NULL)
		return NULL;

	if (tablet_init(tablet, device) != 0) {
		free(tablet);
		return NULL;
	}

	return &tablet->base;
}
