#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmview.h"

static dmview_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmview_create();
}

void dmod_test_teardown(void)
{
    dmview_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmview_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmview_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmview_is_valid(g_handle));
}

DMOD_TEST_STEP(dmview_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmview_destroy(NULL);
}
