/*
 * Copyright (C) 2014 SCALITY SA - http://www.scality.com
 *
 * This file is part of ScalityRestBlock.
 *
 * ScalityRestBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ScalityRestBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ScalityRestBlock.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <srb/srb-log.h>

#include "srb.h"


/* Function for parsing params and reading humand readable size format
 *
 * Returns number of parameters found, and fills up to param_nb parameters.
 */
static int parse_params(char *params, const char *delim, char **param_tbl, int param_nb, int max)
{
	int i;
	int j;
	char *tmp;

	//printk(KERN_DEBUG "DEBUG: parse_params: params(%d): %s\n", max, params);

	j = 0;
	for (i = 0; i < max; i++) {
		tmp = strsep(&params, delim);
		if (NULL != tmp && *tmp != '\0') {
			if (j < param_nb)
				param_tbl[j] = tmp;
			j++;
		}
	}

	//printk(KERN_DEBUG "DEBUG: parse_params: params: %s, %s\n", param_tbl[0], param_tbl[1]);

	return j;
}

/********************************************************************
 * /sys/block/srb?/
 *                   srb_debug	 Sets verbosity
 *                   srb_urls	 Gets device CDMI url
 *                   srb_name   Gets device's on-storage filename
 *******************************************************************/
static ssize_t attr_debug_store(struct device *dv,
				struct device_attribute *attr,
				const char *buff, size_t count)
{
	struct gendisk *disk	  = dev_to_disk(dv);
	struct srb_device *dev = disk->private_data;
	long int val;
	int ret;

	ret = kstrtol(buff, 10, &val);
	if (ret < 0) {
		SRBDEV_LOG_WARN(dev, "Invalid debug value");
		return ret;
	}
	if ((int)val < 0 || (int)val > 7)
	{
		SRBDEV_LOG_WARN(dev, "Invalid debug value (%d) for device %s in sysfs",
				(int)val, srb_device_get_name(dev));
		return -EINVAL;
	}

        srb_device_set_debug_level(dev, (int)val);
	SRBDEV_LOG_DEBUG(dev, "Setting Log level to %d for device %s", (int)val, srb_device_get_name(dev));

	return count;
}

static ssize_t attr_debug_show(struct device *dv,
			struct device_attribute *attr, char *buff)
{
	struct gendisk *disk	  = dev_to_disk(dv);
	struct srb_device *dev = disk->private_data;
	
	return scnprintf(buff, PAGE_SIZE, "%d\n", srb_device_get_debug_level(dev));
}


static ssize_t attr_urls_show(struct device *dv,
			struct device_attribute *attr, char *buff)
{
	struct gendisk *disk	  = dev_to_disk(dv);
	struct srb_device *dev = disk->private_data;
	
	//snprintf(buff, PAGE_SIZE, "%s\n", dev->thread_cdmi_desc[0].url);
	return scnprintf(buff, PAGE_SIZE, "%s\n", srb_device_get_thread_cdmi_desc(dev, 0)->url);
}

static ssize_t attr_disk_name_show(struct device *dv,
				   struct device_attribute *attr, char *buff)
{
	struct gendisk *disk	  = dev_to_disk(dv);
	struct srb_device *dev = disk->private_data;

	//snprintf(buff, PAGE_SIZE, "%s\n", kbasename(dev->thread_cdmi_desc[0].url));
	return scnprintf(buff, PAGE_SIZE, "%s\n", kbasename(srb_device_get_thread_cdmi_desc(dev, 0)->url));
}

static DEVICE_ATTR(srb_debug, S_IWUSR | S_IRUGO, &attr_debug_show, &attr_debug_store);
static DEVICE_ATTR(srb_urls, S_IRUGO, &attr_urls_show, NULL);
static DEVICE_ATTR(srb_name, S_IRUGO, &attr_disk_name_show, NULL);

/************************************************************************
 * /sys/class/srb/
 *                   create     Create the volume's file on the storage
 *                   destroy    Removes the volume's file on the storage
 *                   attach	Attach a volume as a new srb device
 *                   detach	Detaches (remove from the system)
 *                              the requested volume (or device)
 ***********************************************************************/

static struct class *class_srb;		/* /sys/class/srb */


static void class_srb_release(struct class *cls)
{
	if (cls != NULL)
		kfree(cls);
}

static ssize_t class_srb_attach_show(struct class *c, struct class_attribute *attr,
				      char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "# Usage: echo VolumeName DeviceName > attach\n");
}

static ssize_t class_srb_attach_store(struct class *c,
			struct class_attribute *attr,
			const char *buf, size_t count)
{
	int ret;
	const char *delim = " ";
	char *tmp_buf = NULL;
	char *params[2];
	const char **filename = (const char **)&params[0];
	const char **devname = (const char **)&params[1];

	memset(params, 0, sizeof(params));

	tmp_buf = kmalloc(count + 1, GFP_KERNEL);
	if (NULL == tmp_buf) {
		SRB_LOG_ERR(srb_log, "Unable to allocate memory for parameters");
		ret = -ENOMEM;
		goto out;
	}
	memcpy(tmp_buf, buf, count + 1);

	/* remove CR or LF if any and end string */
	if (tmp_buf[count - 1] == '\n' || tmp_buf[count - 1] == '\r')
		tmp_buf[count - 1] = 0;
	else
		tmp_buf[count] = 0;

	ret = parse_params(tmp_buf, delim, params, 2, count);
	if (ret != 2) {
		SRB_LOG_ERR(srb_log, "Invalid parameters: %i instead of 2",
			     ret);
		ret = -EINVAL;
		goto out;
	}

	/* Sanity check params sizes */
	if (NULL == *filename || strlen(*filename) > SRB_CDMI_URL_SIZE) {
		SRB_LOG_ERR(srb_log, "Invalid parameter #1: "
			     "'%s'(%lu characters)", *filename,
			     strlen(*filename));
		ret = -EINVAL;
		goto out;
	}

	if (NULL == *devname || strlen(*devname) > DISK_NAME_LEN) {
		SRB_LOG_ERR(srb_log, "Invalid parameter #2: "
			     "'%s'(%lu characters)", *devname,
			     strlen(*devname));
		ret = -EINVAL;
		goto out;
	}

	SRB_LOG_INFO(srb_log, "Attaching volume '%s' as device '%s'",
		      *filename, *devname);
	ret = srb_device_attach(*filename, *devname);
	if (ret != 0)
		goto out;

	ret = count;
out:
	if (NULL != tmp_buf)
		kfree(tmp_buf);

	return ret;
}

static ssize_t class_srb_detach_show(struct class *c, struct class_attribute *attr,
				      char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "# Usage: echo DeviceName > detach\n");
}

static ssize_t class_srb_detach_store(struct class *c,
				struct class_attribute *attr,
				const char *buf,
				size_t count)
{
	int ret = -ENOENT;
	char devname[DISK_NAME_LEN + 1];

	/* Sanity check device name size */
	if ((count == 0) || (count >= sizeof(devname))) {
		SRB_LOG_ERR(srb_log, "Invalid parameter (too long: %lu)", count);
		return -EINVAL;
	}

	memcpy(devname, buf, count);
	if (devname[count - 1] == '\n')
		devname[count - 1] = 0;
	else
		devname[count] = 0;

	SRB_LOG_INFO(srb_log, "Detaching device %s", devname);
	ret = srb_device_detach(devname);
	if (ret == 0)
		return count;

	return ret;
}

static ssize_t class_srb_addurl_show(struct class *c, struct class_attribute *attr,
					 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "# Usage: echo server_url1,...,server_urlN > add_urls\n");
}

static ssize_t class_srb_addurl_store(struct class *c,
					  struct class_attribute *attr,
					  const char *buf,
					  size_t count)
{
	ssize_t		ret = 0;
	char		url[SRB_CDMI_URL_SIZE+1];
	const char	*tmp = buf;
	const char	*tmpend = tmp;
	int		errcount = 0;

	while (tmp != NULL) {
		while (*tmp != 0 && *tmp == ',')
			++tmp;

		tmpend = strchr(tmp, ',');
		if (tmpend != NULL) {
			memcpy(url, tmp, (tmpend - tmp));
			url[(tmpend - tmp)] = 0;
		}
		else {
			// Strip the ending newline
			tmpend = tmp;
			while (*tmpend && *tmpend != '\n')
				tmpend++;

			if ((tmpend - tmp) > SRB_CDMI_URL_SIZE) {
				SRB_LOG_ERR(srb_log, "Url too big: '%s'", tmp);
				ret = -EINVAL;
				goto end;
			}
			memcpy(url, tmp, (tmpend - tmp));
			url[(tmpend - tmp)] = 0;
			tmpend = NULL;
		}
		url[SRB_CDMI_URL_SIZE] = 0;

		ret = srb_server_add(url);
		if (ret < 0)
			errcount += 1;

		tmp = tmpend;
	}

	ret = count;
	if (errcount > 0) {
		SRB_LOG_ERR(srb_log, "Could not add every url to driver.");
		ret = -EINVAL;
	}

end:
	return ret;
}

static ssize_t class_srb_removeurl_show(struct class *c, struct class_attribute *attr,
					    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "# Usage: echo server_url1,...,server_urlN > remove_urls\n");
}

static ssize_t class_srb_removeurl_store(struct class *c,
					     struct class_attribute *attr,
					     const char *buf,
					     size_t count)
{
	ssize_t		ret = 0;
	char		url[SRB_CDMI_URL_SIZE];
	const char	*tmp = buf;
	const char	*tmpend = tmp;

	while (tmp != NULL)
	{
		while (*tmp != 0 && *tmp == ',')
			++tmp;

		tmpend = strchr(tmp, ',');
		if (tmpend != NULL)
		{
			memcpy(url, tmp, (tmpend - tmp));
			url[(tmpend - tmp)] = 0;
		}
		else
		{
			// Strip the ending newline
			tmpend = tmp;
			while (*tmpend && *tmpend != '\n')
				tmpend++;

			if ((tmpend - tmp) > SRB_CDMI_URL_SIZE)
			{
				SRB_LOG_ERR(srb_log, "Url too big: '%s'", tmp);
				ret = -EINVAL;
				goto end;
			}
			memcpy(url, tmp, (tmpend - tmp));
			url[(tmpend - tmp)] = 0;
			tmpend = NULL;
		}

		ret = srb_server_remove(url);
		if (ret < 0)
		{
			goto end;
		}

		tmp = tmpend;

	}

	ret = count;

end:
	return ret;
}

static ssize_t class_srb_urls_show(struct class *c, struct class_attribute *attr,
				       char *buf)
{
	ssize_t	ret = 0;

	ret = srb_servers_dump(buf, PAGE_SIZE);

	return ret;
}

void srb_sysfs_device_init(struct srb_device *dev)
{
        struct gendisk *disk = srb_device_get_disk(dev);

	device_create_file(disk_to_dev(disk), &dev_attr_srb_debug);
	device_create_file(disk_to_dev(disk), &dev_attr_srb_urls);
	device_create_file(disk_to_dev(disk), &dev_attr_srb_name);
}

static struct class_attribute class_srb_attrs[] = {
	__ATTR(attach,		0600, class_srb_attach_show, class_srb_attach_store),
	__ATTR(detach,		0600, class_srb_detach_show, class_srb_detach_store),
	__ATTR(add_urls,	0600, class_srb_addurl_show, class_srb_addurl_store),
	__ATTR(remove_urls,	0600, class_srb_removeurl_show, class_srb_removeurl_store),
	__ATTR(urls,		0400, class_srb_urls_show, NULL),
	__ATTR_NULL
};

int srb_sysfs_init(void)
{
	int ret = 0;

	/*
	 * create control files in sysfs
	 * /sys/class/srb/...
	 */
	/* TODO: check for class_create() from device.h
	 */
	class_srb = kzalloc(sizeof(*class_srb), GFP_KERNEL);
	if (!class_srb) {
		SRB_LOG_CRIT(srb_log, "Failed to allocate memory for sysfs class registration");
		return -ENOMEM;
	}
	class_srb->name	  = DEV_NAME;
	class_srb->owner	  = THIS_MODULE;
	class_srb->class_release = class_srb_release;
	class_srb->class_attrs   = class_srb_attrs;

	ret = class_register(class_srb);
	if (ret) {
		if (class_srb != NULL)
			kfree(class_srb);
		class_srb = NULL;
		SRB_LOG_CRIT(srb_log, "Failed to create class srb");
		return ret;
	}

	return 0;
}

void srb_sysfs_cleanup(void)
{
	/* TODO: check for class_unregister from device.h
	 */
	if (class_srb)
		class_destroy(class_srb);
	class_srb = NULL;
}
