/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2013 Jonas Ådahl
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <mtdev.h>
#include <assert.h>
#include <libinput.h>

#include "compositor.h"
#include "libinput-device.h"
#include "shared/helpers.h"

struct tablet_output_listener {
	struct wl_listener base;
	struct wl_list tablet_list;
};

static bool
tablet_bind_output(struct weston_tablet *tablet, struct weston_output *output);

void
evdev_led_update(struct evdev_device *device, enum weston_led weston_leds)
{
	enum libinput_led leds = 0;

	if (weston_leds & LED_NUM_LOCK)
		leds |= LIBINPUT_LED_NUM_LOCK;
	if (weston_leds & LED_CAPS_LOCK)
		leds |= LIBINPUT_LED_CAPS_LOCK;
	if (weston_leds & LED_SCROLL_LOCK)
		leds |= LIBINPUT_LED_SCROLL_LOCK;

	libinput_device_led_update(device->device, leds);
}

static void
handle_keyboard_key(struct libinput_device *libinput_device,
		    struct libinput_event_keyboard *keyboard_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	int key_state =
		libinput_event_keyboard_get_key_state(keyboard_event);
	int seat_key_count =
		libinput_event_keyboard_get_seat_key_count(keyboard_event);

	/* Ignore key events that are not seat wide state changes. */
	if ((key_state == LIBINPUT_KEY_STATE_PRESSED &&
	     seat_key_count != 1) ||
	    (key_state == LIBINPUT_KEY_STATE_RELEASED &&
	     seat_key_count != 0))
		return;

	notify_key(device->seat,
		   libinput_event_keyboard_get_time(keyboard_event),
		   libinput_event_keyboard_get_key(keyboard_event),
		   libinput_event_keyboard_get_key_state(keyboard_event),
		   STATE_UPDATE_AUTOMATIC);
}

static void
handle_pointer_motion(struct libinput_device *libinput_device,
		      struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	wl_fixed_t dx, dy;

	dx = wl_fixed_from_double(libinput_event_pointer_get_dx(pointer_event));
	dy = wl_fixed_from_double(libinput_event_pointer_get_dy(pointer_event));
	notify_motion(device->seat,
		      libinput_event_pointer_get_time(pointer_event),
		      dx,
		      dy);
}

static void
handle_pointer_motion_absolute(
	struct libinput_device *libinput_device,
	struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	struct weston_output *output = device->output;
	uint32_t time;
	wl_fixed_t x, y;
	uint32_t width, height;

	if (!output)
		return;

	time = libinput_event_pointer_get_time(pointer_event);
	width = device->output->current_mode->width;
	height = device->output->current_mode->height;

	x = wl_fixed_from_double(
		libinput_event_pointer_get_absolute_x_transformed(pointer_event,
								  width));
	y = wl_fixed_from_double(
		libinput_event_pointer_get_absolute_y_transformed(pointer_event,
								  height));

	weston_output_transform_coordinate(device->output, x, y, &x, &y);
	notify_motion_absolute(device->seat, time, x, y);
}

static void
handle_pointer_button(struct libinput_device *libinput_device,
		      struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	int button_state =
		libinput_event_pointer_get_button_state(pointer_event);
	int seat_button_count =
		libinput_event_pointer_get_seat_button_count(pointer_event);

	/* Ignore button events that are not seat wide state changes. */
	if ((button_state == LIBINPUT_BUTTON_STATE_PRESSED &&
	     seat_button_count != 1) ||
	    (button_state == LIBINPUT_BUTTON_STATE_RELEASED &&
	     seat_button_count != 0))
		return;

	notify_button(device->seat,
		      libinput_event_pointer_get_time(pointer_event),
		      libinput_event_pointer_get_button(pointer_event),
		      libinput_event_pointer_get_button_state(pointer_event));
}

static double
normalize_scroll(struct libinput_event_pointer *pointer_event,
		 enum libinput_pointer_axis axis)
{
	static int warned;
	enum libinput_pointer_axis_source source;
	double value;

	source = libinput_event_pointer_get_axis_source(pointer_event);
	/* libinput < 0.8 sent wheel click events with value 10. Since 0.8
	   the value is the angle of the click in degrees. To keep
	   backwards-compat with existing clients, we just send multiples of
	   the click count.
	 */
	switch (source) {
	case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL:
		value = 10 * libinput_event_pointer_get_axis_value_discrete(
								   pointer_event,
								   axis);
		break;
	case LIBINPUT_POINTER_AXIS_SOURCE_FINGER:
	case LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS:
		value = libinput_event_pointer_get_axis_value(pointer_event,
							      axis);
		break;
	default:
		value = 0;
		if (warned < 5) {
			weston_log("Unknown scroll source %d. Event discarded\n",
				   source);
			warned++;
		}
		break;
	}

	return value;
}

static void
handle_pointer_axis(struct libinput_device *libinput_device,
		    struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	double value;
	enum libinput_pointer_axis axis;

	axis = LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
	if (libinput_event_pointer_has_axis(pointer_event, axis)) {
		value = normalize_scroll(pointer_event, axis);
		notify_axis(device->seat,
			    libinput_event_pointer_get_time(pointer_event),
			    WL_POINTER_AXIS_VERTICAL_SCROLL,
			    wl_fixed_from_double(value));
	}

	axis = LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL;
	if (libinput_event_pointer_has_axis(pointer_event, axis)) {
		value = normalize_scroll(pointer_event, axis);
		notify_axis(device->seat,
			    libinput_event_pointer_get_time(pointer_event),
			    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			    wl_fixed_from_double(value));
	}
}

static void
handle_touch_with_coords(struct libinput_device *libinput_device,
			 struct libinput_event_touch *touch_event,
			 int touch_type)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	wl_fixed_t x;
	wl_fixed_t y;
	uint32_t width, height;
	uint32_t time;
	int32_t slot;

	if (!device->output)
		return;

	time = libinput_event_touch_get_time(touch_event);
	slot = libinput_event_touch_get_seat_slot(touch_event);

	width = device->output->current_mode->width;
	height = device->output->current_mode->height;
	x = wl_fixed_from_double(
		libinput_event_touch_get_x_transformed(touch_event, width));
	y = wl_fixed_from_double(
		libinput_event_touch_get_y_transformed(touch_event, height));

	weston_output_transform_coordinate(device->output,
					   x, y, &x, &y);

	notify_touch(device->seat, time, slot, x, y, touch_type);
}

static void
handle_touch_down(struct libinput_device *device,
		  struct libinput_event_touch *touch_event)
{
	handle_touch_with_coords(device, touch_event, WL_TOUCH_DOWN);
}

static void
handle_touch_motion(struct libinput_device *device,
		    struct libinput_event_touch *touch_event)
{
	handle_touch_with_coords(device, touch_event, WL_TOUCH_MOTION);
}

static void
handle_touch_up(struct libinput_device *libinput_device,
		struct libinput_event_touch *touch_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	uint32_t time = libinput_event_touch_get_time(touch_event);
	int32_t slot = libinput_event_touch_get_seat_slot(touch_event);

	notify_touch(device->seat, time, slot, 0, 0, WL_TOUCH_UP);
}

static void
handle_touch_frame(struct libinput_device *libinput_device,
		   struct libinput_event_touch *touch_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	struct weston_seat *seat = device->seat;

	notify_touch_frame(seat);
}

static void
handle_tablet_proximity(struct libinput_device *libinput_device,
			struct libinput_event_tablet *proximity_event)
{
	struct evdev_device *device;
	struct weston_tablet *tablet;
	struct weston_tablet_tool *tool;
	struct libinput_tool *libinput_tool;
	enum libinput_tool_type libinput_tool_type;
	uint32_t serial, type;
	uint32_t time;
	bool create = true;

	device = libinput_device_get_user_data(libinput_device);
	time = libinput_event_tablet_get_time(proximity_event);
	libinput_tool = libinput_event_tablet_get_tool(proximity_event);
	serial = libinput_tool_get_serial(libinput_tool);
	libinput_tool_type = libinput_tool_get_type(libinput_tool);

	tool = libinput_tool_get_user_data(libinput_tool);
	tablet = device->tablet;

	if (libinput_event_tablet_get_proximity_state(proximity_event) ==
	    LIBINPUT_TOOL_PROXIMITY_OUT) {
		notify_tablet_tool_proximity_out(tool, time);
		return;
	}

	switch (libinput_tool_type) {
	case LIBINPUT_TOOL_PEN:
		type = ZWP_TABLET_TOOL1_TYPE_PEN;
		break;
	case LIBINPUT_TOOL_ERASER:
		type = ZWP_TABLET_TOOL1_TYPE_ERASER;
		break;
	default:
		fprintf(stderr, "Unknown libinput tool type %d\n",
			libinput_tool_type);
		return;
	}

	wl_list_for_each(tool, &device->seat->tablet_tool_list, link) {
		if (tool->serial == serial && tool->type == type) {
			create = false;
			break;
		}
	}

	if (create) {
		tool = weston_seat_add_tablet_tool(device->seat);
		tool->serial = serial;
		tool->hwid = libinput_tool_get_tool_id(libinput_tool);
		tool->type = type;
		tool->capabilities = 0;

		if (libinput_tool_has_axis(libinput_tool,
					   LIBINPUT_TABLET_AXIS_DISTANCE))
		    tool->capabilities |= 1 << ZWP_TABLET_TOOL1_CAPABILITY_DISTANCE;
		if (libinput_tool_has_axis(libinput_tool,
					   LIBINPUT_TABLET_AXIS_PRESSURE))
		    tool->capabilities |= 1 << ZWP_TABLET_TOOL1_CAPABILITY_PRESSURE;
		if (libinput_tool_has_axis(libinput_tool,
					   LIBINPUT_TABLET_AXIS_TILT_X) &&
		    libinput_tool_has_axis(libinput_tool,
					   LIBINPUT_TABLET_AXIS_TILT_Y))
		    tool->capabilities |= 1 << ZWP_TABLET_TOOL1_CAPABILITY_TILT;

		wl_list_insert(&device->seat->tablet_tool_list, &tool->link);
		notify_tablet_tool_added(tool);

		libinput_tool_set_user_data(libinput_tool, tool);
	}

	notify_tablet_tool_proximity_in(tool, time, tablet);
	/* FIXME: we should send axis updates  here */
	notify_tablet_tool_frame(tool, time);
}

static void
handle_tablet_axis(struct libinput_device *libinput_device,
		   struct libinput_event_tablet *axis_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	struct weston_tablet_tool *tool;
	struct weston_tablet *tablet = device->tablet;
	struct libinput_tool *libinput_tool;
	uint32_t time;
	const int NORMALIZED_AXIS_MAX = 65535;

	libinput_tool = libinput_event_tablet_get_tool(axis_event);
	tool = libinput_tool_get_user_data(libinput_tool);
	time = libinput_event_tablet_get_time(axis_event);

	if (libinput_event_tablet_axis_has_changed(axis_event,
						   LIBINPUT_TABLET_AXIS_X) ||
	    libinput_event_tablet_axis_has_changed(axis_event,
						   LIBINPUT_TABLET_AXIS_Y)) {
		double x, y;
		uint32_t width, height;

		width = tablet->output->current_mode->width;
		height = tablet->output->current_mode->height;
		x = libinput_event_tablet_get_x_transformed(axis_event, width);
		y = libinput_event_tablet_get_y_transformed(axis_event, height);

		notify_tablet_tool_motion(tool, time,
					  wl_fixed_from_double(x),
					  wl_fixed_from_double(y));
	}

	if (libinput_event_tablet_axis_has_changed(axis_event,
						   LIBINPUT_TABLET_AXIS_PRESSURE)) {
		double pressure;

		pressure = libinput_event_tablet_get_axis_value(axis_event,
						LIBINPUT_TABLET_AXIS_PRESSURE);
		/* convert axis range [0.0, 1.0] to [0, 65535] */
		pressure *= NORMALIZED_AXIS_MAX;
		notify_tablet_tool_pressure(tool, time, pressure);
	}

	if (libinput_event_tablet_axis_has_changed(axis_event,
						   LIBINPUT_TABLET_AXIS_DISTANCE)) {
		double distance;

		distance = libinput_event_tablet_get_axis_value(axis_event,
						LIBINPUT_TABLET_AXIS_PRESSURE);
		/* convert axis range [0.0, 1.0] to [0, 65535] */
		distance *= NORMALIZED_AXIS_MAX;
		notify_tablet_tool_distance(tool, time, distance);
	}

	if (libinput_event_tablet_axis_has_changed(axis_event,
						   LIBINPUT_TABLET_AXIS_TILT_X) ||
	    libinput_event_tablet_axis_has_changed(axis_event,
						   LIBINPUT_TABLET_AXIS_TILT_Y)) {
		double tx, ty;

		tx = libinput_event_tablet_get_axis_value(axis_event,
						LIBINPUT_TABLET_AXIS_TILT_X);
		ty = libinput_event_tablet_get_axis_value(axis_event,
						LIBINPUT_TABLET_AXIS_TILT_Y);
		/* convert axis range [-1.0, 1.0] to [-65535, 65535] */
		tx *= NORMALIZED_AXIS_MAX;
		ty *= NORMALIZED_AXIS_MAX;
		notify_tablet_tool_tilt(tool, time, tx, ty);
	}

	notify_tablet_tool_frame(tool, time);
}

static void
handle_tablet_button(struct libinput_device *libinput_device,
		     struct libinput_event_tablet *button_event)
{
	struct weston_tablet_tool *tool;
	struct libinput_tool *libinput_tool;
	uint32_t time, button;
	enum zwp_tablet_tool1_button_state state;

	libinput_tool = libinput_event_tablet_get_tool(button_event);
	tool = libinput_tool_get_user_data(libinput_tool);
	time = libinput_event_tablet_get_time(button_event);
	button = libinput_event_tablet_get_button(button_event);
	if (libinput_event_tablet_get_button_state(button_event) ==
	    LIBINPUT_BUTTON_STATE_PRESSED)
		state = ZWP_TABLET_TOOL1_BUTTON_STATE_PRESSED;
	else
		state = ZWP_TABLET_TOOL1_BUTTON_STATE_RELEASED;

	if (button == BTN_TOUCH) {
		if (state == ZWP_TABLET_TOOL1_BUTTON_STATE_PRESSED)
			notify_tablet_tool_down(tool, time);
		else
			notify_tablet_tool_up(tool, time);

	} else {
		notify_tablet_tool_button(tool, time, button, state);
	}
}

int
evdev_device_process_event(struct libinput_event *event)
{
	struct libinput_device *libinput_device =
		libinput_event_get_device(event);
	int handled = 1;

	switch (libinput_event_get_type(event)) {
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		handle_keyboard_key(libinput_device,
				    libinput_event_get_keyboard_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_MOTION:
		handle_pointer_motion(libinput_device,
				      libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
		handle_pointer_motion_absolute(
			libinput_device,
			libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_BUTTON:
		handle_pointer_button(libinput_device,
				      libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_AXIS:
		handle_pointer_axis(libinput_device,
				    libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_DOWN:
		handle_touch_down(libinput_device,
				  libinput_event_get_touch_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_MOTION:
		handle_touch_motion(libinput_device,
				    libinput_event_get_touch_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_UP:
		handle_touch_up(libinput_device,
				libinput_event_get_touch_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_FRAME:
		handle_touch_frame(libinput_device,
				   libinput_event_get_touch_event(event));
		break;
	case LIBINPUT_EVENT_TABLET_PROXIMITY:
		handle_tablet_proximity(libinput_device,
					libinput_event_get_tablet_event(event));
		break;
	case LIBINPUT_EVENT_TABLET_AXIS:
		handle_tablet_axis(libinput_device,
				   libinput_event_get_tablet_event(event));
		break;
	case LIBINPUT_EVENT_TABLET_BUTTON:
		handle_tablet_button(libinput_device,
				     libinput_event_get_tablet_event(event));
		break;
	default:
		handled = 0;
		weston_log("unknown libinput event %d\n",
			   libinput_event_get_type(event));
	}

	return handled;
}

static void
notify_output_destroy(struct wl_listener *listener, void *data)
{
	struct evdev_device *device =
		container_of(listener,
			     struct evdev_device, output_destroy_listener);
	struct weston_compositor *c = device->seat->compositor;
	struct weston_output *output;

	if (!device->output_name && !wl_list_empty(&c->output_list)) {
		output = container_of(c->output_list.next,
				      struct weston_output, link);
		evdev_device_set_output(device, output);
	} else {
		device->output = NULL;
	}
}

/**
 * The WL_CALIBRATION property requires a pixel-specific matrix to be
 * applied after scaling device coordinates to screen coordinates. libinput
 * can't do that, so we need to convert the calibration to the normalized
 * format libinput expects.
 */
static void
evdev_device_set_calibration(struct evdev_device *device)
{
	struct udev *udev;
	struct udev_device *udev_device = NULL;
	const char *sysname = libinput_device_get_sysname(device->device);
	const char *calibration_values;
	uint32_t width, height;
	float calibration[6];
	enum libinput_config_status status;

	if (!device->output)
		return;

	width = device->output->width;
	height = device->output->height;
	if (width == 0 || height == 0)
		return;

	/* If libinput has a pre-set calibration matrix, don't override it */
	if (!libinput_device_config_calibration_has_matrix(device->device) ||
	    libinput_device_config_calibration_get_default_matrix(
							  device->device,
							  calibration) != 0)
		return;

	udev = udev_new();
	if (!udev)
		return;

	udev_device = udev_device_new_from_subsystem_sysname(udev,
							     "input",
							     sysname);
	if (!udev_device)
		goto out;

	calibration_values =
		udev_device_get_property_value(udev_device,
					       "WL_CALIBRATION");

	if (!calibration_values || sscanf(calibration_values,
					  "%f %f %f %f %f %f",
					  &calibration[0],
					  &calibration[1],
					  &calibration[2],
					  &calibration[3],
					  &calibration[4],
					  &calibration[5]) != 6)
		goto out;

	weston_log("Applying calibration: %f %f %f %f %f %f "
		   "(normalized %f %f)\n",
		    calibration[0],
		    calibration[1],
		    calibration[2],
		    calibration[3],
		    calibration[4],
		    calibration[5],
		    calibration[2] / width,
		    calibration[5] / height);

	/* normalize to a format libinput can use. There is a chance of
	   this being wrong if the width/height don't match the device
	   width/height but I'm not sure how to fix that */
	calibration[2] /= width;
	calibration[5] /= height;

	status = libinput_device_config_calibration_set_matrix(device->device,
							       calibration);
	if (status != LIBINPUT_CONFIG_STATUS_SUCCESS)
		weston_log("Failed to apply calibration.\n");

out:
	if (udev_device)
		udev_device_unref(udev_device);
	udev_unref(udev);
}

void
evdev_device_set_output(struct evdev_device *device,
			struct weston_output *output)
{
	if (device->output_destroy_listener.notify) {
		wl_list_remove(&device->output_destroy_listener.link);
		device->output_destroy_listener.notify = NULL;
	}

	device->output = output;
	device->output_destroy_listener.notify = notify_output_destroy;
	wl_signal_add(&output->destroy_signal,
		      &device->output_destroy_listener);
	evdev_device_set_calibration(device);
}

static void
configure_device(struct evdev_device *device)
{
	struct weston_compositor *compositor = device->seat->compositor;
	struct weston_config_section *s;
	int enable_tap;
	int enable_tap_default;

	s = weston_config_get_section(compositor->config,
				      "libinput", NULL, NULL);

	if (libinput_device_config_tap_get_finger_count(device->device) > 0) {
		enable_tap_default =
			libinput_device_config_tap_get_default_enabled(
				device->device);
		weston_config_section_get_bool(s, "enable_tap",
					       &enable_tap,
					       enable_tap_default);
		libinput_device_config_tap_set_enabled(device->device,
						       enable_tap);
	}

	evdev_device_set_calibration(device);
}

static void
bind_unbound_tablets(struct wl_listener *listener_base, void *data)
{
	struct tablet_output_listener *listener =
		wl_container_of(listener_base, listener, base);
	struct weston_tablet *tablet, *tmp;

	wl_list_for_each_safe(tablet, tmp, &listener->tablet_list, link) {
		if (tablet_bind_output(tablet, data)) {
			wl_list_remove(&tablet->link);
			wl_list_insert(&tablet->seat->tablet_list,
				       &tablet->link);
			tablet->device->seat_caps |= EVDEV_SEAT_TABLET;
			notify_tablet_added(tablet);
		}
	}

	if (wl_list_empty(&listener->tablet_list)) {
		wl_list_remove(&listener_base->link);
		free(listener);
	}
}

static bool
tablet_bind_output(struct weston_tablet *tablet, struct weston_output *output)
{
	struct wl_list *output_list = &tablet->seat->compositor->output_list;
	struct weston_compositor *compositor = tablet->seat->compositor;
	struct tablet_output_listener *listener;
	struct wl_listener *listener_base;

	/* TODO: Properly bind tablets with built-in displays */
	switch (tablet->type) {
		case ZWP_TABLET1_TYPE_EXTERNAL:
		case ZWP_TABLET1_TYPE_INTERNAL:
		case ZWP_TABLET1_TYPE_DISPLAY:
			if (output) {
				tablet->output = output;
			} else {
				if (wl_list_empty(output_list))
					break;

				/* Find the first available display */
				wl_list_for_each(output, output_list, link)
					break;
				tablet->output = output;
			}
		break;
	}

	if (tablet->output)
		return true;

	listener_base = wl_signal_get(&compositor->output_created_signal,
				      bind_unbound_tablets);
	if (listener_base == NULL) {
		listener = zalloc(sizeof(*listener));

		wl_list_init(&listener->tablet_list);

		listener_base = &listener->base;
		listener_base->notify = bind_unbound_tablets;

		wl_signal_add(&compositor->output_created_signal,
			      listener_base);
	} else {
		listener = wl_container_of(listener_base, listener, base);
	}

	wl_list_insert(&listener->tablet_list, &tablet->link);
	return false;
}

static void
evdev_device_init_tablet(struct evdev_device *device,
			 struct libinput_device *libinput_device,
			 struct weston_seat *seat)
{
	struct weston_tablet *tablet;
	struct udev_device *udev_device;

	tablet = weston_seat_add_tablet(seat);
	tablet->name = strdup(libinput_device_get_name(libinput_device));
	tablet->vid = libinput_device_get_id_vendor(libinput_device);
	tablet->pid = libinput_device_get_id_product(libinput_device);

	/* FIXME: we need libwacom to get this information */
	tablet->type = ZWP_TABLET1_TYPE_EXTERNAL;

	udev_device = libinput_device_get_udev_device(libinput_device);
	if (udev_device) {
		tablet->path = udev_device_get_devnode(udev_device);
		udev_device_unref(udev_device);
	}

	/* If we can successfully bind the tablet to an output, then
	 * it's ready to get added to the seat's tablet list, otherwise
	 * it will get added when an appropriate output is available */
	if (tablet_bind_output(tablet, NULL)) {
		wl_list_insert(&seat->tablet_list, &tablet->link);
		device->seat_caps |= EVDEV_SEAT_TABLET;

		notify_tablet_added(tablet);
	}

	device->tablet = tablet;
	tablet->device = device;
}

struct evdev_device *
evdev_device_create(struct libinput_device *libinput_device,
		    struct weston_seat *seat)
{
	struct evdev_device *device;

	device = zalloc(sizeof *device);
	if (device == NULL)
		return NULL;

	device->seat = seat;
	wl_list_init(&device->link);
	device->device = libinput_device;

	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_KEYBOARD)) {
		weston_seat_init_keyboard(seat, NULL);
		device->seat_caps |= EVDEV_SEAT_KEYBOARD;
	}
	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_POINTER)) {
		weston_seat_init_pointer(seat);
		device->seat_caps |= EVDEV_SEAT_POINTER;
	}
	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_TOUCH)) {
		weston_seat_init_touch(seat);
		device->seat_caps |= EVDEV_SEAT_TOUCH;
	}
	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_TABLET)) {
		evdev_device_init_tablet(device, libinput_device, seat);
	}

	libinput_device_set_user_data(libinput_device, device);
	libinput_device_ref(libinput_device);

	configure_device(device);

	return device;
}

void
evdev_device_destroy(struct evdev_device *device)
{
	if (device->seat_caps & EVDEV_SEAT_POINTER)
		weston_seat_release_pointer(device->seat);
	if (device->seat_caps & EVDEV_SEAT_KEYBOARD)
		weston_seat_release_keyboard(device->seat);
	if (device->seat_caps & EVDEV_SEAT_TOUCH)
		weston_seat_release_touch(device->seat);
	if (device->seat_caps & EVDEV_SEAT_TABLET)
		weston_seat_release_tablet(device->tablet);

	if (device->output)
		wl_list_remove(&device->output_destroy_listener.link);
	wl_list_remove(&device->link);
	libinput_device_unref(device->device);
	free(device->devnode);
	free(device->output_name);
	free(device);
}

void
evdev_notify_keyboard_focus(struct weston_seat *seat,
			    struct wl_list *evdev_devices)
{
	struct wl_array keys;

	if (seat->keyboard_device_count == 0)
		return;

	wl_array_init(&keys);
	notify_keyboard_focus_in(seat, &keys, STATE_UPDATE_AUTOMATIC);
	wl_array_release(&keys);
}
