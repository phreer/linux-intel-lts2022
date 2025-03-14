// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  ACPI-WMI mapping driver
 *
 *  Copyright (C) 2007-2008 Carlos Corbacho <carlos@strangeworlds.co.uk>
 *
 *  GUID parsing code from ldm.c is:
 *   Copyright (C) 2001,2002 Richard Russon <ldm@flatcap.org>
 *   Copyright (c) 2001-2007 Anton Altaparmakov
 *   Copyright (C) 2001,2002 Jakob Kemi <jakob.kemi@telia.com>
 *
 *  WMI bus infrastructure by Andrew Lutomirski and Darren Hart:
 *    Copyright (C) 2015 Andrew Lutomirski
 *    Copyright (C) 2017 VMware, Inc. All Rights Reserved.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uuid.h>
#include <linux/wmi.h>
#include <linux/fs.h>
#include <uapi/linux/wmi.h>

MODULE_AUTHOR("Carlos Corbacho");
MODULE_DESCRIPTION("ACPI-WMI Mapping Driver");
MODULE_LICENSE("GPL");

static LIST_HEAD(wmi_block_list);

struct guid_block {
	guid_t guid;
	union {
		char object_id[2];
		struct {
			unsigned char notify_id;
			unsigned char reserved;
		};
	};
	u8 instance_count;
	u8 flags;
} __packed;
static_assert(sizeof(typeof_member(struct guid_block, guid)) == 16);
static_assert(sizeof(struct guid_block) == 20);
static_assert(__alignof__(struct guid_block) == 1);

enum {	/* wmi_block flags */
	WMI_READ_TAKES_NO_ARGS,
	WMI_PROBED,
};

struct wmi_block {
	struct wmi_device dev;
	struct list_head list;
	struct guid_block gblock;
	struct miscdevice char_dev;
	struct mutex char_mutex;
	struct acpi_device *acpi_device;
	wmi_notify_handler handler;
	void *handler_data;
	u64 req_buf_size;
	unsigned long flags;
};


/*
 * If the GUID data block is marked as expensive, we must enable and
 * explicitily disable data collection.
 */
#define ACPI_WMI_EXPENSIVE   BIT(0)
#define ACPI_WMI_METHOD      BIT(1)	/* GUID is a method */
#define ACPI_WMI_STRING      BIT(2)	/* GUID takes & returns a string */
#define ACPI_WMI_EVENT       BIT(3)	/* GUID is an event */

static bool debug_event;
module_param(debug_event, bool, 0444);
MODULE_PARM_DESC(debug_event,
		 "Log WMI Events [0/1]");

static bool debug_dump_wdg;
module_param(debug_dump_wdg, bool, 0444);
MODULE_PARM_DESC(debug_dump_wdg,
		 "Dump available WMI interfaces [0/1]");

static const struct acpi_device_id wmi_device_ids[] = {
	{"PNP0C14", 0},
	{"pnp0c14", 0},
	{ }
};
MODULE_DEVICE_TABLE(acpi, wmi_device_ids);

/* allow duplicate GUIDs as these device drivers use struct wmi_driver */
static const char * const allow_duplicates[] = {
	"05901221-D566-11D1-B2F0-00A0C9062910",	/* wmi-bmof */
	NULL
};

/*
 * GUID parsing functions
 */

static acpi_status find_guid(const char *guid_string, struct wmi_block **out)
{
	guid_t guid_input;
	struct wmi_block *wblock;

	if (!guid_string)
		return AE_BAD_PARAMETER;

	if (guid_parse(guid_string, &guid_input))
		return AE_BAD_PARAMETER;

	list_for_each_entry(wblock, &wmi_block_list, list) {
		if (guid_equal(&wblock->gblock.guid, &guid_input)) {
			if (out)
				*out = wblock;

			return AE_OK;
		}
	}

	return AE_NOT_FOUND;
}

static bool guid_parse_and_compare(const char *string, const guid_t *guid)
{
	guid_t guid_input;

	if (guid_parse(string, &guid_input))
		return false;

	return guid_equal(&guid_input, guid);
}

static const void *find_guid_context(struct wmi_block *wblock,
				     struct wmi_driver *wdriver)
{
	const struct wmi_device_id *id;

	id = wdriver->id_table;
	if (!id)
		return NULL;

	while (*id->guid_string) {
		if (guid_parse_and_compare(id->guid_string, &wblock->gblock.guid))
			return id->context;
		id++;
	}
	return NULL;
}

static int get_subobj_info(acpi_handle handle, const char *pathname,
			   struct acpi_device_info **info)
{
	struct acpi_device_info *dummy_info, **info_ptr;
	acpi_handle subobj_handle;
	acpi_status status;

	status = acpi_get_handle(handle, (char *)pathname, &subobj_handle);
	if (status == AE_NOT_FOUND)
		return -ENOENT;
	else if (ACPI_FAILURE(status))
		return -EIO;

	info_ptr = info ? info : &dummy_info;
	status = acpi_get_object_info(subobj_handle, info_ptr);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (!info)
		kfree(dummy_info);

	return 0;
}

static acpi_status wmi_method_enable(struct wmi_block *wblock, bool enable)
{
	struct guid_block *block;
	char method[5];
	acpi_status status;
	acpi_handle handle;

	block = &wblock->gblock;
	handle = wblock->acpi_device->handle;

	snprintf(method, 5, "WE%02X", block->notify_id);
	status = acpi_execute_simple_method(handle, method, enable);
	if (status == AE_NOT_FOUND)
		return AE_OK;

	return status;
}

#define WMI_ACPI_METHOD_NAME_SIZE 5

static inline void get_acpi_method_name(const struct wmi_block *wblock,
					const char method,
					char buffer[static WMI_ACPI_METHOD_NAME_SIZE])
{
	static_assert(ARRAY_SIZE(wblock->gblock.object_id) == 2);
	static_assert(WMI_ACPI_METHOD_NAME_SIZE >= 5);

	buffer[0] = 'W';
	buffer[1] = method;
	buffer[2] = wblock->gblock.object_id[0];
	buffer[3] = wblock->gblock.object_id[1];
	buffer[4] = '\0';
}

static inline acpi_object_type get_param_acpi_type(const struct wmi_block *wblock)
{
	if (wblock->gblock.flags & ACPI_WMI_STRING)
		return ACPI_TYPE_STRING;
	else
		return ACPI_TYPE_BUFFER;
}

static acpi_status get_event_data(const struct wmi_block *wblock, struct acpi_buffer *out)
{
	union acpi_object param = {
		.integer = {
			.type = ACPI_TYPE_INTEGER,
			.value = wblock->gblock.notify_id,
		}
	};
	struct acpi_object_list input = {
		.count = 1,
		.pointer = &param,
	};

	return acpi_evaluate_object(wblock->acpi_device->handle, "_WED", &input, out);
}

/*
 * Exported WMI functions
 */

/**
 * set_required_buffer_size - Sets the buffer size needed for performing IOCTL
 * @wdev: A wmi bus device from a driver
 * @length: Required buffer size
 *
 * Allocates memory needed for buffer, stores the buffer size in that memory
 */
int set_required_buffer_size(struct wmi_device *wdev, u64 length)
{
	struct wmi_block *wblock;

	wblock = container_of(wdev, struct wmi_block, dev);
	wblock->req_buf_size = length;

	return 0;
}
EXPORT_SYMBOL_GPL(set_required_buffer_size);

/**
 * wmi_evaluate_method - Evaluate a WMI method
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @instance: Instance index
 * @method_id: Method ID to call
 * @in: Buffer containing input for the method call
 * @out: Empty buffer to return the method results
 *
 * Call an ACPI-WMI method
 */
acpi_status wmi_evaluate_method(const char *guid_string, u8 instance, u32 method_id,
				const struct acpi_buffer *in, struct acpi_buffer *out)
{
	struct wmi_block *wblock = NULL;
	acpi_status status;

	status = find_guid(guid_string, &wblock);
	if (ACPI_FAILURE(status))
		return status;

	return wmidev_evaluate_method(&wblock->dev, instance, method_id,
				      in, out);
}
EXPORT_SYMBOL_GPL(wmi_evaluate_method);

/**
 * wmidev_evaluate_method - Evaluate a WMI method
 * @wdev: A wmi bus device from a driver
 * @instance: Instance index
 * @method_id: Method ID to call
 * @in: Buffer containing input for the method call
 * @out: Empty buffer to return the method results
 *
 * Call an ACPI-WMI method
 */
acpi_status wmidev_evaluate_method(struct wmi_device *wdev, u8 instance, u32 method_id,
				   const struct acpi_buffer *in, struct acpi_buffer *out)
{
	struct guid_block *block;
	struct wmi_block *wblock;
	acpi_handle handle;
	struct acpi_object_list input;
	union acpi_object params[3];
	char method[WMI_ACPI_METHOD_NAME_SIZE];

	wblock = container_of(wdev, struct wmi_block, dev);
	block = &wblock->gblock;
	handle = wblock->acpi_device->handle;

	if (!(block->flags & ACPI_WMI_METHOD))
		return AE_BAD_DATA;

	if (block->instance_count <= instance)
		return AE_BAD_PARAMETER;

	input.count = 2;
	input.pointer = params;
	params[0].type = ACPI_TYPE_INTEGER;
	params[0].integer.value = instance;
	params[1].type = ACPI_TYPE_INTEGER;
	params[1].integer.value = method_id;

	if (in) {
		input.count = 3;

		params[2].type = get_param_acpi_type(wblock);
		params[2].buffer.length = in->length;
		params[2].buffer.pointer = in->pointer;
	}

	get_acpi_method_name(wblock, 'M', method);

	return acpi_evaluate_object(handle, method, &input, out);
}
EXPORT_SYMBOL_GPL(wmidev_evaluate_method);

static acpi_status __query_block(struct wmi_block *wblock, u8 instance,
				 struct acpi_buffer *out)
{
	struct guid_block *block;
	acpi_handle handle;
	acpi_status status, wc_status = AE_ERROR;
	struct acpi_object_list input;
	union acpi_object wq_params[1];
	char wc_method[WMI_ACPI_METHOD_NAME_SIZE];
	char method[WMI_ACPI_METHOD_NAME_SIZE];

	if (!out)
		return AE_BAD_PARAMETER;

	block = &wblock->gblock;
	handle = wblock->acpi_device->handle;

	if (block->instance_count <= instance)
		return AE_BAD_PARAMETER;

	/* Check GUID is a data block */
	if (block->flags & (ACPI_WMI_EVENT | ACPI_WMI_METHOD))
		return AE_ERROR;

	input.count = 1;
	input.pointer = wq_params;
	wq_params[0].type = ACPI_TYPE_INTEGER;
	wq_params[0].integer.value = instance;

	if (instance == 0 && test_bit(WMI_READ_TAKES_NO_ARGS, &wblock->flags))
		input.count = 0;

	/*
	 * If ACPI_WMI_EXPENSIVE, call the relevant WCxx method first to
	 * enable collection.
	 */
	if (block->flags & ACPI_WMI_EXPENSIVE) {
		get_acpi_method_name(wblock, 'C', wc_method);

		/*
		 * Some GUIDs break the specification by declaring themselves
		 * expensive, but have no corresponding WCxx method. So we
		 * should not fail if this happens.
		 */
		wc_status = acpi_execute_simple_method(handle, wc_method, 1);
	}

	get_acpi_method_name(wblock, 'Q', method);
	status = acpi_evaluate_object(handle, method, &input, out);

	/*
	 * If ACPI_WMI_EXPENSIVE, call the relevant WCxx method, even if
	 * the WQxx method failed - we should disable collection anyway.
	 */
	if ((block->flags & ACPI_WMI_EXPENSIVE) && ACPI_SUCCESS(wc_status)) {
		/*
		 * Ignore whether this WCxx call succeeds or not since
		 * the previously executed WQxx method call might have
		 * succeeded, and returning the failing status code
		 * of this call would throw away the result of the WQxx
		 * call, potentially leaking memory.
		 */
		acpi_execute_simple_method(handle, wc_method, 0);
	}

	return status;
}

/**
 * wmi_query_block - Return contents of a WMI block (deprecated)
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @instance: Instance index
 * @out: Empty buffer to return the contents of the data block to
 *
 * Return the contents of an ACPI-WMI data block to a buffer
 */
acpi_status wmi_query_block(const char *guid_string, u8 instance,
			    struct acpi_buffer *out)
{
	struct wmi_block *wblock;
	acpi_status status;

	status = find_guid(guid_string, &wblock);
	if (ACPI_FAILURE(status))
		return status;

	return __query_block(wblock, instance, out);
}
EXPORT_SYMBOL_GPL(wmi_query_block);

union acpi_object *wmidev_block_query(struct wmi_device *wdev, u8 instance)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct wmi_block *wblock = container_of(wdev, struct wmi_block, dev);

	if (ACPI_FAILURE(__query_block(wblock, instance, &out)))
		return NULL;

	return out.pointer;
}
EXPORT_SYMBOL_GPL(wmidev_block_query);

/**
 * wmi_set_block - Write to a WMI block
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @instance: Instance index
 * @in: Buffer containing new values for the data block
 *
 * Write the contents of the input buffer to an ACPI-WMI data block
 */
acpi_status wmi_set_block(const char *guid_string, u8 instance,
			  const struct acpi_buffer *in)
{
	struct wmi_block *wblock = NULL;
	struct guid_block *block;
	acpi_handle handle;
	struct acpi_object_list input;
	union acpi_object params[2];
	char method[WMI_ACPI_METHOD_NAME_SIZE];
	acpi_status status;

	if (!in)
		return AE_BAD_DATA;

	status = find_guid(guid_string, &wblock);
	if (ACPI_FAILURE(status))
		return status;

	block = &wblock->gblock;
	handle = wblock->acpi_device->handle;

	if (block->instance_count <= instance)
		return AE_BAD_PARAMETER;

	/* Check GUID is a data block */
	if (block->flags & (ACPI_WMI_EVENT | ACPI_WMI_METHOD))
		return AE_ERROR;

	input.count = 2;
	input.pointer = params;
	params[0].type = ACPI_TYPE_INTEGER;
	params[0].integer.value = instance;
	params[1].type = get_param_acpi_type(wblock);
	params[1].buffer.length = in->length;
	params[1].buffer.pointer = in->pointer;

	get_acpi_method_name(wblock, 'S', method);

	return acpi_evaluate_object(handle, method, &input, NULL);
}
EXPORT_SYMBOL_GPL(wmi_set_block);

static void wmi_dump_wdg(const struct guid_block *g)
{
	pr_info("%pUL:\n", &g->guid);
	if (g->flags & ACPI_WMI_EVENT)
		pr_info("\tnotify_id: 0x%02X\n", g->notify_id);
	else
		pr_info("\tobject_id: %2pE\n", g->object_id);
	pr_info("\tinstance_count: %d\n", g->instance_count);
	pr_info("\tflags: %#x", g->flags);
	if (g->flags) {
		if (g->flags & ACPI_WMI_EXPENSIVE)
			pr_cont(" ACPI_WMI_EXPENSIVE");
		if (g->flags & ACPI_WMI_METHOD)
			pr_cont(" ACPI_WMI_METHOD");
		if (g->flags & ACPI_WMI_STRING)
			pr_cont(" ACPI_WMI_STRING");
		if (g->flags & ACPI_WMI_EVENT)
			pr_cont(" ACPI_WMI_EVENT");
	}
	pr_cont("\n");

}

static void wmi_notify_debug(u32 value, void *context)
{
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmi_get_event_data(value, &response);
	if (status != AE_OK) {
		pr_info("bad event status 0x%x\n", status);
		return;
	}

	obj = response.pointer;
	if (!obj)
		return;

	pr_info("DEBUG: event 0x%02X ", value);
	switch (obj->type) {
	case ACPI_TYPE_BUFFER:
		pr_cont("BUFFER_TYPE - length %u\n", obj->buffer.length);
		break;
	case ACPI_TYPE_STRING:
		pr_cont("STRING_TYPE - %s\n", obj->string.pointer);
		break;
	case ACPI_TYPE_INTEGER:
		pr_cont("INTEGER_TYPE - %llu\n", obj->integer.value);
		break;
	case ACPI_TYPE_PACKAGE:
		pr_cont("PACKAGE_TYPE - %u elements\n", obj->package.count);
		break;
	default:
		pr_cont("object type 0x%X\n", obj->type);
	}
	kfree(obj);
}

/**
 * wmi_install_notify_handler - Register handler for WMI events
 * @guid: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 * @handler: Function to handle notifications
 * @data: Data to be returned to handler when event is fired
 *
 * Register a handler for events sent to the ACPI-WMI mapper device.
 */
acpi_status wmi_install_notify_handler(const char *guid,
				       wmi_notify_handler handler,
				       void *data)
{
	struct wmi_block *block;
	acpi_status status = AE_NOT_EXIST;
	guid_t guid_input;

	if (!guid || !handler)
		return AE_BAD_PARAMETER;

	if (guid_parse(guid, &guid_input))
		return AE_BAD_PARAMETER;

	list_for_each_entry(block, &wmi_block_list, list) {
		acpi_status wmi_status;

		if (guid_equal(&block->gblock.guid, &guid_input)) {
			if (block->handler &&
			    block->handler != wmi_notify_debug)
				return AE_ALREADY_ACQUIRED;

			block->handler = handler;
			block->handler_data = data;

			wmi_status = wmi_method_enable(block, true);
			if ((wmi_status != AE_OK) ||
			    ((wmi_status == AE_OK) && (status == AE_NOT_EXIST)))
				status = wmi_status;
		}
	}

	return status;
}
EXPORT_SYMBOL_GPL(wmi_install_notify_handler);

/**
 * wmi_remove_notify_handler - Unregister handler for WMI events
 * @guid: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 *
 * Unregister handler for events sent to the ACPI-WMI mapper device.
 */
acpi_status wmi_remove_notify_handler(const char *guid)
{
	struct wmi_block *block;
	acpi_status status = AE_NOT_EXIST;
	guid_t guid_input;

	if (!guid)
		return AE_BAD_PARAMETER;

	if (guid_parse(guid, &guid_input))
		return AE_BAD_PARAMETER;

	list_for_each_entry(block, &wmi_block_list, list) {
		acpi_status wmi_status;

		if (guid_equal(&block->gblock.guid, &guid_input)) {
			if (!block->handler ||
			    block->handler == wmi_notify_debug)
				return AE_NULL_ENTRY;

			if (debug_event) {
				block->handler = wmi_notify_debug;
				status = AE_OK;
			} else {
				wmi_status = wmi_method_enable(block, false);
				block->handler = NULL;
				block->handler_data = NULL;
				if ((wmi_status != AE_OK) ||
				    ((wmi_status == AE_OK) &&
				     (status == AE_NOT_EXIST)))
					status = wmi_status;
			}
		}
	}

	return status;
}
EXPORT_SYMBOL_GPL(wmi_remove_notify_handler);

/**
 * wmi_get_event_data - Get WMI data associated with an event
 *
 * @event: Event to find
 * @out: Buffer to hold event data. out->pointer should be freed with kfree()
 *
 * Returns extra data associated with an event in WMI.
 */
acpi_status wmi_get_event_data(u32 event, struct acpi_buffer *out)
{
	struct wmi_block *wblock;

	list_for_each_entry(wblock, &wmi_block_list, list) {
		struct guid_block *gblock = &wblock->gblock;

		if ((gblock->flags & ACPI_WMI_EVENT) && gblock->notify_id == event)
			return get_event_data(wblock, out);
	}

	return AE_NOT_FOUND;
}
EXPORT_SYMBOL_GPL(wmi_get_event_data);

/**
 * wmi_has_guid - Check if a GUID is available
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 *
 * Check if a given GUID is defined by _WDG
 */
bool wmi_has_guid(const char *guid_string)
{
	return ACPI_SUCCESS(find_guid(guid_string, NULL));
}
EXPORT_SYMBOL_GPL(wmi_has_guid);

/**
 * wmi_get_acpi_device_uid() - Get _UID name of ACPI device that defines GUID
 * @guid_string: 36 char string of the form fa50ff2b-f2e8-45de-83fa-65417f2f49ba
 *
 * Find the _UID of ACPI device associated with this WMI GUID.
 *
 * Return: The ACPI _UID field value or NULL if the WMI GUID was not found
 */
char *wmi_get_acpi_device_uid(const char *guid_string)
{
	struct wmi_block *wblock = NULL;
	acpi_status status;

	status = find_guid(guid_string, &wblock);
	if (ACPI_FAILURE(status))
		return NULL;

	return acpi_device_uid(wblock->acpi_device);
}
EXPORT_SYMBOL_GPL(wmi_get_acpi_device_uid);

#define dev_to_wblock(__dev)	container_of_const(__dev, struct wmi_block, dev.dev)
#define dev_to_wdev(__dev)	container_of_const(__dev, struct wmi_device, dev)

static inline struct wmi_driver *drv_to_wdrv(struct device_driver *drv)
{
	return container_of(drv, struct wmi_driver, driver);
}

/*
 * sysfs interface
 */
static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct wmi_block *wblock = dev_to_wblock(dev);

	return sysfs_emit(buf, "wmi:%pUL\n", &wblock->gblock.guid);
}
static DEVICE_ATTR_RO(modalias);

static ssize_t guid_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct wmi_block *wblock = dev_to_wblock(dev);

	return sysfs_emit(buf, "%pUL\n", &wblock->gblock.guid);
}
static DEVICE_ATTR_RO(guid);

static ssize_t instance_count_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct wmi_block *wblock = dev_to_wblock(dev);

	return sysfs_emit(buf, "%d\n", (int)wblock->gblock.instance_count);
}
static DEVICE_ATTR_RO(instance_count);

static ssize_t expensive_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct wmi_block *wblock = dev_to_wblock(dev);

	return sysfs_emit(buf, "%d\n",
			  (wblock->gblock.flags & ACPI_WMI_EXPENSIVE) != 0);
}
static DEVICE_ATTR_RO(expensive);

static struct attribute *wmi_attrs[] = {
	&dev_attr_modalias.attr,
	&dev_attr_guid.attr,
	&dev_attr_instance_count.attr,
	&dev_attr_expensive.attr,
	NULL
};
ATTRIBUTE_GROUPS(wmi);

static ssize_t notify_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct wmi_block *wblock = dev_to_wblock(dev);

	return sysfs_emit(buf, "%02X\n", (unsigned int)wblock->gblock.notify_id);
}
static DEVICE_ATTR_RO(notify_id);

static struct attribute *wmi_event_attrs[] = {
	&dev_attr_notify_id.attr,
	NULL
};
ATTRIBUTE_GROUPS(wmi_event);

static ssize_t object_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct wmi_block *wblock = dev_to_wblock(dev);

	return sysfs_emit(buf, "%c%c\n", wblock->gblock.object_id[0],
			  wblock->gblock.object_id[1]);
}
static DEVICE_ATTR_RO(object_id);

static ssize_t setable_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct wmi_device *wdev = dev_to_wdev(dev);

	return sysfs_emit(buf, "%d\n", (int)wdev->setable);
}
static DEVICE_ATTR_RO(setable);

static struct attribute *wmi_data_attrs[] = {
	&dev_attr_object_id.attr,
	&dev_attr_setable.attr,
	NULL
};
ATTRIBUTE_GROUPS(wmi_data);

static struct attribute *wmi_method_attrs[] = {
	&dev_attr_object_id.attr,
	NULL
};
ATTRIBUTE_GROUPS(wmi_method);

static int wmi_dev_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct wmi_block *wblock = dev_to_wblock(dev);

	if (add_uevent_var(env, "MODALIAS=wmi:%pUL", &wblock->gblock.guid))
		return -ENOMEM;

	if (add_uevent_var(env, "WMI_GUID=%pUL", &wblock->gblock.guid))
		return -ENOMEM;

	return 0;
}

static void wmi_dev_release(struct device *dev)
{
	struct wmi_block *wblock = dev_to_wblock(dev);

	kfree(wblock);
}

static int wmi_dev_match(struct device *dev, struct device_driver *driver)
{
	struct wmi_driver *wmi_driver = drv_to_wdrv(driver);
	struct wmi_block *wblock = dev_to_wblock(dev);
	const struct wmi_device_id *id = wmi_driver->id_table;

	if (id == NULL)
		return 0;

	while (*id->guid_string) {
		if (guid_parse_and_compare(id->guid_string, &wblock->gblock.guid))
			return 1;

		id++;
	}

	return 0;
}
static int wmi_char_open(struct inode *inode, struct file *filp)
{
	/*
	 * The miscdevice already stores a pointer to itself
	 * inside filp->private_data
	 */
	struct wmi_block *wblock = container_of(filp->private_data, struct wmi_block, char_dev);

	filp->private_data = wblock;

	return nonseekable_open(inode, filp);
}

static ssize_t wmi_char_read(struct file *filp, char __user *buffer,
			     size_t length, loff_t *offset)
{
	struct wmi_block *wblock = filp->private_data;

	return simple_read_from_buffer(buffer, length, offset,
				       &wblock->req_buf_size,
				       sizeof(wblock->req_buf_size));
}

static long wmi_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct wmi_ioctl_buffer __user *input =
		(struct wmi_ioctl_buffer __user *) arg;
	struct wmi_block *wblock = filp->private_data;
	struct wmi_ioctl_buffer *buf;
	struct wmi_driver *wdriver;
	int ret;

	if (_IOC_TYPE(cmd) != WMI_IOC)
		return -ENOTTY;

	/* make sure we're not calling a higher instance than exists*/
	if (_IOC_NR(cmd) >= wblock->gblock.instance_count)
		return -EINVAL;

	mutex_lock(&wblock->char_mutex);
	buf = wblock->handler_data;
	if (get_user(buf->length, &input->length)) {
		dev_dbg(&wblock->dev.dev, "Read length from user failed\n");
		ret = -EFAULT;
		goto out_ioctl;
	}
	/* if it's too small, abort */
	if (buf->length < wblock->req_buf_size) {
		dev_err(&wblock->dev.dev,
			"Buffer %lld too small, need at least %lld\n",
			buf->length, wblock->req_buf_size);
		ret = -EINVAL;
		goto out_ioctl;
	}
	/* if it's too big, warn, driver will only use what is needed */
	if (buf->length > wblock->req_buf_size)
		dev_warn(&wblock->dev.dev,
			"Buffer %lld is bigger than required %lld\n",
			buf->length, wblock->req_buf_size);

	/* copy the structure from userspace */
	if (copy_from_user(buf, input, wblock->req_buf_size)) {
		dev_dbg(&wblock->dev.dev, "Copy %llu from user failed\n",
			wblock->req_buf_size);
		ret = -EFAULT;
		goto out_ioctl;
	}

	/* let the driver do any filtering and do the call */
	wdriver = drv_to_wdrv(wblock->dev.dev.driver);
	if (!try_module_get(wdriver->driver.owner)) {
		ret = -EBUSY;
		goto out_ioctl;
	}
	ret = wdriver->filter_callback(&wblock->dev, cmd, buf);
	module_put(wdriver->driver.owner);
	if (ret)
		goto out_ioctl;

	/* return the result (only up to our internal buffer size) */
	if (copy_to_user(input, buf, wblock->req_buf_size)) {
		dev_dbg(&wblock->dev.dev, "Copy %llu to user failed\n",
			wblock->req_buf_size);
		ret = -EFAULT;
	}

out_ioctl:
	mutex_unlock(&wblock->char_mutex);
	return ret;
}

static const struct file_operations wmi_fops = {
	.owner		= THIS_MODULE,
	.read		= wmi_char_read,
	.open		= wmi_char_open,
	.unlocked_ioctl	= wmi_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static int wmi_dev_probe(struct device *dev)
{
	struct wmi_block *wblock = dev_to_wblock(dev);
	struct wmi_driver *wdriver = drv_to_wdrv(dev->driver);
	int ret = 0;
	char *buf;

	if (ACPI_FAILURE(wmi_method_enable(wblock, true)))
		dev_warn(dev, "failed to enable device -- probing anyway\n");

	if (wdriver->probe) {
		ret = wdriver->probe(dev_to_wdev(dev),
				find_guid_context(wblock, wdriver));
		if (ret != 0)
			goto probe_failure;
	}

	/* driver wants a character device made */
	if (wdriver->filter_callback) {
		/* check that required buffer size declared by driver or MOF */
		if (!wblock->req_buf_size) {
			dev_err(&wblock->dev.dev,
				"Required buffer size not set\n");
			ret = -EINVAL;
			goto probe_failure;
		}

		wblock->handler_data = kmalloc(wblock->req_buf_size,
					       GFP_KERNEL);
		if (!wblock->handler_data) {
			ret = -ENOMEM;
			goto probe_failure;
		}

		buf = kasprintf(GFP_KERNEL, "wmi/%s", wdriver->driver.name);
		if (!buf) {
			ret = -ENOMEM;
			goto probe_string_failure;
		}
		wblock->char_dev.minor = MISC_DYNAMIC_MINOR;
		wblock->char_dev.name = buf;
		wblock->char_dev.fops = &wmi_fops;
		wblock->char_dev.mode = 0444;
		ret = misc_register(&wblock->char_dev);
		if (ret) {
			dev_warn(dev, "failed to register char dev: %d\n", ret);
			ret = -ENOMEM;
			goto probe_misc_failure;
		}
	}

	set_bit(WMI_PROBED, &wblock->flags);
	return 0;

probe_misc_failure:
	kfree(buf);
probe_string_failure:
	kfree(wblock->handler_data);
probe_failure:
	if (ACPI_FAILURE(wmi_method_enable(wblock, false)))
		dev_warn(dev, "failed to disable device\n");
	return ret;
}

static void wmi_dev_remove(struct device *dev)
{
	struct wmi_block *wblock = dev_to_wblock(dev);
	struct wmi_driver *wdriver = drv_to_wdrv(dev->driver);

	clear_bit(WMI_PROBED, &wblock->flags);

	if (wdriver->filter_callback) {
		misc_deregister(&wblock->char_dev);
		kfree(wblock->char_dev.name);
		kfree(wblock->handler_data);
	}

	if (wdriver->remove)
		wdriver->remove(dev_to_wdev(dev));

	if (ACPI_FAILURE(wmi_method_enable(wblock, false)))
		dev_warn(dev, "failed to disable device\n");
}

static struct class wmi_bus_class = {
	.name = "wmi_bus",
};

static struct bus_type wmi_bus_type = {
	.name = "wmi",
	.dev_groups = wmi_groups,
	.match = wmi_dev_match,
	.uevent = wmi_dev_uevent,
	.probe = wmi_dev_probe,
	.remove = wmi_dev_remove,
};

static const struct device_type wmi_type_event = {
	.name = "event",
	.groups = wmi_event_groups,
	.release = wmi_dev_release,
};

static const struct device_type wmi_type_method = {
	.name = "method",
	.groups = wmi_method_groups,
	.release = wmi_dev_release,
};

static const struct device_type wmi_type_data = {
	.name = "data",
	.groups = wmi_data_groups,
	.release = wmi_dev_release,
};

/*
 * _WDG is a static list that is only parsed at startup,
 * so it's safe to count entries without extra protection.
 */
static int guid_count(const guid_t *guid)
{
	struct wmi_block *wblock;
	int count = 0;

	list_for_each_entry(wblock, &wmi_block_list, list) {
		if (guid_equal(&wblock->gblock.guid, guid))
			count++;
	}

	return count;
}

static int wmi_create_device(struct device *wmi_bus_dev,
			     struct wmi_block *wblock,
			     struct acpi_device *device)
{
	struct acpi_device_info *info;
	char method[WMI_ACPI_METHOD_NAME_SIZE];
	int result;
	uint count;

	if (wblock->gblock.flags & ACPI_WMI_EVENT) {
		wblock->dev.dev.type = &wmi_type_event;
		goto out_init;
	}

	if (wblock->gblock.flags & ACPI_WMI_METHOD) {
		wblock->dev.dev.type = &wmi_type_method;
		mutex_init(&wblock->char_mutex);
		goto out_init;
	}

	/*
	 * Data Block Query Control Method (WQxx by convention) is
	 * required per the WMI documentation. If it is not present,
	 * we ignore this data block.
	 */
	get_acpi_method_name(wblock, 'Q', method);
	result = get_subobj_info(device->handle, method, &info);

	if (result) {
		dev_warn(wmi_bus_dev,
			 "%s data block query control method not found\n",
			 method);
		return result;
	}

	wblock->dev.dev.type = &wmi_type_data;

	/*
	 * The Microsoft documentation specifically states:
	 *
	 *   Data blocks registered with only a single instance
	 *   can ignore the parameter.
	 *
	 * ACPICA will get mad at us if we call the method with the wrong number
	 * of arguments, so check what our method expects.  (On some Dell
	 * laptops, WQxx may not be a method at all.)
	 */
	if (info->type != ACPI_TYPE_METHOD || info->param_count == 0)
		set_bit(WMI_READ_TAKES_NO_ARGS, &wblock->flags);

	kfree(info);

	get_acpi_method_name(wblock, 'S', method);
	result = get_subobj_info(device->handle, method, NULL);

	if (result == 0)
		wblock->dev.setable = true;

 out_init:
	wblock->dev.dev.bus = &wmi_bus_type;
	wblock->dev.dev.parent = wmi_bus_dev;

	count = guid_count(&wblock->gblock.guid);
	if (count)
		dev_set_name(&wblock->dev.dev, "%pUL-%d", &wblock->gblock.guid, count);
	else
		dev_set_name(&wblock->dev.dev, "%pUL", &wblock->gblock.guid);

	device_initialize(&wblock->dev.dev);

	return 0;
}

static void wmi_free_devices(struct acpi_device *device)
{
	struct wmi_block *wblock, *next;

	/* Delete devices for all the GUIDs */
	list_for_each_entry_safe(wblock, next, &wmi_block_list, list) {
		if (wblock->acpi_device == device) {
			list_del(&wblock->list);
			device_unregister(&wblock->dev.dev);
		}
	}
}

static bool guid_already_parsed_for_legacy(struct acpi_device *device, const guid_t *guid)
{
	struct wmi_block *wblock;

	list_for_each_entry(wblock, &wmi_block_list, list) {
		/* skip warning and register if we know the driver will use struct wmi_driver */
		for (int i = 0; allow_duplicates[i] != NULL; i++) {
			guid_t tmp;

			if (guid_parse(allow_duplicates[i], &tmp))
				continue;
			if (guid_equal(&tmp, guid))
				return false;
		}
		if (guid_equal(&wblock->gblock.guid, guid)) {
			/*
			 * Because we historically didn't track the relationship
			 * between GUIDs and ACPI nodes, we don't know whether
			 * we need to suppress GUIDs that are unique on a
			 * given node but duplicated across nodes.
			 */
			dev_warn(&device->dev, "duplicate WMI GUID %pUL (first instance was on %s)\n",
				 guid, dev_name(&wblock->acpi_device->dev));
			return true;
		}
	}

	return false;
}

/*
 * Parse the _WDG method for the GUID data blocks
 */
static int parse_wdg(struct device *wmi_bus_dev, struct acpi_device *device)
{
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	const struct guid_block *gblock;
	struct wmi_block *wblock, *next;
	union acpi_object *obj;
	acpi_status status;
	u32 i, total;
	int retval;

	status = acpi_evaluate_object(device->handle, "_WDG", NULL, &out);
	if (ACPI_FAILURE(status))
		return -ENXIO;

	obj = out.pointer;
	if (!obj)
		return -ENXIO;

	if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return -ENXIO;
	}

	gblock = (const struct guid_block *)obj->buffer.pointer;
	total = obj->buffer.length / sizeof(struct guid_block);

	for (i = 0; i < total; i++) {
		if (debug_dump_wdg)
			wmi_dump_wdg(&gblock[i]);

		if (!gblock[i].instance_count) {
			dev_info(wmi_bus_dev, FW_INFO "%pUL has zero instances\n", &gblock[i].guid);
			continue;
		}

		if (guid_already_parsed_for_legacy(device, &gblock[i].guid))
			continue;

		wblock = kzalloc(sizeof(*wblock), GFP_KERNEL);
		if (!wblock) {
			dev_err(wmi_bus_dev, "Failed to allocate %pUL\n", &gblock[i].guid);
			continue;
		}

		wblock->acpi_device = device;
		wblock->gblock = gblock[i];

		retval = wmi_create_device(wmi_bus_dev, wblock, device);
		if (retval) {
			kfree(wblock);
			continue;
		}

		list_add_tail(&wblock->list, &wmi_block_list);

		if (debug_event) {
			wblock->handler = wmi_notify_debug;
			wmi_method_enable(wblock, true);
		}
	}

	/*
	 * Now that all of the devices are created, add them to the
	 * device tree and probe subdrivers.
	 */
	list_for_each_entry_safe(wblock, next, &wmi_block_list, list) {
		if (wblock->acpi_device != device)
			continue;

		retval = device_add(&wblock->dev.dev);
		if (retval) {
			dev_err(wmi_bus_dev, "failed to register %pUL\n",
				&wblock->gblock.guid);
			if (debug_event)
				wmi_method_enable(wblock, false);
			list_del(&wblock->list);
			put_device(&wblock->dev.dev);
		}
	}

	kfree(obj);

	return 0;
}

/*
 * WMI can have EmbeddedControl access regions. In which case, we just want to
 * hand these off to the EC driver.
 */
static acpi_status
acpi_wmi_ec_space_handler(u32 function, acpi_physical_address address,
			  u32 bits, u64 *value,
			  void *handler_context, void *region_context)
{
	int result = 0, i = 0;
	u8 temp = 0;

	if ((address > 0xFF) || !value)
		return AE_BAD_PARAMETER;

	if (function != ACPI_READ && function != ACPI_WRITE)
		return AE_BAD_PARAMETER;

	if (bits != 8)
		return AE_BAD_PARAMETER;

	if (function == ACPI_READ) {
		result = ec_read(address, &temp);
		(*value) |= ((u64)temp) << i;
	} else {
		temp = 0xff & ((*value) >> i);
		result = ec_write(address, temp);
	}

	switch (result) {
	case -EINVAL:
		return AE_BAD_PARAMETER;
	case -ENODEV:
		return AE_NOT_FOUND;
	case -ETIME:
		return AE_TIME;
	default:
		return AE_OK;
	}
}

static void acpi_wmi_notify_handler(acpi_handle handle, u32 event,
				    void *context)
{
	struct wmi_block *wblock = NULL, *iter;

	list_for_each_entry(iter, &wmi_block_list, list) {
		struct guid_block *block = &iter->gblock;

		if (iter->acpi_device->handle == handle &&
		    (block->flags & ACPI_WMI_EVENT) &&
		    (block->notify_id == event)) {
			wblock = iter;
			break;
		}
	}

	if (!wblock)
		return;

	/* If a driver is bound, then notify the driver. */
	if (test_bit(WMI_PROBED, &wblock->flags) && wblock->dev.dev.driver) {
		struct wmi_driver *driver = drv_to_wdrv(wblock->dev.dev.driver);
		struct acpi_buffer evdata = { ACPI_ALLOCATE_BUFFER, NULL };
		acpi_status status;

		if (!driver->no_notify_data) {
			status = get_event_data(wblock, &evdata);
			if (ACPI_FAILURE(status)) {
				dev_warn(&wblock->dev.dev, "failed to get event data\n");
				return;
			}
		}

		if (driver->notify)
			driver->notify(&wblock->dev, evdata.pointer);

		kfree(evdata.pointer);
	} else if (wblock->handler) {
		/* Legacy handler */
		wblock->handler(event, wblock->handler_data);
	}

	if (debug_event)
		pr_info("DEBUG: GUID %pUL event 0x%02X\n", &wblock->gblock.guid, event);

	acpi_bus_generate_netlink_event(
		wblock->acpi_device->pnp.device_class,
		dev_name(&wblock->dev.dev),
		event, 0);
}

static int acpi_wmi_remove(struct platform_device *device)
{
	struct acpi_device *acpi_device = ACPI_COMPANION(&device->dev);

	acpi_remove_notify_handler(acpi_device->handle, ACPI_ALL_NOTIFY,
				   acpi_wmi_notify_handler);
	acpi_remove_address_space_handler(acpi_device->handle,
				ACPI_ADR_SPACE_EC, &acpi_wmi_ec_space_handler);
	wmi_free_devices(acpi_device);
	device_unregister(dev_get_drvdata(&device->dev));

	return 0;
}

static int acpi_wmi_probe(struct platform_device *device)
{
	struct acpi_device *acpi_device;
	struct device *wmi_bus_dev;
	acpi_status status;
	int error;

	acpi_device = ACPI_COMPANION(&device->dev);
	if (!acpi_device) {
		dev_err(&device->dev, "ACPI companion is missing\n");
		return -ENODEV;
	}

	status = acpi_install_address_space_handler(acpi_device->handle,
						    ACPI_ADR_SPACE_EC,
						    &acpi_wmi_ec_space_handler,
						    NULL, NULL);
	if (ACPI_FAILURE(status)) {
		dev_err(&device->dev, "Error installing EC region handler\n");
		return -ENODEV;
	}

	status = acpi_install_notify_handler(acpi_device->handle,
					     ACPI_ALL_NOTIFY,
					     acpi_wmi_notify_handler,
					     NULL);
	if (ACPI_FAILURE(status)) {
		dev_err(&device->dev, "Error installing notify handler\n");
		error = -ENODEV;
		goto err_remove_ec_handler;
	}

	wmi_bus_dev = device_create(&wmi_bus_class, &device->dev, MKDEV(0, 0),
				    NULL, "wmi_bus-%s", dev_name(&device->dev));
	if (IS_ERR(wmi_bus_dev)) {
		error = PTR_ERR(wmi_bus_dev);
		goto err_remove_notify_handler;
	}
	dev_set_drvdata(&device->dev, wmi_bus_dev);

	error = parse_wdg(wmi_bus_dev, acpi_device);
	if (error) {
		pr_err("Failed to parse WDG method\n");
		goto err_remove_busdev;
	}

	return 0;

err_remove_busdev:
	device_unregister(wmi_bus_dev);

err_remove_notify_handler:
	acpi_remove_notify_handler(acpi_device->handle, ACPI_ALL_NOTIFY,
				   acpi_wmi_notify_handler);

err_remove_ec_handler:
	acpi_remove_address_space_handler(acpi_device->handle,
					  ACPI_ADR_SPACE_EC,
					  &acpi_wmi_ec_space_handler);

	return error;
}

int __must_check __wmi_driver_register(struct wmi_driver *driver,
				       struct module *owner)
{
	driver->driver.owner = owner;
	driver->driver.bus = &wmi_bus_type;

	return driver_register(&driver->driver);
}
EXPORT_SYMBOL(__wmi_driver_register);

void wmi_driver_unregister(struct wmi_driver *driver)
{
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL(wmi_driver_unregister);

static struct platform_driver acpi_wmi_driver = {
	.driver = {
		.name = "acpi-wmi",
		.acpi_match_table = wmi_device_ids,
	},
	.probe = acpi_wmi_probe,
	.remove = acpi_wmi_remove,
};

static int __init acpi_wmi_init(void)
{
	int error;

	if (acpi_disabled)
		return -ENODEV;

	error = class_register(&wmi_bus_class);
	if (error)
		return error;

	error = bus_register(&wmi_bus_type);
	if (error)
		goto err_unreg_class;

	error = platform_driver_register(&acpi_wmi_driver);
	if (error) {
		pr_err("Error loading mapper\n");
		goto err_unreg_bus;
	}

	return 0;

err_unreg_bus:
	bus_unregister(&wmi_bus_type);

err_unreg_class:
	class_unregister(&wmi_bus_class);

	return error;
}

static void __exit acpi_wmi_exit(void)
{
	platform_driver_unregister(&acpi_wmi_driver);
	bus_unregister(&wmi_bus_type);
	class_unregister(&wmi_bus_class);
}

subsys_initcall_sync(acpi_wmi_init);
module_exit(acpi_wmi_exit);
