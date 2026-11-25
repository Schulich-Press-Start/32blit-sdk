#ifndef TINYALSA_INTERVAL_H
#define TINYALSA_INTERVAL_H

#include <stdlib.h>
#include <unistd.h>

/** A closed range signed interval. */

struct tinyalsa_signed_interval {
    /** The maximum value of the interval */
    ssize_t max;
    /** The minimum value of the interval */
    ssize_t min;
};

/** A closed range unsigned interval. */

struct tinyalsa_unsigned_interval {
    /** The maximum value of the interval */
    size_t max;
    /** The minimum value of the interval */
    size_t min;
};

#endif /* TINYALSA_INTERVAL_H */