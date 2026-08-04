/*
 * iperf_config.h — stub for cjson.c which unconditionally includes it.
 *
 * The upstream iperf3 tree generates this via autoconf with dozens of
 * HAVE_* feature macros. cjson.c itself doesn't read any of them, so
 * an empty stub is sufficient. Everything real our client cares about
 * (bsdsocket via Roadshow, newlib) is available unconditionally on
 * AmigaOS 4.
 */
