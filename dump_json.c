/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2026 RackTop Systems Inc.
 */
#include "hwgrok.h"

#define	__STDC_FORMAT_MACROS
#include <inttypes.h>
#include <sys/smbios.h>

#define	PG_CHIP			"chip-properties"
#define	PG_DIMM			"dimm-properties"
#define	PG_FC			"fibre-channel"
#define	PG_UFM			"ufm-properties"
#define	PG_UFM_SLOT		"ufm-slot-properties"

#define	PROP_CAPACITY		"capacity-in-bytes"
#define	PROP_CHIP_BRAND		"brand"
#define	PROP_DIMM_MANUFACTURER	"manufacturer"
#define	PROP_DIMM_PRODUCT	"product"
#define	PROP_DIMM_SIZE_OLD	"dimm-size"
#define	PROP_FC_LINK_STATE	"link-state"
#define	PROP_FC_NODE_WWN	"node-wwn"
#define	PROP_FC_PORT_TYPE	"port-type"
#define	PROP_FC_PORT_WWN	"port-wwn"
#define	PROP_FC_SPEED		"speed"
#define	PROP_FC_SPEEDS		"supported-speeds"
#define	PROP_LOGICAL_BLOCK_SIZE	"logical-block-size"
#define	PROP_LOGICAL_DISK	"logical-disk"
#define	PROP_NVME_CAPACITY	"nvm-capacity"
#define	PROP_NVME_VERSION	"nvme-version"
#define	PROP_UFM_ACTIVE		"ufm-slot-active"
#define	PROP_UFM_DESCRIPTION	"ufm-description"
#define	PROP_UFM_MODE		"ufm-slot-mode"
#define	PROP_UFM_VERSION	"ufm-slot-version"

static hwg_info_t *dump_hwinfo;
static topo_hdl_t *dump_thp;

static void
json_string(const char *s)
{
	const unsigned char *p = (const unsigned char *)(s != NULL ? s : "");

	(void) putchar('"');
	for (; *p != '\0'; p++) {
		switch (*p) {
		case '"':
			(void) fputs("\\\"", stdout);
			break;
		case '\\':
			(void) fputs("\\\\", stdout);
			break;
		case '\b':
			(void) fputs("\\b", stdout);
			break;
		case '\f':
			(void) fputs("\\f", stdout);
			break;
		case '\n':
			(void) fputs("\\n", stdout);
			break;
		case '\r':
			(void) fputs("\\r", stdout);
			break;
		case '\t':
			(void) fputs("\\t", stdout);
			break;
		default:
			if (*p < 0x20)
				(void) printf("\\u%04x", *p);
			else
				(void) putchar(*p);
			break;
		}
	}
	(void) putchar('"');
}

static void
json_key(const char *key, boolean_t *first)
{
	if (!*first)
		(void) putchar(',');
	*first = B_FALSE;
	json_string(key);
	(void) putchar(':');
}

static void
json_string_field(const char *key, const char *value, boolean_t *first)
{
	if (value == NULL || value[0] == '\0')
		return;
	json_key(key, first);
	json_string(value);
}

static void
json_uint32_field(const char *key, uint32_t value, boolean_t *first)
{
	json_key(key, first);
	(void) printf("%" PRIu32, value);
}

static void
json_uint64_field(const char *key, uint64_t value, boolean_t *first)
{
	json_key(key, first);
	(void) printf("%" PRIu64, value);
}

static boolean_t
node_named(tnode_t *node, const char *name)
{
	return (node != NULL && strcmp(topo_node_name(node), name) == 0);
}

static boolean_t
node_is_descendant(tnode_t *node, tnode_t *ancestor)
{
	for (node = topo_node_parent(node); node != NULL;
	    node = topo_node_parent(node)) {
		if (node == ancestor)
			return (B_TRUE);
	}
	return (B_FALSE);
}

static tnode_t *
node_ancestor(tnode_t *node, const char *name1, const char *name2)
{
	for (; node != NULL; node = topo_node_parent(node)) {
		if (node_named(node, name1) ||
		    (name2 != NULL && node_named(node, name2)))
			return (node);
	}
	return (NULL);
}

static int
node_get_string(tnode_t *node, const char *group, const char *name,
    char **value)
{
	int err;

	*value = NULL;
	if (topo_prop_get_string(node, group, name, value, &err) == 0)
		return (0);
	return (err == ETOPO_PROP_NOENT ? 1 : -1);
}

static char *
node_get_label(tnode_t *node)
{
	char *label = NULL;
	int err;

	if (topo_node_label(node, &label, &err) != 0)
		return (NULL);
	return (label);
}

static char *
node_get_fmri(tnode_t *node)
{
	nvlist_t *fmri = NULL;
	char *raw = NULL, *slash, *ret = NULL;
	int err;

	if (topo_node_resource(node, &fmri, &err) != 0 ||
	    topo_fmri_nvl2str(dump_thp, fmri, &raw, &err) != 0)
		goto out;

	slash = strstr(raw + strlen("hc://"), "/");
	if (slash != NULL)
		(void) asprintf(&ret, "hc://%s", slash);
	else
		ret = strdup(raw);

out:
	if (raw != NULL)
		topo_hdl_strfree(dump_thp, raw);
	nvlist_free(fmri);
	return (ret);
}

static char *
node_get_fmri_string(tnode_t *node, const char *name)
{
	nvlist_t *fmri = NULL;
	char *value, *ret = NULL;
	int err;

	if (topo_node_resource(node, &fmri, &err) == 0 &&
	    nvlist_lookup_string(fmri, name, &value) == 0)
		ret = strdup(value);
	nvlist_free(fmri);
	return (ret);
}

static char *
node_get_ancestor_string(tnode_t *node, const char *group, const char *name)
{
	char *value = NULL;

	for (; node != NULL; node = topo_node_parent(node)) {
		if (node_get_string(node, group, name, &value) == 0)
			return (value);
	}
	return (NULL);
}

static boolean_t
node_has_string(tnode_t *node, const char *group, const char *name)
{
	char *value;

	if (node_get_string(node, group, name, &value) != 0)
		return (B_FALSE);
	topo_hdl_strfree(dump_thp, value);
	return (B_TRUE);
}

static tnode_t *
find_node(const char *name)
{
	hwg_node_ref_t *ref;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (node_named(ref->hwnr_node, name))
			return (ref->hwnr_node);
	}
	return (NULL);
}

static uint32_t
count_descendants(tnode_t *parent, const char *name)
{
	hwg_node_ref_t *ref;
	uint32_t count = 0;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (node_named(ref->hwnr_node, name) &&
		    node_is_descendant(ref->hwnr_node, parent))
			count++;
	}
	return (count);
}

static void
dump_ufm(tnode_t *owner, boolean_t *first)
{
	hwg_node_ref_t *ref, *sref;
	boolean_t first_ufm = B_TRUE;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *ufm = ref->hwnr_node;
		char *description = NULL;
		boolean_t first_field = B_TRUE, first_slot = B_TRUE;

		if (!node_named(ufm, UFM) || topo_node_parent(ufm) != owner)
			continue;

		if (first_ufm) {
			json_key("ufm", first);
			(void) putchar('[');
		} else {
			(void) putchar(',');
		}
		first_ufm = B_FALSE;
		(void) putchar('{');

		if (node_get_string(ufm, PG_UFM, PROP_UFM_DESCRIPTION,
		    &description) == 0) {
			json_string_field("description", description, &first_field);
			topo_hdl_strfree(dump_thp, description);
		}

		json_key("slots", &first_field);
		(void) putchar('[');
		for (sref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
		    sref != NULL; sref = (hwg_node_ref_t *)sref->ll_next) {
			tnode_t *slot = sref->hwnr_node;
			char *version = NULL;
			uint32_t mode, active;
			int err;
			boolean_t sf = B_TRUE;
			const char *modestr = NULL;

			if (!node_named(slot, SLOT) ||
			    topo_node_parent(slot) != ufm)
				continue;
			if (!first_slot)
				(void) putchar(',');
			first_slot = B_FALSE;
			(void) putchar('{');
			json_uint32_field("id", (uint32_t)topo_node_instance(slot),
			    &sf);

			if (node_get_string(slot, PG_UFM_SLOT, PROP_UFM_VERSION,
			    &version) == 0) {
				json_string_field("version", version, &sf);
				topo_hdl_strfree(dump_thp, version);
			}
			if (topo_prop_get_uint32(slot, PG_UFM_SLOT,
			    PROP_UFM_ACTIVE, &active, &err) == 0) {
				json_key("active", &sf);
				(void) fputs(active != 0 ? "true" : "false",
				    stdout);
			}
			if (topo_prop_get_uint32(slot, PG_UFM_SLOT,
			    PROP_UFM_MODE, &mode, &err) == 0) {
				switch (mode) {
				case TOPO_UFM_SLOT_MODE_NONE:
					modestr = "none";
					break;
				case TOPO_UFM_SLOT_MODE_RO:
					modestr = "read-only";
					break;
				case TOPO_UFM_SLOT_MODE_WO:
					modestr = "write-only";
					break;
				case TOPO_UFM_SLOT_MODE_RW:
					modestr = "read-write";
					break;
				}
				json_string_field("mode", modestr, &sf);
			}
			(void) putchar('}');
		}
		(void) fputs("]}", stdout);
	}
	if (!first_ufm)
		(void) putchar(']');
}

static void
dump_transceiver(tnode_t *port, boolean_t *first)
{
	hwg_node_ref_t *ref;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *xcvr = ref->hwnr_node;
		char *type = NULL, *usable = NULL, *vendor = NULL;
		char *part = NULL, *rev = NULL, *serial = NULL;
		boolean_t tf = B_TRUE;

		if (!node_named(xcvr, TRANSCEIVER) ||
		    topo_node_parent(xcvr) != port)
			continue;

		json_key("transceiver", first);
		(void) putchar('{');
		if (node_get_string(xcvr, TOPO_PGROUP_TRANSCEIVER,
		    TOPO_PROP_TRANSCEIVER_TYPE, &type) == 0) {
			json_string_field("type", type, &tf);
			topo_hdl_strfree(dump_thp, type);
		}
		if (node_get_string(xcvr, TOPO_PGROUP_TRANSCEIVER,
		    TOPO_PROP_TRANSCEIVER_USABLE, &usable) == 0) {
			json_key("usable", &tf);
			(void) fputs(strcmp(usable, "true") == 0 ?
			    "true" : "false", stdout);
			topo_hdl_strfree(dump_thp, usable);
		}
		if (node_get_string(xcvr, TOPO_PGROUP_SFF_TRANSCEIVER,
		    TOPO_PORT_SFF_TRANSCEIVER_VENDOR, &vendor) == 0) {
			json_string_field("vendor", vendor, &tf);
			topo_hdl_strfree(dump_thp, vendor);
		}
		if (node_get_string(xcvr, TOPO_PGROUP_SFF_TRANSCEIVER,
		    TOPO_PORT_SFF_TRANSCEIVER_PN, &part) == 0) {
			json_string_field("part_number", part, &tf);
			topo_hdl_strfree(dump_thp, part);
		}
		if (node_get_string(xcvr, TOPO_PGROUP_SFF_TRANSCEIVER,
		    TOPO_PORT_SFF_TRANSCEIVER_REV, &rev) == 0) {
			json_string_field("revision", rev, &tf);
			topo_hdl_strfree(dump_thp, rev);
		}
		if (node_get_string(xcvr, TOPO_PGROUP_SFF_TRANSCEIVER,
		    TOPO_PORT_SFF_TRANSCEIVER_SN, &serial) == 0) {
			json_string_field("serial_number", serial, &tf);
			topo_hdl_strfree(dump_thp, serial);
		}
		(void) putchar('}');
		return;
	}
}

static tnode_t *
port_pcifn(tnode_t *port)
{
	return (node_ancestor(port, PCI_FUNCTION, PCIEX_FUNCTION));
}

static void
dump_pci_identity(tnode_t *fn, boolean_t *first)
{
	char *vendor = NULL, *model = NULL, *serial;

	if (fn == NULL)
		return;
	if (node_get_string(fn, TOPO_PGROUP_PCI, TOPO_PCI_VENDNM,
	    &vendor) == 0) {
		json_string_field("manufacturer", vendor, first);
		topo_hdl_strfree(dump_thp, vendor);
	}
	if (node_get_string(fn, TOPO_PGROUP_PCI, TOPO_PCI_DEVNM,
	    &model) == 0) {
		json_string_field("model", model, first);
		topo_hdl_strfree(dump_thp, model);
	}
	if ((serial = node_get_fmri_string(fn, FM_FMRI_HC_SERIAL_ID)) != NULL) {
		json_string_field("serial_number", serial, first);
		free(serial);
	}
}

static void
dump_driver_name(tnode_t *node, boolean_t emit_name, boolean_t *first)
{
	char *driver;
	uint32_t instance;
	int err;

	driver = node_get_ancestor_string(node, TOPO_PGROUP_IO,
	    TOPO_IO_DRIVER);
	if (driver == NULL)
		return;

	for (; node != NULL; node = topo_node_parent(node)) {
		if (topo_prop_get_uint32(node, TOPO_PGROUP_IO,
		    TOPO_IO_INSTANCE, &instance, &err) == 0)
			break;
	}
	if (node != NULL) {
		char name[128];

		(void) snprintf(name, sizeof (name), "%s%" PRIu32, driver,
		    instance);
		if (emit_name)
			json_string_field("name", name, first);
		json_string_field("driver", driver, first);
		json_uint32_field("instance", instance, first);
	} else {
		json_string_field("driver", driver, first);
	}
	topo_hdl_strfree(dump_thp, driver);
}

static void
dump_location(tnode_t *fn, boolean_t *first)
{
	char *label;
	boolean_t lf = B_TRUE;

	if (fn == NULL)
		return;
	label = node_get_label(fn);
	json_key("location", first);
	(void) putchar('{');
	if (label == NULL || strcmp(label, "MB") == 0) {
		json_string_field("type", "onboard", &lf);
		if (label != NULL)
			json_string_field("label", label, &lf);
	} else {
		json_string_field("type", "pci-slot", &lf);
		json_string_field("slot", label, &lf);
	}
	(void) putchar('}');
	if (label != NULL)
		topo_hdl_strfree(dump_thp, label);
}

static void
dump_network_interface(tnode_t *port)
{
	tnode_t *fn = port_pcifn(port);
	char *id, *name = NULL, *mac = NULL;
	boolean_t first = B_TRUE;

	(void) putchar('{');
	if ((id = node_get_fmri(port)) != NULL) {
		json_string_field("id", id, &first);
		free(id);
	}
	if (node_get_string(port, TOPO_PGROUP_DATALINK,
	    TOPO_PGROUP_DATALINK_LINK_NAME, &name) == 0) {
		json_string_field("name", name, &first);
		topo_hdl_strfree(dump_thp, name);
	}
	dump_driver_name(fn, B_FALSE, &first);
	dump_pci_identity(fn, &first);
	if (node_get_string(port, TOPO_PGROUP_DATALINK,
	    TOPO_PGROUP_DATALINK_PMAC, &mac) == 0) {
		json_string_field("primary_mac_address", mac, &first);
		topo_hdl_strfree(dump_thp, mac);
	}
	dump_location(fn, &first);
	dump_transceiver(port, &first);
	if (fn != NULL)
		dump_ufm(fn, &first);
	else
		dump_ufm(port, &first);
	(void) putchar('}');
}

static void
dump_fc_port(tnode_t *port)
{
	char *id, *value = NULL;
	uint64_t speed, *speeds = NULL;
	uint_t nspeeds = 0;
	uint32_t instance;
	int err;
	boolean_t first = B_TRUE;

	(void) putchar('{');
	if ((id = node_get_fmri(port)) != NULL) {
		json_string_field("id", id, &first);
		free(id);
	}
	dump_driver_name(port, B_TRUE, &first);

	if (node_get_string(port, PG_FC, PROP_FC_NODE_WWN, &value) == 0) {
		json_string_field("node_wwn", value, &first);
		topo_hdl_strfree(dump_thp, value);
	}
	if (node_get_string(port, PG_FC, PROP_FC_PORT_WWN, &value) == 0) {
		json_string_field("port_wwn", value, &first);
		topo_hdl_strfree(dump_thp, value);
	}
	if (node_get_string(port, PG_FC, PROP_FC_LINK_STATE, &value) == 0) {
		json_string_field("link_state", value, &first);
		topo_hdl_strfree(dump_thp, value);
	}
	if (node_get_string(port, PG_FC, PROP_FC_PORT_TYPE, &value) == 0) {
		json_string_field("port_type", value, &first);
		topo_hdl_strfree(dump_thp, value);
	}
	if (topo_prop_get_uint64(port, PG_FC, PROP_FC_SPEED, &speed,
	    &err) == 0)
		json_uint64_field("speed_bps", speed, &first);
	if (topo_prop_get_uint64_array(port, PG_FC, PROP_FC_SPEEDS,
	    &speeds, &nspeeds, &err) == 0) {
		json_key("supported_speeds_bps", &first);
		(void) putchar('[');
		for (instance = 0; instance < nspeeds; instance++) {
			if (instance != 0)
				(void) putchar(',');
			(void) printf("%" PRIu64, speeds[instance]);
		}
		(void) putchar(']');
		topo_hdl_free(dump_thp, speeds,
		    nspeeds * sizeof (uint64_t));
	}
	dump_transceiver(port, &first);
	dump_ufm(port, &first);
	(void) putchar('}');
}

static boolean_t
node_has_fc_ports(tnode_t *fn)
{
	hwg_node_ref_t *ref;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (node_is_descendant(ref->hwnr_node, fn) &&
		    node_has_string(ref->hwnr_node, PG_FC,
		    PROP_FC_PORT_WWN))
			return (B_TRUE);
	}
	return (B_FALSE);
}

static boolean_t
node_has_network_ports(tnode_t *fn)
{
	hwg_node_ref_t *ref;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (node_is_descendant(ref->hwnr_node, fn) &&
		    node_has_string(ref->hwnr_node, TOPO_PGROUP_DATALINK,
		    TOPO_PGROUP_DATALINK_LINK_NAME))
			return (B_TRUE);
	}
	return (B_FALSE);
}

static tnode_t *
node_nvme(tnode_t *fn)
{
	hwg_node_ref_t *ref;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (node_named(ref->hwnr_node, NVME) &&
		    node_is_descendant(ref->hwnr_node, fn))
			return (ref->hwnr_node);
	}
	return (NULL);
}

static void
dump_fc_adapter(tnode_t *fn)
{
	hwg_node_ref_t *ref;
	boolean_t first = B_TRUE, first_port = B_TRUE;
	char *id;

	(void) putchar('{');
	json_string_field("type", "fibre-channel", &first);
	if ((id = node_get_fmri(fn)) != NULL) {
		json_string_field("id", id, &first);
		free(id);
	}
	dump_driver_name(fn, B_TRUE, &first);
	dump_pci_identity(fn, &first);
	json_key("ports", &first);
	(void) putchar('[');
	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (!node_is_descendant(ref->hwnr_node, fn) ||
		    !node_has_string(ref->hwnr_node, PG_FC,
		    PROP_FC_PORT_WWN))
			continue;
		if (!first_port)
			(void) putchar(',');
		first_port = B_FALSE;
		dump_fc_port(ref->hwnr_node);
	}
	(void) putchar(']');
	dump_ufm(fn, &first);
	(void) putchar('}');
}

static void
dump_network_adapter(tnode_t *fn)
{
	hwg_node_ref_t *ref;
	boolean_t first = B_TRUE, first_if = B_TRUE;
	char *id;

	(void) putchar('{');
	json_string_field("type", "network", &first);
	if ((id = node_get_fmri(fn)) != NULL) {
		json_string_field("id", id, &first);
		free(id);
	}
	dump_pci_identity(fn, &first);
	json_key("interfaces", &first);
	(void) putchar('[');
	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		char *portid;
		boolean_t rf = B_TRUE;

		if (!node_is_descendant(ref->hwnr_node, fn) ||
		    !node_has_string(ref->hwnr_node, TOPO_PGROUP_DATALINK,
		    TOPO_PGROUP_DATALINK_LINK_NAME))
			continue;
		if (!first_if)
			(void) putchar(',');
		first_if = B_FALSE;
		(void) putchar('{');
		if ((portid = node_get_fmri(ref->hwnr_node)) != NULL) {
			json_string_field("ref", portid, &rf);
			free(portid);
		}
		(void) putchar('}');
	}
	(void) putchar(']');
	dump_ufm(fn, &first);
	(void) putchar('}');
}

static void
dump_nvme(tnode_t *nvme)
{
	hwg_node_ref_t *ref;
	tnode_t *first_disk = NULL;
	boolean_t first = B_TRUE, first_ns = B_TRUE;
	char *id, *model, *serial, *value = NULL;
	uint64_t capacity;
	int err;

	(void) putchar('{');
	json_string_field("type", "nvme", &first);
	if ((id = node_get_fmri(nvme)) != NULL) {
		json_string_field("id", id, &first);
		free(id);
	}
	dump_driver_name(nvme, B_TRUE, &first);
	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (node_named(ref->hwnr_node, DISK) &&
		    topo_node_parent(ref->hwnr_node) == nvme) {
			first_disk = ref->hwnr_node;
			break;
		}
	}
	if (first_disk != NULL && node_get_string(first_disk,
	    TOPO_PGROUP_STORAGE, TOPO_STORAGE_MANUFACTURER, &value) == 0) {
		json_string_field("vendor", value, &first);
		topo_hdl_strfree(dump_thp, value);
	}
	if ((model = node_get_fmri_string(nvme, FM_FMRI_HC_PART)) != NULL) {
		json_string_field("product", model, &first);
		free(model);
	}
	if ((serial = node_get_fmri_string(nvme,
	    FM_FMRI_HC_SERIAL_ID)) != NULL) {
		json_string_field("serial_number", serial, &first);
		free(serial);
	}
	if (first_disk != NULL && node_get_string(first_disk,
	    TOPO_PGROUP_STORAGE, TOPO_STORAGE_FIRMWARE_REV, &value) == 0) {
		json_string_field("firmware_revision", value, &first);
		topo_hdl_strfree(dump_thp, value);
	}
	if (node_get_string(nvme, TOPO_PGROUP_NVME,
	    PROP_NVME_VERSION, &value) == 0) {
		json_string_field("nvme_version", value, &first);
		topo_hdl_strfree(dump_thp, value);
	}
	if (topo_prop_get_uint64(nvme, TOPO_PGROUP_NVME,
	    PROP_NVME_CAPACITY, &capacity, &err) == 0)
		json_uint64_field("capacity_bytes", capacity, &first);

	json_key("namespaces", &first);
	(void) putchar('[');
	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *disk = ref->hwnr_node;
		char *nsid, *name = NULL, *vendor = NULL, *product = NULL;
		char *nsserial = NULL;
		uint64_t size, block_size;
		boolean_t nf = B_TRUE;

		if (!node_named(disk, DISK) ||
		    topo_node_parent(disk) != nvme)
			continue;
		if (!first_ns)
			(void) putchar(',');
		first_ns = B_FALSE;
		(void) putchar('{');
		if ((nsid = node_get_fmri(disk)) != NULL) {
			json_string_field("id", nsid, &nf);
			free(nsid);
		}
		json_uint32_field("namespace_id",
		    (uint32_t)topo_node_instance(disk) + 1, &nf);
		if (node_get_string(disk, TOPO_PGROUP_STORAGE,
		    PROP_LOGICAL_DISK, &name) == 0) {
			json_string_field("name", name, &nf);
			topo_hdl_strfree(dump_thp, name);
		}
		if (node_get_string(disk, TOPO_PGROUP_STORAGE,
		    TOPO_STORAGE_MANUFACTURER, &vendor) == 0) {
			json_string_field("vendor", vendor, &nf);
			topo_hdl_strfree(dump_thp, vendor);
		}
		if (node_get_string(disk, TOPO_PGROUP_STORAGE,
		    TOPO_STORAGE_MODEL, &product) == 0) {
			json_string_field("product", product, &nf);
			topo_hdl_strfree(dump_thp, product);
		}
		if (node_get_string(disk, TOPO_PGROUP_STORAGE,
		    "serial-number", &nsserial) == 0) {
			json_string_field("serial_number", nsserial, &nf);
			topo_hdl_strfree(dump_thp, nsserial);
		}
		if (topo_prop_get_uint64(disk, TOPO_PGROUP_STORAGE,
		    PROP_CAPACITY, &size, &err) == 0)
			json_uint64_field("size_bytes", size, &nf);
		if (topo_prop_get_uint64(disk, TOPO_PGROUP_STORAGE,
		    PROP_LOGICAL_BLOCK_SIZE, &block_size, &err) == 0)
			json_uint64_field("logical_block_size", block_size, &nf);
		(void) putchar('}');
	}
	(void) putchar(']');
	dump_ufm(nvme, &first);
	(void) putchar('}');
}

static void
dump_generic_pci(tnode_t *fn)
{
	boolean_t first = B_TRUE;
	char *id;

	(void) putchar('{');
	json_string_field("type", "pci", &first);
	if ((id = node_get_fmri(fn)) != NULL) {
		json_string_field("id", id, &first);
		free(id);
	}
	dump_driver_name(fn, B_TRUE, &first);
	dump_pci_identity(fn, &first);
	dump_ufm(fn, &first);
	(void) putchar('}');
}

static void
dump_slot_occupant(tnode_t *fn)
{
	tnode_t *nvme;

	if ((nvme = node_nvme(fn)) != NULL)
		dump_nvme(nvme);
	else if (node_has_fc_ports(fn))
		dump_fc_adapter(fn);
	else if (node_has_network_ports(fn))
		dump_network_adapter(fn);
	else
		dump_generic_pci(fn);
}

static tnode_t *
find_slot_function(hwg_slot_info_t *slot)
{
	hwg_node_ref_t *ref;
	uint32_t bdf;
	int err;

	if (slot->hwsi_usage == SMB_SLU_AVAIL)
		return (NULL);

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *node = ref->hwnr_node;

		if (!node_named(node, PCI_FUNCTION) &&
		    !node_named(node, PCIEX_FUNCTION))
			continue;
		if (topo_prop_get_uint32(node, TOPO_PGROUP_PCI,
		    TOPO_PCI_BDF, &bdf, &err) == 0 &&
		    ((bdf >> 8) & 0xff) == slot->hwsi_bus &&
		    (bdf & 0xff) == slot->hwsi_df)
			return (node);
	}

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *node = ref->hwnr_node;
		char *label;
		boolean_t match;

		if (!node_named(node, PCI_FUNCTION) &&
		    !node_named(node, PCIEX_FUNCTION))
			continue;
		if ((label = node_get_label(node)) == NULL)
			continue;
		match = strcmp(label, slot->hwsi_label) == 0;
		topo_hdl_strfree(dump_thp, label);
		if (match)
			return (node);
	}
	return (NULL);
}

static void
dump_system(void)
{
	tnode_t *mb = find_node(MOTHERBOARD);
	boolean_t first = B_TRUE, mbfirst = B_TRUE;
	char *id, *value = NULL, *serial, *revision;

	(void) fputs("\"system\":{", stdout);
	json_string_field("serial_number", dump_hwinfo->hwi_system_serial,
	    &first);
	json_string_field("description", dump_hwinfo->hwi_system_description,
	    &first);
	json_string_field("manufacturer",
	    dump_hwinfo->hwi_system_manufacturer, &first);
	json_string_field("product", dump_hwinfo->hwi_system_product, &first);

	if (mb != NULL) {
		json_key("motherboard", &first);
		(void) putchar('{');
		if ((id = node_get_fmri(mb)) != NULL) {
			json_string_field("id", id, &mbfirst);
			free(id);
		}
		if (node_get_string(mb, TOPO_PGROUP_MOTHERBOARD,
		    TOPO_PROP_MB_MANUFACTURER, &value) == 0) {
			json_string_field("manufacturer", value, &mbfirst);
			topo_hdl_strfree(dump_thp, value);
		}
		if (node_get_string(mb, TOPO_PGROUP_MOTHERBOARD,
		    TOPO_PROP_MB_PRODUCT, &value) == 0) {
			json_string_field("model", value, &mbfirst);
			topo_hdl_strfree(dump_thp, value);
		}
		if ((serial = node_get_fmri_string(mb,
		    FM_FMRI_HC_SERIAL_ID)) != NULL) {
			json_string_field("serial_number", serial, &mbfirst);
			free(serial);
		}
		if ((revision = node_get_fmri_string(mb,
		    FM_FMRI_HC_REVISION)) != NULL) {
			json_string_field("revision", revision, &mbfirst);
			free(revision);
		}
		dump_ufm(mb, &mbfirst);
		(void) putchar('}');
	}
	(void) putchar('}');
}

static void
dump_cpus(void)
{
	hwg_node_ref_t *ref;
	boolean_t first_cpu = B_TRUE;

	(void) fputs("\"cpus\":[", stdout);
	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *chip = ref->hwnr_node;
		boolean_t first = B_TRUE;
		char *id, *label, *brand = NULL, *serial;
		uint32_t cores, threads;

		if (!node_named(chip, CHIP))
			continue;
		if (!first_cpu)
			(void) putchar(',');
		first_cpu = B_FALSE;
		(void) putchar('{');
		if ((id = node_get_fmri(chip)) != NULL) {
			json_string_field("id", id, &first);
			free(id);
		}
		if ((label = node_get_label(chip)) != NULL) {
			json_string_field("label", label, &first);
			topo_hdl_strfree(dump_thp, label);
		}
		if (node_get_string(chip, PG_CHIP, PROP_CHIP_BRAND,
		    &brand) == 0) {
			json_string_field("model", brand, &first);
			topo_hdl_strfree(dump_thp, brand);
		}
		if ((serial = node_get_fmri_string(chip,
		    FM_FMRI_HC_SERIAL_ID)) != NULL) {
			json_string_field("serial_number", serial, &first);
			free(serial);
		}
		cores = count_descendants(chip, CORE);
		threads = count_descendants(chip, STRAND);
		json_uint32_field("cores", cores, &first);
		json_uint32_field("threads", threads, &first);
		dump_ufm(chip, &first);
		(void) putchar('}');
	}
	(void) putchar(']');
}

static uint64_t
parse_size(const char *size)
{
	char *end;
	uint64_t value;

	if (size == NULL)
		return (0);
	value = strtoull(size, &end, 10);
	while (*end == ' ')
		end++;
	switch (*end) {
	case 'T':
	case 't':
		value *= 1024;
		/* FALLTHROUGH */
	case 'G':
	case 'g':
		value *= 1024;
		/* FALLTHROUGH */
	case 'M':
	case 'm':
		value *= 1024 * 1024;
		break;
	case 'K':
	case 'k':
		value *= 1024;
		break;
	}
	return (value);
}

static void
dump_dimms(void)
{
	hwg_node_ref_t *ref;
	boolean_t first_dimm = B_TRUE;

	(void) fputs("\"dimms\":[", stdout);
	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *dimm = ref->hwnr_node;
		boolean_t first = B_TRUE;
		char *id, *label, *value = NULL, *serial, *part;
		uint64_t size;
		int err;

		if (!node_named(dimm, DIMM))
			continue;
		if (!first_dimm)
			(void) putchar(',');
		first_dimm = B_FALSE;
		(void) putchar('{');
		if ((id = node_get_fmri(dimm)) != NULL) {
			json_string_field("id", id, &first);
			free(id);
		}
		if ((label = node_get_label(dimm)) != NULL) {
			json_string_field("label", label, &first);
			topo_hdl_strfree(dump_thp, label);
		}
		if (topo_prop_get_uint64(dimm, PG_DIMM, TOPO_PROP_DIMM_SIZE,
		    &size, &err) == 0) {
			json_uint64_field("size_bytes", size, &first);
		} else if (node_get_string(dimm, PG_DIMM,
		    PROP_DIMM_SIZE_OLD, &value) == 0) {
			size = parse_size(value);
			if (size != 0)
				json_uint64_field("size_bytes", size, &first);
			topo_hdl_strfree(dump_thp, value);
		}
		if (node_get_string(dimm, PG_DIMM, PROP_DIMM_MANUFACTURER,
		    &value) == 0) {
			json_string_field("manufacturer", value, &first);
			topo_hdl_strfree(dump_thp, value);
		}
		if (node_get_string(dimm, PG_DIMM, PROP_DIMM_PRODUCT,
		    &value) == 0) {
			json_string_field("product", value, &first);
			topo_hdl_strfree(dump_thp, value);
		}
		if ((serial = node_get_fmri_string(dimm,
		    FM_FMRI_HC_SERIAL_ID)) != NULL) {
			json_string_field("serial_number", serial, &first);
			free(serial);
		}
		if ((part = node_get_fmri_string(dimm,
		    FM_FMRI_HC_PART)) != NULL) {
			json_string_field("part_number", part, &first);
			free(part);
		}
		dump_ufm(dimm, &first);
		(void) putchar('}');
	}
	(void) putchar(']');
}

static void
dump_network_interfaces(void)
{
	hwg_node_ref_t *ref;
	boolean_t first = B_TRUE;

	(void) fputs("\"network_interfaces\":[", stdout);
	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		if (!node_has_string(ref->hwnr_node, TOPO_PGROUP_DATALINK,
		    TOPO_PGROUP_DATALINK_LINK_NAME))
			continue;
		if (!first)
			(void) putchar(',');
		first = B_FALSE;
		dump_network_interface(ref->hwnr_node);
	}
	(void) putchar(']');
}

static void
dump_smbios_slots(void)
{
	hwg_slot_info_t *slot;
	boolean_t first_slot = B_TRUE;

	for (slot = (hwg_slot_info_t *)dump_hwinfo->hwi_slots.ll_next;
	    slot != NULL; slot = (hwg_slot_info_t *)slot->ll_next) {
		tnode_t *fn = find_slot_function(slot);
		boolean_t first = B_TRUE;
		char id[64];

		if (!first_slot)
			(void) putchar(',');
		first_slot = B_FALSE;
		(void) putchar('{');
		(void) snprintf(id, sizeof (id), "smbios-slot:%u:%u:%u",
		    slot->hwsi_segment, slot->hwsi_bus, slot->hwsi_df);
		json_string_field("id", id, &first);
		json_string_field("label", slot->hwsi_label, &first);
		json_key("occupied", &first);
		(void) fputs(fn != NULL ? "true" : "false", stdout);
		json_key("occupant", &first);
		if (fn != NULL)
			dump_slot_occupant(fn);
		else
			(void) fputs("null", stdout);
		(void) putchar('}');
	}
}

static boolean_t
label_seen_before(hwg_node_ref_t *stop, const char *label)
{
	hwg_node_ref_t *ref;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL && ref != stop; ref = (hwg_node_ref_t *)ref->ll_next) {
		char *other;
		boolean_t same;

		if (!node_named(ref->hwnr_node, PCI_FUNCTION) &&
		    !node_named(ref->hwnr_node, PCIEX_FUNCTION))
			continue;
		if ((other = node_get_label(ref->hwnr_node)) == NULL)
			continue;
		same = strcmp(other, label) == 0;
		topo_hdl_strfree(dump_thp, other);
		if (same)
			return (B_TRUE);
	}
	return (B_FALSE);
}

static void
dump_topo_slots(void)
{
	hwg_node_ref_t *ref;
	boolean_t first_slot = B_TRUE;

	for (ref = (hwg_node_ref_t *)dump_hwinfo->hwi_nodes.ll_next;
	    ref != NULL; ref = (hwg_node_ref_t *)ref->ll_next) {
		tnode_t *fn = ref->hwnr_node;
		char *label, *id;
		boolean_t first = B_TRUE;

		if (!node_named(fn, PCI_FUNCTION) &&
		    !node_named(fn, PCIEX_FUNCTION))
			continue;
		if ((label = node_get_label(fn)) == NULL)
			continue;
		if (strcmp(label, "MB") == 0 || label_seen_before(ref, label)) {
			topo_hdl_strfree(dump_thp, label);
			continue;
		}
		if (!first_slot)
			(void) putchar(',');
		first_slot = B_FALSE;
		(void) putchar('{');
		if ((id = node_get_fmri(fn)) != NULL) {
			json_string_field("id", id, &first);
			free(id);
		}
		json_string_field("label", label, &first);
		json_key("occupied", &first);
		(void) fputs("true", stdout);
		json_key("occupant", &first);
		dump_slot_occupant(fn);
		(void) putchar('}');
		topo_hdl_strfree(dump_thp, label);
	}
}

static void
dump_slots(void)
{
	(void) fputs("\"slots\":[", stdout);
	if (dump_hwinfo->hwi_slots.ll_next != NULL)
		dump_smbios_slots();
	else
		dump_topo_slots();
	(void) putchar(']');
}

int
dump_hw_config_json(hwg_info_t *hwinfo, char *type)
{
	(void) type;
	dump_hwinfo = hwinfo;
	if (hwinfo->hwi_topo_hdl == NULL)
		return (-1);
	dump_thp = hwinfo->hwi_topo_hdl;

	(void) fputs("{\"schema_version\":1,", stdout);
	dump_system();
	(void) putchar(',');
	dump_cpus();
	(void) putchar(',');
	dump_dimms();
	(void) putchar(',');
	dump_network_interfaces();
	(void) putchar(',');
	dump_slots();
	(void) fputs("}\n", stdout);
	return (0);
}
