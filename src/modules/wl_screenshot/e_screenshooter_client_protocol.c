#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

extern const struct wl_interface wl_output_interface;
extern const struct wl_interface wl_buffer_interface;

static const struct wl_interface *types[] = {
	&wl_output_interface,
	&wl_buffer_interface,
};

static const struct wl_message screenshooter_requests[] = {
	{ "shoot", "oo", types + 0 },
};

static const struct wl_message screenshooter_events[] = 
{
	{ "done", "", types + 0 },
};

WL_EXPORT const struct wl_interface screenshooter_interface = {
	"screenshooter", 1,
	1, screenshooter_requests,
	1, screenshooter_events,
};

