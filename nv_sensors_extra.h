/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * nv_sensors_extra.h - internal declarations for the NVIDIA GDDR6 hwmon driver
 *
 * Reads GDDR6/GDDR6X memory and GPU hotspot temperatures from the PCI
 * BAR0 of supported NVIDIA GPUs. The GPU itself is owned by the
 * proprietary nvidia driver.
 */

#ifndef _NV_SENSORS_EXTRA_H
#define _NV_SENSORS_EXTRA_H

#include <linux/pci.h>
#include <linux/types.h>

struct nv_sensors_extra_entry {
	u16 device_id;
	u32 vram_offset;
	u32 hotspot_offset;
	const char *name;
	const char *vram;
	const char *arch;
};

/*
 * nv_sensors_extra_match() - find the device table entry for @device_id
 *
 * Return: a pointer to the matching entry, or NULL if the device is not
 * supported.
 */
const struct nv_sensors_extra_entry *nv_sensors_extra_match(u16 device_id);

/*
 * nv_sensors_extra_vram_temp() - convert a raw VRAM sensor register value to
 * millidegrees Celsius
 *
 * Return: temperature in millidegrees, or -ENODATA if @raw is an invalid
 * value (device in deep sleep, all zero or all one bits).
 */
long nv_sensors_extra_vram_temp(u32 raw);

/*
 * nv_sensors_extra_hotspot_temp() - convert a raw GPU hotspot sensor register
 * value to millidegrees Celsius
 *
 * Return: temperature in millidegrees, or -ENODATA if @raw is an invalid
 * value (device in deep sleep, all zero or all one bits).
 */
long nv_sensors_extra_hotspot_temp(u32 raw);

/*
 * nv_sensors_extra_bound_to_nvidia() - check whether @pdev is bound to the
 * proprietary nvidia driver
 *
 * Return: true if the device has a driver attached and that driver is
 * named "nvidia", false otherwise.
 */
bool nv_sensors_extra_bound_to_nvidia(const struct pci_dev *pdev);

#endif /* _NV_SENSORS_EXTRA_H */
