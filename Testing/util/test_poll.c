#include "mercury_poll.h"
#include "mercury_event.h"

#include "mercury_test_config.h"

#include <stdio.h>
#include <stdlib.h>

struct hg_test_poll_cb_args {
    int event_fd;
};

static int
poll_cb(void *arg, int error, struct hg_poll_event *event)
{
    struct hg_test_poll_cb_args *poll_cb_args =
        (struct hg_test_poll_cb_args *) arg;
    (void) error;

    hg_event_get(poll_cb_args->event_fd, &event->progressed);

    return HG_UTIL_SUCCESS;
}

int
main(void)
{
    struct hg_test_poll_cb_args poll_cb_args;
    hg_poll_set_t *poll_set;
    struct hg_poll_event events[1];
    unsigned int nevents = 0;
    int event_fd, ret = EXIT_SUCCESS;

    poll_set = hg_poll_create();
    event_fd = hg_event_create();

    poll_cb_args.event_fd = event_fd;

    /* Add event descriptor */
    hg_poll_add(poll_set, event_fd, HG_POLLIN, poll_cb, &poll_cb_args);

    /* Set event */
    hg_event_set(event_fd);

    /* Wait with timeout 0 */
    hg_poll_wait(poll_set, 0, 1, events, &nevents);
    if ((nevents != 1) || !events[0].progressed) {
        /* We expect success */
        fprintf(stderr, "Error: should have progressed\n");
        ret = EXIT_FAILURE;
        goto done;
    }

    /* Reset progressed */
    nevents = 0;
    events[0].progressed = HG_UTIL_FALSE;

    /* Wait with timeout 0 */
    hg_poll_wait(poll_set, 0, 1, events, &nevents);
    if ((nevents != 1) || events[0].progressed) {
        /* We do not expect success */
        fprintf(stderr, "Error: should not have progressed\n");
        ret = EXIT_FAILURE;
        goto done;
    }

    /* Reset progressed */
    nevents = 0;
    events[0].progressed = HG_UTIL_FALSE;

    /* Wait with timeout */
    hg_poll_wait(poll_set, 100, 1, events, &nevents);
    if (nevents || events[0].progressed) {
        /* We do not expect success */
        fprintf(stderr, "Error: should not have progressed\n");
        ret = EXIT_FAILURE;
        goto done;
    }

    /* Set event */
    hg_event_set(event_fd);

    /* Reset progressed */
    nevents = 0;
    events[0].progressed = HG_UTIL_FALSE;

    /* Wait with timeout */
    hg_poll_wait(poll_set, 1000, 1, events, &nevents);
    if (!nevents || !events[0].progressed) {
        /* We expect success */
        fprintf(stderr, "Error: did not progress correctly\n");
        ret = EXIT_FAILURE;
        goto done;
    }

done:
    hg_poll_remove(poll_set, event_fd);
    hg_poll_destroy(poll_set);
    hg_event_destroy(event_fd);

    return ret;
}
