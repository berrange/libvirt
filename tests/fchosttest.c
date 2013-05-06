/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "virutil.h"
#include "testutils.h"

#define TEST_FC_HOST_PREFIX "./fchostdata/fc_host/"
#define TEST_FC_HOST_NUM 5

/* Test virIsCapableFCHost */
static int
test1(const void *data ATTRIBUTE_UNUSED)
{
    if (virIsCapableFCHost(TEST_FC_HOST_PREFIX,
                           TEST_FC_HOST_NUM))
        return 0;

    return -1;
}

/* Test virIsCapableVport */
static int
test2(const void *data ATTRIBUTE_UNUSED)
{
    if (virIsCapableVport(TEST_FC_HOST_PREFIX,
                          TEST_FC_HOST_NUM))
        return 0;

    return -1;
}

/* Test virReadFCHost */
static int
test3(const void *data ATTRIBUTE_UNUSED)
{
    const char *expect_wwnn = "2001001b32a9da4e";
    const char *expect_wwpn = "2101001b32a9da4e";
    const char *expect_fabric_wwn = "2001000dec9877c1";
    const char *expect_max_vports = "127";
    const char *expect_vports = "0";
    char *wwnn = NULL;
    char *wwpn = NULL;
    char *fabric_wwn = NULL;
    char *max_vports = NULL;
    char *vports = NULL;
    int ret = -1;

    if (virReadFCHost(TEST_FC_HOST_PREFIX,
                      TEST_FC_HOST_NUM,
                      "node_name",
                      &wwnn) < 0)
        return -1;

    if (virReadFCHost(TEST_FC_HOST_PREFIX,
                      TEST_FC_HOST_NUM,
                      "port_name",
                      &wwpn) < 0)
        goto cleanup;

    if (virReadFCHost(TEST_FC_HOST_PREFIX,
                      TEST_FC_HOST_NUM,
                      "fabric_name",
                      &fabric_wwn) < 0)
        goto cleanup;

    if (virReadFCHost(TEST_FC_HOST_PREFIX,
                      TEST_FC_HOST_NUM,
                      "max_npiv_vports",
                      &max_vports) < 0)
        goto cleanup;


    if (virReadFCHost(TEST_FC_HOST_PREFIX,
                      TEST_FC_HOST_NUM,
                      "npiv_vports_inuse",
                      &vports) < 0)
        goto cleanup;

    if (STRNEQ(expect_wwnn, wwnn) ||
        STRNEQ(expect_wwpn, wwpn) ||
        STRNEQ(expect_fabric_wwn, fabric_wwn) ||
        STRNEQ(expect_max_vports, max_vports) ||
        STRNEQ(expect_vports, vports))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(wwnn);
    VIR_FREE(wwpn);
    VIR_FREE(fabric_wwn);
    VIR_FREE(max_vports);
    VIR_FREE(vports);
    return ret;
}

/* Test virGetFCHostNameByWWN */
static int
test4(const void *data ATTRIBUTE_UNUSED)
{
    const char *expect_hostname = "host5";
    char *hostname = NULL;
    int ret = -1;

    if (!(hostname = virGetFCHostNameByWWN(TEST_FC_HOST_PREFIX,
                                           "2001001b32a9da4e",
                                           "2101001b32a9da4e")))
        return -1;

    if (STRNEQ(hostname, expect_hostname))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(hostname);
    return ret;
}

/* Test virFindFCHostCapableVport (host4 is not Online) */
static int
test5(const void *data ATTRIBUTE_UNUSED)
{
    const char *expect_hostname = "host5";
    char *hostname = NULL;
    int ret = -1;

    if (!(hostname = virFindFCHostCapableVport(TEST_FC_HOST_PREFIX)))
        return -1;

    if (STRNEQ(hostname, expect_hostname))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(hostname);
    return ret;
}

static int
mymain(void)
{
    int ret = 0;

    if (virtTestRun("test1", 1, test1, NULL) < 0)
        ret = -1;
    if (virtTestRun("test2", 1, test2, NULL) < 0)
        ret = -1;
    if (virtTestRun("test3", 1, test3, NULL) < 0)
        ret = -1;
    if (virtTestRun("test4", 1, test4, NULL) < 0)
        ret = -1;
    if (virtTestRun("test5", 1, test5, NULL) < 0)
        ret = -1;

    return ret;
}

VIRT_TEST_MAIN(mymain)
