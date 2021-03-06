/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 6WIND S.A.
 * Copyright 2017 Mellanox.
 */

#include <rte_malloc.h>

#include "failsafe_private.h"

static int
fs_ethdev_portid_get(const char *name, uint16_t *port_id)
{
	uint16_t pid;
	size_t len;

	if (name == NULL) {
		DEBUG("Null pointer is specified\n");
		return -EINVAL;
	}
	len = strlen(name);
	RTE_ETH_FOREACH_DEV(pid) {
		if (!strncmp(name, rte_eth_devices[pid].device->name, len)) {
			*port_id = pid;
			return 0;
		}
	}
	return -ENODEV;
}

static int
fs_bus_init(struct rte_eth_dev *dev)
{
	struct sub_device *sdev;
	struct rte_devargs *da;
	uint8_t i;
	uint16_t pid;
	int ret;

	FOREACH_SUBDEV(sdev, i, dev) {
		if (sdev->state != DEV_PARSED)
			continue;
		da = &sdev->devargs;
		if (fs_ethdev_portid_get(da->name, &pid) != 0) {
			ret = rte_eal_hotplug_add(da->bus->name,
						  da->name,
						  da->args);
			if (ret) {
				ERROR("sub_device %d probe failed %s%s%s", i,
				      rte_errno ? "(" : "",
				      rte_errno ? strerror(rte_errno) : "",
				      rte_errno ? ")" : "");
				continue;
			}
			if (fs_ethdev_portid_get(da->name, &pid) != 0) {
				ERROR("sub_device %d init went wrong", i);
				return -ENODEV;
			}
		} else {
			char devstr[DEVARGS_MAXLEN] = "";
			struct rte_devargs *probed_da =
					rte_eth_devices[pid].device->devargs;

			/* Take control of device probed by EAL options. */
			free(da->args);
			memset(da, 0, sizeof(*da));
			if (probed_da != NULL)
				snprintf(devstr, sizeof(devstr), "%s,%s",
					 probed_da->name, probed_da->args);
			else
				snprintf(devstr, sizeof(devstr), "%s",
					 rte_eth_devices[pid].device->name);
			ret = rte_eal_devargs_parse(devstr, da);
			if (ret) {
				ERROR("Probed devargs parsing failed with code"
				      " %d", ret);
				return ret;
			}
			INFO("Taking control of a probed sub device"
			      " %d named %s", i, da->name);
		}
		ret = rte_eth_dev_owner_set(pid, &PRIV(dev)->my_owner);
		if (ret < 0) {
			INFO("sub_device %d owner set failed (%s),"
			     " will try again later", i, strerror(-ret));
			continue;
		} else if (strncmp(rte_eth_devices[pid].device->name, da->name,
			   strlen(da->name)) != 0) {
			/*
			 * The device probably was removed and its port id was
			 * reallocated before ownership set.
			 */
			rte_eth_dev_owner_unset(pid, PRIV(dev)->my_owner.id);
			INFO("sub_device %d was probably removed before taking"
			     " ownership, will try again later", i);
			continue;
		}
		ETH(sdev) = &rte_eth_devices[pid];
		SUB_ID(sdev) = i;
		sdev->fs_dev = dev;
		sdev->dev = ETH(sdev)->device;
		ETH(sdev)->state = RTE_ETH_DEV_DEFERRED;
		sdev->state = DEV_PROBED;
	}
	return 0;
}

int
failsafe_eal_init(struct rte_eth_dev *dev)
{
	int ret;

	ret = fs_bus_init(dev);
	if (ret)
		return ret;
	if (PRIV(dev)->state < DEV_PROBED)
		PRIV(dev)->state = DEV_PROBED;
	fs_switch_dev(dev, NULL);
	return 0;
}

static int
fs_bus_uninit(struct rte_eth_dev *dev)
{
	struct sub_device *sdev = NULL;
	uint8_t i;
	int sdev_ret;
	int ret = 0;

	FOREACH_SUBDEV_STATE(sdev, i, dev, DEV_PROBED) {
		sdev_ret = rte_eal_hotplug_remove(sdev->bus->name,
							sdev->dev->name);
		if (sdev_ret) {
			ERROR("Failed to remove requested device %s (err: %d)",
			      sdev->dev->name, sdev_ret);
			continue;
		}
		sdev->state = DEV_PROBED - 1;
	}
	return ret;
}

int
failsafe_eal_uninit(struct rte_eth_dev *dev)
{
	int ret;

	ret = fs_bus_uninit(dev);
	PRIV(dev)->state = DEV_PROBED - 1;
	return ret;
}
