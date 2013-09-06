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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "compositor.h"
#include "evdev.h"
#include "udev-seat.h"
#include "input-device-server-protocol.h"


struct input_device_backend {
	struct weston_compositor *compositor;

	struct wl_listener seat_created_listener;
	struct wl_listener destroy_listener;
};

struct input_device_manager {
	struct wl_global *input_device_manager_global;
	struct weston_seat *seat;
	struct wl_resource *resource;
	/**
	 * List of physical input devices attached to this seat. This is the
	 * set of wl_resources we can send input events to.
	 */
	struct wl_list input_device_list;
	struct wl_signal destroy_signal;
	struct wl_listener destroy_listener;
};

struct input_device {
	struct wl_resource *resource;
	struct wl_list link;
	struct evdev_device *dev;
	struct input_device_manager *manager;
	struct wl_signal destroy_signal;
	struct wl_listener destroy_listener;
};


static void
unbind_input_device_manager(struct wl_resource *resource)
{
	struct input_device_manager *input_device_manager = wl_resource_get_user_data(resource);

	input_device_manager->resource = NULL;
}

static void release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void get_axes(struct wl_client *client, struct wl_resource *resource)
{
	struct input_device *input_dev = wl_resource_get_user_data(resource);
	struct evdev_device *dev = input_dev->dev;

	if (dev->caps & EVDEV_MOTION_ABS) {
		/* FIXME: actually send the axes we have */
		wl_input_device_send_axis_capability(resource,
					WL_INPUT_DEVICE_AXIS_ABSOLUTE_AXIS | ABS_X,
					dev->abs.min_x, dev->abs.max_x,
					0, 0, 0);
		wl_input_device_send_axis_capability(resource,
					WL_INPUT_DEVICE_AXIS_ABSOLUTE_AXIS | ABS_Y,
					dev->abs.min_y, dev->abs.max_y,
					0, 0, 0);
	}

}

WL_EXPORT void
notify_extra_axis(struct weston_seat *seat, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct weston_pointer *pointer = seat->pointer;

	/* FIXME: adjust axis value */

	if (pointer->focus_resource_input_device) {
		wl_input_device_send_axis(pointer->focus_resource_input_device, time, axis, value);
	}
}

WL_EXPORT void
notify_frame(struct weston_seat *seat, uint32_t time)
{
	struct weston_pointer *pointer = seat->pointer;

	weston_log("sending frame\n");

	if (pointer->focus_resource_input_device)
		wl_input_device_send_frame(pointer->focus_resource_input_device, time);
}

const struct wl_input_device_interface device_interface = {
	release,
	get_axes,
};

WL_EXPORT void
input_device_set_focus(struct weston_pointer *pointer, struct weston_surface *surface)
{
	struct wl_resource *focus_resource;
	struct input_device_manager *input_device_manager = pointer->seat->input_device_manager;

	focus_resource = find_resource_for_surface(&input_device_manager->input_device_list, surface);
	if (focus_resource) {
		weston_log("::: enter event axis update \n");
		/* FIXME: for all axes, call wl_input_device_send_axis() */
		wl_input_device_send_frame(focus_resource, 0 /* FIXME: time */);
	}

	pointer->focus_resource_input_device = focus_resource;
}

static void
destroy_device_interface(struct wl_resource *resource)
{
	struct input_device *dev = wl_resource_get_user_data(resource);
	wl_list_remove(&dev->link);
	wl_list_remove(wl_resource_get_link(dev->resource));
	free(dev);
}

static void
get_devices(struct wl_client *client, struct wl_resource *resource)
{
	struct input_device_manager *input_device_manager = wl_resource_get_user_data(resource);
	struct weston_seat *seat = input_device_manager->seat;
        struct udev_seat *udev = container_of(seat, struct udev_seat, base);
        struct evdev_device *dev;

	/* FIXME: don't go through the udev seat here */
	wl_list_for_each(dev, &udev->devices_list, link) {
		struct input_device *input_dev = zalloc(sizeof(*input_dev));
		struct wl_resource *deviceid;
		deviceid = wl_resource_create(client,
					      &wl_input_device_interface, 1,
					      0);

		wl_resource_set_user_data(deviceid, input_dev);

		input_dev->resource = deviceid;
		input_dev->dev = dev;
		input_dev->manager = input_device_manager;
		wl_list_init(&input_dev->link);
		wl_list_insert(&seat->input_device_manager->input_device_list, wl_resource_get_link(deviceid));

		wl_resource_set_implementation(deviceid,
				&device_interface,
				input_dev,
				destroy_device_interface);

		wl_signal_init(&input_dev->destroy_signal);
		wl_signal_add(&input_dev->destroy_signal, &input_dev->destroy_listener);

		wl_input_device_manager_send_added(resource,
						   deviceid,
						   dev->devname,
						   /* FIXME: */
						   1, 1, "---phys--",
						   "--uniq--",
						   dev->caps
						   );
	}
	/* FIXME: no hooks for new devices yet */

}

const struct wl_input_device_manager_interface input_device_manager_interface = {
       get_devices,
};

static void
bind_input_device_manager(struct wl_client *client,
			  void *data,
			  uint32_t version,
			  uint32_t id)
{
	struct input_device_manager *input_device_manager = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_input_device_manager_interface, 1, id);
	wl_resource_set_implementation(resource, &input_device_manager_interface,
					input_device_manager, unbind_input_device_manager);
	input_device_manager->resource = resource;

}

static void
input_device_manager_destroy(struct wl_listener *listener, void *data)
{
	struct input_device_manager *input_device_manager =
		container_of(listener, struct input_device_manager, destroy_listener);

	free(input_device_manager);
}


static void
handle_seat_created(struct wl_listener *listener,
		    void *data)
{
	struct weston_seat *seat = data;
	struct weston_compositor *ec = seat->compositor;
	struct input_device_manager *input_device_manager;

	input_device_manager = zalloc(sizeof(*input_device_manager));
	input_device_manager->seat = seat;
	wl_list_init(&input_device_manager->input_device_list);

	input_device_manager->input_device_manager_global =
		wl_global_create(ec->wl_display, &wl_input_device_manager_interface, 1,
				 input_device_manager, bind_input_device_manager);

	input_device_manager->destroy_listener.notify = input_device_manager_destroy;
	wl_signal_add(&seat->destroy_signal, &input_device_manager->destroy_listener);

	seat->input_device_manager = input_device_manager;
}

static void
input_device_backend_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct input_device_backend *input_device_backend =
		container_of(listener, struct input_device_backend, destroy_listener);

	free(input_device_backend);
}

WL_EXPORT void
input_device_backend_init(struct weston_compositor *ec)
{
	struct input_device_backend *input_device_backend;

	input_device_backend = zalloc(sizeof(*input_device_backend));
	input_device_backend->compositor = ec;

	input_device_backend->seat_created_listener.notify = handle_seat_created;
	wl_signal_add(&ec->seat_created_signal,
		      &input_device_backend->seat_created_listener);

	input_device_backend->destroy_listener.notify = input_device_backend_notifier_destroy;
	wl_signal_add(&ec->destroy_signal, &input_device_backend->destroy_listener);
}
