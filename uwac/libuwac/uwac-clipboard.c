/*
 * Copyright © 2018 Armin Novak <armin.novak@thincast.com>
 * Copyright © 2018 Thincast Technologies GmbH
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
#include "uwac-priv.h"
#include "uwac-utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>

/* paste */
static void data_offer_offer(void* data, struct wl_data_offer* data_offer,
                             const char* offered_mime_type)
{
	UwacSeat* seat = (UwacSeat*)data;

	assert(seat);
	if (!seat->ignore_announcement)
	{
		UwacClipboardEvent* event =
		    (UwacClipboardEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_CLIPBOARD_OFFER);

		if (!event)
		{
			assert(uwacErrorHandler(seat->display, UWAC_ERROR_INTERNAL,
			                        "failed to allocate a clipboard event\n"));
		}
		else
		{
			event->seat = seat;
			(void)snprintf(event->mime, sizeof(event->mime), "%s", offered_mime_type);
		}
	}
}

static const struct wl_data_offer_listener data_offer_listener = { .offer = data_offer_offer };

static void data_device_data_offer(void* data, struct wl_data_device* data_device,
                                   struct wl_data_offer* data_offer)
{
	UwacSeat* seat = (UwacSeat*)data;

	assert(seat);
	fprintf(stderr, "[data_device_data_offer] New data offer: %p, ignore_announcement=%d\n",
	        data_offer, seat->ignore_announcement);

	if (!seat->ignore_announcement)
	{
		UwacClipboardEvent* event =
		    (UwacClipboardEvent*)UwacDisplayNewEvent(seat->display, UWAC_EVENT_CLIPBOARD_SELECT);

		if (!event)
		{
			assert(uwacErrorHandler(seat->display, UWAC_ERROR_INTERNAL,
			                        "failed to allocate a close event\n"));
		}
		else
			event->seat = seat;

		wl_data_offer_add_listener(data_offer, &data_offer_listener, data);
		seat->offer = data_offer;
		fprintf(stderr, "[data_device_data_offer] Set seat->offer to %p\n", data_offer);
	}
	else
	{
		seat->offer = nullptr;
		fprintf(stderr, "[data_device_data_offer] Cleared seat->offer (ignore_announcement=true)\n");
	}
}

static void data_device_selection(void* data, struct wl_data_device* data_device,
                                  struct wl_data_offer* data_offer)
{
	UwacSeat* seat = (UwacSeat*)data;

	assert(seat);
	if (seat->ignore_announcement)
		return;

	if (data_offer)
	{
		UwacClipboardEvent* event = (UwacClipboardEvent*)UwacDisplayNewEvent(
		    seat->display, UWAC_EVENT_CLIPBOARD_OFFERS_DONE);

		if (!event)
		{
			assert(uwacErrorHandler(seat->display, UWAC_ERROR_INTERNAL,
			                        "failed to allocate a clipboard event\n"));
		}
		else
			event->seat = seat;
	}
}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer, .selection = data_device_selection
};

/* copy */
static void data_source_target_handler(void* data, struct wl_data_source* data_source,
                                       const char* mime_type)
{
}

static void data_source_send_handler(void* data, struct wl_data_source* data_source,
                                     const char* mime_type, int fd)
{
	UwacSeat* seat = (UwacSeat*)data;
	seat->transfer_data(seat, seat->data_context, mime_type, fd);
}

static void data_source_cancelled_handler(void* data, struct wl_data_source* data_source)
{
	UwacSeat* seat = (UwacSeat*)data;
	seat->cancel_data(seat, seat->data_context);
}

static const struct wl_data_source_listener data_source_listener = {
	.target = data_source_target_handler,
	.send = data_source_send_handler,
	.cancelled = data_source_cancelled_handler
};

static void UwacRegisterDeviceListener(UwacSeat* s)
{
	wl_data_device_add_listener(s->data_device, &data_device_listener, s);
}

static UwacReturnCode UwacCreateDataSource(UwacSeat* s)
{
	if (!s)
		return UWAC_ERROR_INTERNAL;

	s->data_source = wl_data_device_manager_create_data_source(s->display->data_device_manager);
	wl_data_source_add_listener(s->data_source, &data_source_listener, s);
	return UWAC_SUCCESS;
}

UwacReturnCode UwacSeatRegisterClipboard(UwacSeat* s)
{
	UwacClipboardEvent* event = nullptr;

	if (!s)
		return UWAC_ERROR_INTERNAL;

	if (!s->display->data_device_manager || !s->data_device)
		return UWAC_NOT_ENOUGH_RESOURCES;

	UwacRegisterDeviceListener(s);

	UwacReturnCode rc = UwacCreateDataSource(s);

	if (rc != UWAC_SUCCESS)
		return rc;
	event = (UwacClipboardEvent*)UwacDisplayNewEvent(s->display, UWAC_EVENT_CLIPBOARD_AVAILABLE);

	if (!event)
	{
		assert(uwacErrorHandler(s->display, UWAC_ERROR_INTERNAL,
		                        "failed to allocate a clipboard event\n"));
		return UWAC_ERROR_INTERNAL;
	}

	event->seat = s;
	return UWAC_SUCCESS;
}

UwacReturnCode UwacClipboardOfferDestroy(UwacSeat* seat)
{
	if (!seat)
		return UWAC_ERROR_INTERNAL;

	if (seat->data_source)
		wl_data_source_destroy(seat->data_source);

	return UwacCreateDataSource(seat);
}

UwacReturnCode UwacClipboardOfferCreate(UwacSeat* seat, const char* mime)
{
	if (!seat || !mime)
		return UWAC_ERROR_INTERNAL;

	wl_data_source_offer(seat->data_source, mime);
	return UWAC_SUCCESS;
}

static void callback_done(void* data, struct wl_callback* callback, uint32_t serial)
{
	*(uint32_t*)data = serial;
}

static const struct wl_callback_listener callback_listener = { .done = callback_done };

/*
 * Clipboard requests are issued from the cliprdr channel worker thread, while the
 * main thread drives the Wayland default event queue via UwacDisplayDispatch().
 * Dispatching the default queue from two threads at once makes libwayland abort
 * (e.g. "wl_display_dispatch_queue: Assertion `ret == -1 || ret > 0' failed").
 * To stay thread-safe we route every synchronous clipboard wait through a private
 * event queue, which libwayland serialises against the main thread's reader using
 * its prepare_read/read_events protocol. The default queue is then only ever
 * dispatched by the main thread.
 */
static uint32_t get_serial(UwacSeat* s)
{
	struct wl_display* display = s->display->display;
	uint32_t serial = 0;

	fprintf(stderr, "[get_serial] Getting serial number...\n");

	struct wl_event_queue* queue = wl_display_create_queue(display);
	if (!queue)
	{
		fprintf(stderr, "[get_serial] ERROR: Failed to create private queue\n");
		return 0;
	}

	/* Wrap the display so the sync callback is delivered to our private queue. */
	struct wl_display* wrapped = wl_proxy_create_wrapper(display);
	if (!wrapped)
	{
		fprintf(stderr, "[get_serial] ERROR: Failed to create proxy wrapper\n");
		wl_event_queue_destroy(queue);
		return 0;
	}
	wl_proxy_set_queue((struct wl_proxy*)wrapped, queue);

	struct wl_callback* callback = wl_display_sync(wrapped);
	wl_proxy_wrapper_destroy(wrapped);

	if (!callback)
	{
		fprintf(stderr, "[get_serial] ERROR: wl_display_sync failed\n");
		wl_event_queue_destroy(queue);
		return 0;
	}
	wl_callback_add_listener(callback, &callback_listener, &serial);

	while (serial == 0)
	{
		if (wl_display_dispatch_queue(display, queue) < 0)
		{
			fprintf(stderr, "[get_serial] ERROR: wl_display_dispatch_queue failed\n");
			break;
		}
	}

	fprintf(stderr, "[get_serial] Got serial: %u\n", serial);

	wl_callback_destroy(callback);
	wl_event_queue_destroy(queue);
	return serial;
}

/* Thread-safe replacement for wl_display_roundtrip(); see get_serial(). */
static void clipboard_roundtrip(UwacSeat* s)
{
	struct wl_display* display = s->display->display;
	struct wl_event_queue* queue = wl_display_create_queue(display);
	if (!queue)
	{
		/* Without a private queue we must not dispatch the default queue from
		 * this thread; just push pending requests out and let the main loop read. */
		fprintf(stderr, "[clipboard_roundtrip] WARNING: Failed to create private queue, flushing display\n");
		wl_display_flush(display);
		return;
	}

	int ret = wl_display_roundtrip_queue(display, queue);
	if (ret < 0)
	{
		fprintf(stderr, "[clipboard_roundtrip] ERROR: wl_display_roundtrip_queue returned %d\n", ret);
	}
	else
	{
		fprintf(stderr, "[clipboard_roundtrip] SUCCESS: roundtrip completed with result %d\n", ret);
	}
	wl_event_queue_destroy(queue);
}

UwacReturnCode UwacClipboardOfferAnnounce(UwacSeat* seat, void* context,
                                          UwacDataTransferHandler transfer,
                                          UwacCancelDataTransferHandler cancel)
{
	if (!seat)
		return UWAC_ERROR_INTERNAL;

	seat->data_context = context;
	seat->transfer_data = transfer;
	seat->cancel_data = cancel;
	seat->ignore_announcement = true;
	wl_data_device_set_selection(seat->data_device, seat->data_source, get_serial(seat));
	clipboard_roundtrip(seat);
	seat->ignore_announcement = false;
	return UWAC_SUCCESS;
}

void* UwacClipboardDataGet(UwacSeat* seat, const char* mime, size_t* size)
{
	ssize_t r = 0;
	size_t alloc = 0;
	size_t pos = 0;
	char* data = nullptr;
	int pipefd[2] = WINPR_C_ARRAY_INIT;

	if (!seat)
	{
		fprintf(stderr, "[UwacClipboardDataGet] ERROR: seat is NULL\n");
		return nullptr;
	}

	if (!mime)
	{
		fprintf(stderr, "[UwacClipboardDataGet] ERROR: mime is NULL\n");
		return nullptr;
	}

	if (!size)
	{
		fprintf(stderr, "[UwacClipboardDataGet] ERROR: size is NULL\n");
		return nullptr;
	}

	/* Make a local copy of the offer pointer to avoid race conditions with the main thread
	 * which can clear/modify it via data_device_data_offer callback. */
	struct wl_data_offer* local_offer = seat->offer;
	if (!local_offer)
	{
		fprintf(stderr,
		        "[UwacClipboardDataGet] ERROR: seat->offer is NULL (clipboard data not available or "
		        "cleared by main thread)\n");
		return nullptr;
	}

	fprintf(stderr, "[UwacClipboardDataGet] Getting clipboard data for mime: %s (offer=%p)\n", mime,
	        local_offer);

	*size = 0;
	if (pipe(pipefd) != 0)
	{
		fprintf(stderr, "[UwacClipboardDataGet] ERROR: pipe() failed\n");
		return nullptr;
	}

	fprintf(stderr, "[UwacClipboardDataGet] Calling wl_data_offer_receive for mime: %s\n", mime);
	wl_data_offer_receive(local_offer, mime, pipefd[1]);
	close(pipefd[1]);

	fprintf(stderr, "[UwacClipboardDataGet] Calling clipboard_roundtrip...\n");
	clipboard_roundtrip(seat);

	fprintf(stderr, "[UwacClipboardDataGet] Roundtrip complete, flushing display\n");
	wl_display_flush(seat->display->display);

	do
	{
		if (alloc >= SIZE_MAX - 1024)
			goto fail;

		alloc += 1024;
		void* tmp = xrealloc(data, alloc);
		if (!tmp)
			goto fail;

		data = tmp;

		if (pos >= alloc)
			goto fail;

		r = read(pipefd[0], &data[pos], alloc - pos);
		if (r > 0)
			pos += r;
		if (r < 0)
			goto fail;
	} while (r > 0);

	close(pipefd[0]);

	if (alloc > 0)
	{
		data[pos] = '\0';
		*size = pos + 1;
	}
	return data;

fail:
	free(data);
	close(pipefd[0]);
	return nullptr;
}
