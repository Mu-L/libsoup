#include "libsoup/soup.h"

int LLVMFuzzerTestOneInput (const unsigned char *data, size_t size);
static int set_logger = 0;

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
static GLogWriterOutput
empty_logging_func (GLogLevelFlags log_level, const GLogField *fields,
                    gsize n_fields, gpointer user_data)
{
        return G_LOG_WRITER_HANDLED;
}
#endif

/* Disables logging for oss-fuzz. Must be used with each target. */
static void
fuzz_set_logging_func (void)
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        if (!set_logger)
        {
                set_logger = 1;
                g_log_set_writer_func (empty_logging_func, NULL, NULL);
        }
#endif
}
