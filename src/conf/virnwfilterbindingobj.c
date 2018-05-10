/*
 * virnwfilterbindingobj.c: network filter binding XML processing
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
#include "nwfilter_params.h"
#include "virnwfilterbindingobj.h"
#include "viruuid.h"


#define VIR_FROM_THIS VIR_FROM_NWFILTER

static virClassPtr virNWFilterBindingObjClass;
static void virNWFilterBindingObjDispose(void *obj);

static int virNWFilterBindingObjOnceInit(void)
{
    if (!VIR_CLASS_NEW(virNWFilterBindingObj, virClassForObjectLockable()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virNWFilterBindingObj)

virNWFilterBindingObjPtr
virNWFilterBindingObjNew(void)
{
    if (virNWFilterBindingObjInitialize() < 0)
        return NULL;

    return virObjectNew(virNWFilterBindingObjClass);
}

void
virNWFilterBindingObjDispose(void *obj)
{
    virNWFilterBindingObjPtr bobj = obj;

    virNWFilterBindingDefFree(bobj->def);
}


/**
 * virNWFilterBindingnObjEndAPI:
 * @obj: binding object
 *
 * Finish working with a binding object in an API.  This function
 * clears whatever was left of a domain that was gathered using
 * virNWFilterBindingObjListFindByPortDev(). Currently that means
 * only unlocking and decrementing the reference counter of that
 * object. And in order to make sure the caller does not access
 * the object, the pointer is cleared.
 */
void
virNWFilterBindingObjEndAPI(virNWFilterBindingObjPtr *obj)
{
    if (!*obj)
        return;

    virObjectUnlock(*obj);
    virObjectUnref(*obj);
    *obj = NULL;
}


char *
virNWFilterBindingObjConfigFile(const char *dir,
                                const char *name)
{
    char *ret;

    ignore_value(virAsprintf(&ret, "%s/%s.xml", dir, name));
    return ret;
}


static virNWFilterBindingObjPtr
virNWFilterBindingObjParseXML(xmlDocPtr doc,
                              xmlXPathContextPtr ctxt)
{
    virNWFilterBindingObjPtr ret;
    xmlNodePtr node;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    if (virXPathInt("string(./portdev/@index)", ctxt, &ret->portdevindex) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("filter binding status has no port dev index"));
        goto cleanup;
    }

    if (!(node = virXPathNode("./filterbinding", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("filter binding status missing binding"));
        goto cleanup;
    }

    if (!(ret->def = virNWFilterBindingDefParseNode(doc, node)))
        goto cleanup;

    return ret;

 cleanup:
    virObjectUnref(ret);
    return NULL;
}


static virNWFilterBindingObjPtr
virNWFilterBindingObjParseNode(xmlDocPtr doc,
                               xmlNodePtr root)
{
    xmlXPathContextPtr ctxt = NULL;
    virNWFilterBindingObjPtr obj = NULL;

    if (STRNEQ((const char *)root->name, "filterbindingb")) {
        virReportError(VIR_ERR_XML_ERROR,
                       "%s",
                       _("unknown root element for nw filter"));
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(doc);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    obj = virNWFilterBindingObjParseXML(doc, ctxt);

 cleanup:
    xmlXPathFreeContext(ctxt);
    return obj;
}


static virNWFilterBindingObjPtr
virNWFilterBindingObjParse(const char *xmlStr,
                           const char *filename)
{
    virNWFilterBindingObjPtr obj = NULL;
    xmlDocPtr xml;

    if ((xml = virXMLParse(filename, xmlStr, _("(nwfilterbinding_status)")))) {
        obj = virNWFilterBindingObjParseNode(xml, xmlDocGetRootElement(xml));
        xmlFreeDoc(xml);
    }

    return obj;
}



virNWFilterBindingObjPtr
virNWFilterBindingObjParseFile(const char *filename)
{
    return virNWFilterBindingObjParse(NULL, filename);
}


char *
virNWFilterBindingObjFormat(const virNWFilterBindingObj *obj)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferAddLit(&buf, "<filterbindingstatus>\n");

    virBufferAdjustIndent(&buf, 2);

    virBufferAsprintf(&buf, "<portdev index='%d'/>\n", obj->portdevindex);

    if (virNWFilterBindingDefFormatBuf(&buf, obj->def) < 0) {
        virBufferFreeAndReset(&buf);
        return NULL;
    }

    virBufferAdjustIndent(&buf, -2);
    virBufferAddLit(&buf, "</filterbindingstatus>\n");

    if (virBufferCheckError(&buf) < 0)
        return NULL;

    return virBufferContentAndReset(&buf);
}
