/*
 * virnetworkportdef.c: network port XML processing
 *
 * Copyright (C) 2018 Red Hat, Inc.
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
 */

#include <config.h>

#include "viralloc.h"
#include "virerror.h"
#include "virstring.h"
#include "virnetworkportdef.h"


#define VIR_FROM_THIS VIR_FROM_NETWORK

VIR_ENUM_IMPL(virNetworkPort, VIR_NETWORK_PORT_TYPE_LAST,
              "bridge", "direct", "hostdev-pci");

void
virNetworkPortDefFree(virNetworkPortDefPtr def)
{
    if (!def)
        return;

    VIR_FREE(def->ownername);
    VIR_FREE(def->group);

    virNetDevBandwidthFree(def->bandwidth);
    virNetDevVlanClear(&def->vlan);
    VIR_FREE(def->virtPortProfile);

    switch ((virNetworkPortType)def->type) {
    case VIR_NETWORK_PORT_TYPE_BRIDGE:
        VIR_FREE(def->data.bridge.brname);
        break;

    case VIR_NETWORK_PORT_TYPE_DIRECT:
        VIR_FREE(def->data.direct.linkdev);
        break;

    case VIR_NETWORK_PORT_TYPE_HOSTDEV_PCI:
        break;

    case VIR_NETWORK_PORT_TYPE_LAST:
    default:
        break;
    }

    VIR_FREE(def);
}



static virNetworkPortDefPtr
virNetworkPortDefParseXML(xmlXPathContextPtr ctxt)
{
    virNetworkPortDefPtr def;
    char *uuid = NULL;
    xmlNodePtr virtPortNode;
    xmlNodePtr vlanNode;
    xmlNodePtr bandwidth_node;
    char *trustGuestRxFilters = NULL;
    char *mac = NULL;

    if (VIR_ALLOC(def) < 0)
        return NULL;

    uuid = virXPathString("string(./uuid)", ctxt);
    if (!uuid) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("network port has no uuid"));
        goto cleanup;
    }
    if (virUUIDParse(uuid, def->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to parse UUID '%s'"), uuid);
        VIR_FREE(uuid);
        goto cleanup;
    }
    VIR_FREE(uuid);

    def->ownername = virXPathString("string(./owner/name)", ctxt);
    if (!def->ownername) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("network port has no owner name"));
        goto cleanup;
    }

    uuid = virXPathString("string(./owner/uuid)", ctxt);
    if (!uuid) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("network port has no owner UUID"));
        goto cleanup;
    }

    if (virUUIDParse(uuid, def->owneruuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to parse UUID '%s'"), uuid);
        VIR_FREE(uuid);
        goto cleanup;
    }
    VIR_FREE(uuid);

    def->group = virXPathString("string(./group)", ctxt);

    virtPortNode = virXPathNode("./virtualport", ctxt);
    if (virtPortNode &&
        (!(def->virtPortProfile = virNetDevVPortProfileParse(virtPortNode, 0)))) {
        goto cleanup;
    }

    mac = virXPathString("string(./mac/@address)", ctxt);
    if (!mac) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("network port has no mac"));
        goto cleanup;
    }
    if (virMacAddrParse(mac, &def->mac) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to parse MAC '%s'"), mac);
        VIR_FREE(mac);
        goto cleanup;
    }
    VIR_FREE(mac);

    bandwidth_node = virXPathNode("./bandwidth", ctxt);
    if (bandwidth_node &&
        virNetDevBandwidthParse(&def->bandwidth, bandwidth_node, -1) < 0)
        goto cleanup;

    vlanNode = virXPathNode("./vlan", ctxt);
    if (vlanNode && virNetDevVlanParse(vlanNode, ctxt, &def->vlan) < 0)
        goto cleanup;


    trustGuestRxFilters
        = virXPathString("string(./rxfilters/@trustGuest)", ctxt);
    if (trustGuestRxFilters) {
        if ((def->trustGuestRxFilters
             = virTristateBoolTypeFromString(trustGuestRxFilters)) <= 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("Invalid guest rx filters trust setting '%s' "),
                           trustGuestRxFilters);
            goto cleanup;
        }
    }



    return def;

 cleanup:
    virNetworkPortDefFree(def);
    return NULL;
}


virNetworkPortDefPtr
virNetworkPortDefParseNode(xmlDocPtr xml,
                           xmlNodePtr root)
{
    xmlXPathContextPtr ctxt = NULL;
    virNetworkPortDefPtr def = NULL;

    if (STRNEQ((const char *)root->name, "networkport")) {
        virReportError(VIR_ERR_XML_ERROR,
                       "%s",
                       _("unknown root element for network port"));
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    def = virNetworkPortDefParseXML(ctxt);

 cleanup:
    xmlXPathFreeContext(ctxt);
    return def;
}


static virNetworkPortDefPtr
virNetworkPortDefParse(const char *xmlStr,
                       const char *filename)
{
    virNetworkPortDefPtr def = NULL;
    xmlDocPtr xml;

    if ((xml = virXMLParse(filename, xmlStr, _("(networkport_definition)")))) {
        def = virNetworkPortDefParseNode(xml, xmlDocGetRootElement(xml));
        xmlFreeDoc(xml);
    }

    return def;
}


virNetworkPortDefPtr
virNetworkPortDefParseString(const char *xmlStr)
{
    return virNetworkPortDefParse(xmlStr, NULL);
}


virNetworkPortDefPtr
virNetworkPortDefParseFile(const char *filename)
{
    return virNetworkPortDefParse(NULL, filename);
}


char *
virNetworkPortDefFormat(const virNetworkPortDef *def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (virNetworkPortDefFormatBuf(&buf, def) < 0) {
        virBufferFreeAndReset(&buf);
        return NULL;
    }

    if (virBufferCheckError(&buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}


int
virNetworkPortDefFormatBuf(virBufferPtr buf,
                           const virNetworkPortDef *def)
{
    char uuid[VIR_UUID_STRING_BUFLEN];
    char macaddr[VIR_MAC_STRING_BUFLEN];

    virBufferAddLit(buf, "<networkport>\n");

    virBufferAdjustIndent(buf, 2);

    virUUIDFormat(def->uuid, uuid);
    virBufferAsprintf(buf, "<uuid>%s</uuid>\n", uuid);

    virBufferAddLit(buf, "<owner>\n");
    virBufferAdjustIndent(buf, 2);
    virBufferEscapeString(buf, "<name>%s</name>\n", def->ownername);
    virUUIDFormat(def->owneruuid, uuid);
    virBufferAsprintf(buf, "<uuid>%s</uuid>\n", uuid);
    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</owner>\n");

    if (def->group)
        virBufferEscapeString(buf, "<group>%s</group>\n", def->group);

    virMacAddrFormat(&def->mac, macaddr);
    virBufferAsprintf(buf, "<mac address='%s'/>\n", macaddr);

    if (def->trustGuestRxFilters)
        virBufferAsprintf(buf, "<rxfilters trustGuest='%s'/>",
                          virTristateBoolTypeToString(def->trustGuestRxFilters));

    if (virNetDevVlanFormat(&def->vlan, buf) < 0)
        return -1;
    if (virNetDevVPortProfileFormat(def->virtPortProfile, buf) < 0)
        return -1;
    virNetDevBandwidthFormat(def->bandwidth, buf);

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</networkport>\n");

    return 0;
}
