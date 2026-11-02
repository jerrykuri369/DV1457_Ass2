#ifndef SOLVER_H
#define SOLVER_H

#include <errno.h>
#include <stdlib.h>

/* Outcome of parsing a solver's command-line options. */
enum parse_result {
    PARSE_OK = 0,    /* options are valid: run the solver */
    PARSE_INFO = 1,  /* help or usage was requested: show the message only */
    PARSE_ERROR = -1 /* invalid options: show the message */
};

/* Parses a complete decimal integer in [min, max]. Returns 0 on success. */
static inline int parse_int_arg(const char *text, long min, long max, int *out)
{
    char *end;

    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max)
        return -1;
    *out = (int)value;
    return 0;
}

#endif
