/*
 * Copyright © 2013 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cairo.h>

#include <wayland-client.h>
#include "../shared/os-compatibility.h"
#include "input-device-client-protocol.h"
#include "../shared/zalloc.h"
#include "window.h"

struct input_client {
	struct display *display;
	struct widget *widget;
	struct wl_registry *registry;
	struct window *window;
	struct wl_input_device_manager *manager;
	void *data;
	int x, y, w, h;

	struct wl_list devices;
};

struct input_device {
	struct wl_list link;
	struct wl_resource *resource;
	struct wl_input_device *dev;
	struct input_client *input_client;
	char *name;
};

static const int width = 500;
static const int height = 400;
static const int width_max = 0;
static const int height_max = 0;

static void handle_axis_cap(void *data,
			    struct wl_input_device *device,
			    uint32_t axis,
			    int32_t min,
			    int32_t max,
			    uint32_t resolution,
			    uint32_t fuzz,
			    uint32_t flat)
{
	struct input_device *dev = data;
	printf("%s: axis %#x [%d..%d]\n", dev->name, axis, min, max);
}

static void axis_event(void *data,
		       struct wl_input_device *device,
		       uint32_t time,
		       uint32_t axis,
		       wl_fixed_t value)
{
	struct input_device *dev = data;
	printf(":::: event %s %x %x %d\n", dev->name, (axis & 0xFF00) >> 8, axis & 0xFF, value);
}

static void frame(void *data,
		  struct wl_input_device *wl_input_device,
		  uint32_t time)
{
	struct input_device *dev = data;
	printf(":::: frame %s\n", dev->name);
}


struct wl_input_device_listener device_interface_listener = {
	handle_axis_cap,
	axis_event,
	frame
};

static void device_added(void *data,
			 struct wl_input_device_manager *wl_input_device_manager,
			 struct wl_input_device *device,
			 const char *name,
			 uint32_t vid,
			 uint32_t pid,
			 const char *phys,
			 const char *uniq,
			 uint32_t capabilities)
{
	struct input_client *input_client = data;
	struct input_device *dev;

	printf("device added: %s\n", name);
	printf("	Vendor: %#x Product: %x\n", vid, pid);
	if (phys)
		printf("	Phys %s\n", phys);
	if (uniq)
		printf("	Uniq %s\n", uniq);


	dev = zalloc(sizeof(*dev));
	dev->input_client = input_client;
	dev->dev = device;
	dev->name = strdup(name); /* FIXME: leaks */
	wl_input_device_add_listener(dev->dev,
				     &device_interface_listener,
				     dev);
	wl_input_device_get_axes(device);
	wl_display_roundtrip(display_get_display(input_client->display));

	wl_list_insert(&input_client->devices, &dev->link);
}

static void device_removed(void *data,
			 struct wl_input_device_manager *wl_input_device_manager,
			 struct wl_input_device *device)
{

	printf("device removed\n");

}

static const struct wl_input_device_manager_listener input_device_manager_listener = {
	device_added,
	device_removed
};

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t name, const char *interface, uint32_t version)
{
	struct input_client *input_client = data;

	printf("global interface: %s\n", interface);

	if (strcmp(interface, "wl_input_device_manager") == 0) {
		input_client->manager = wl_registry_bind(registry, name,
					       &wl_input_device_manager_interface, 1);
		wl_input_device_manager_add_listener(input_client->manager,
						     &input_device_manager_listener,
						     input_client);
		wl_input_device_manager_get_devices(input_client->manager);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	handle_global,
	handle_global_remove
};

static int
motion_handler(struct widget *widget, struct input *input, uint32_t time,
	       float x, float y, void *data)
{
	printf("motion time: %d, x: %f, y: %f\n", time, x, y);

	return CURSOR_LEFT_PTR;
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct input_client *e = data;
	cairo_surface_t *surface;
	cairo_t *cr;
	struct rectangle rect;

	printf("redraw\n");

	widget_get_allocation(e->widget, &rect);
	surface = window_get_surface(e->window);

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

	cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
	cairo_fill(cr);

	cairo_rectangle(cr, e->x, e->y, e->w, e->h);
	cairo_set_source_rgba(cr, 1.0, 0, 0, 1);
	cairo_fill(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct input_client *e = data;
	printf("resize width: %d, height: %d\n", width, height);

	/* if a maximum width is set, constrain to it */
	if (width_max && width_max < width)
		width = width_max;

	/* if a maximum height is set, constrain to it */
	if (height_max && height_max < height)
		height = height_max;

	/* set the new window dimensions */
	widget_set_size(e->widget, width, height);
}


static int
enter_handler(struct widget *widget, struct input *input, float x, float y, void *data)
{
	printf("enter\n");

	return CURSOR_LEFT_PTR;
}

static void
leave_handler(struct widget *widget, struct input *input, void *data)
{
	printf("leave\n");
}

static void
button_handler(struct widget *widget,
	       struct input *input, uint32_t time,
	       uint32_t button,
	       enum wl_pointer_button_state state,
	       void *data)
{
	printf("button\n");
}

static struct input_client*
input_client_create(struct display *d)
{
	struct input_client *input_client;
	struct wl_display *display;

	input_client = malloc(sizeof *input_client);
	input_client->display = d;
	input_client->x = width * 1.0 / 4.0;
	input_client->w = width * 2.0 / 4.0;
	input_client->y = height * 1.0 / 4.0;
	input_client->h = height * 2.0 / 4.0;
	wl_list_init(&input_client->devices);

	input_client->window = window_create(d);
	window_set_user_data(input_client->window, input_client);
	window_set_title(input_client->window, "Input Extension demo");
	input_client->display = d;
	input_client->widget = frame_create(input_client->window, input_client);
	widget_set_motion_handler(input_client->widget, motion_handler);
	widget_set_redraw_handler(input_client->widget, redraw_handler);
	widget_set_resize_handler(input_client->widget, resize_handler);
	widget_set_enter_handler(input_client->widget, enter_handler);
	widget_set_leave_handler(input_client->widget, leave_handler);
	widget_set_button_handler(input_client->widget, button_handler);
	window_schedule_resize(input_client->window, width, height);

	display = display_get_display(d);
	input_client->registry = wl_display_get_registry(display);
	wl_registry_add_listener(input_client->registry, &registry_listener, input_client);
	wl_display_roundtrip(display);

	return input_client;
}

int
main(int argc, char **argv)
{
	struct input_client *input_client;
	struct display *d;

	d = display_create(&argc, argv);
	input_client = input_client_create(d);

	display_run(d);
	display_destroy(d);

	return 0;
}
