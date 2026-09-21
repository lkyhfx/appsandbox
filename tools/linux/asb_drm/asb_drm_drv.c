// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * AppSandbox virtual DRM driver — module entry point.
 *
 * Lifecycle:
 *   module_init() registers a platform_driver and creates a single
 *   platform_device. Match triggers probe() which builds the DRM device,
 *   adds the CRTC / connector / planes via the per-subsystem init helpers,
 *   and registers with the DRM core. /dev/dri/cardN appears at that point.
 *
 * We deliberately use platform_bus (not faux_bus) because some Wayland
 * compositors filter virtual KMS devices that live under /devices/faux/.
 * The platform bus puts us at /devices/platform/asb_drm.0/drm/cardN, which
 * is indistinguishable from any other in-tree platform-bus DRM driver from
 * userspace's point of view.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/version.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "asb_drm.h"

/* --------------------------------------------------------------------------
 * Module parameters
 * -------------------------------------------------------------------------- */

static unsigned int width_param   = ASB_DEFAULT_WIDTH;
static unsigned int height_param  = ASB_DEFAULT_HEIGHT;
static unsigned int refresh_param = ASB_DEFAULT_REFRESH;
static char profile_param[8]      = "720p";

module_param_named(width,   width_param,   uint, 0444);
MODULE_PARM_DESC(width,   "Initial display width  (default 1280)");
module_param_named(height,  height_param,  uint, 0444);
MODULE_PARM_DESC(height,  "Initial display height (default 720)");
module_param_named(refresh, refresh_param, uint, 0444);
MODULE_PARM_DESC(refresh, "Refresh rate in Hz     (default 60)");
module_param_string(profile, profile_param, sizeof(profile_param), 0444);
MODULE_PARM_DESC(profile, "Display profile: 720p (default), 1080p, or 4k (3840x2160@60)");

void asb_mode_snapshot(struct asb_device *asb, unsigned int *width,
                       unsigned int *height, unsigned int *refresh,
                       bool *runtime_mode)
{
	mutex_lock(&asb->mode_lock);
	if (width) *width = asb->width;
	if (height) *height = asb->height;
	if (refresh) *refresh = asb->refresh;
	if (runtime_mode) *runtime_mode = asb->runtime_mode;
	mutex_unlock(&asb->mode_lock);
}

int asb_mode_set_runtime(struct asb_device *asb, unsigned int width,
                         unsigned int height, unsigned int refresh)
{
	bool changed;

	if (width < ASB_MIN_WIDTH || width > ASB_MAX_WIDTH ||
	    height < ASB_MIN_HEIGHT || height > ASB_MAX_HEIGHT ||
	    refresh < 24 || refresh > 240)
		return -EINVAL;
	/* Keep direct sysfs writers on the same boundaries as the display
	 * control protocol. The guest agent normally performs this normalization
	 * before reaching sysfs, but the kernel entry point must be safe alone. */
	width = width > ASB_MAX_WIDTH - 7u ? ASB_MAX_WIDTH : (width + 7u) & ~7u;
	height = height > ASB_MAX_HEIGHT - 1u ? ASB_MAX_HEIGHT : (height + 1u) & ~1u;

	mutex_lock(&asb->mode_lock);
	changed = !asb->runtime_mode || asb->width != width ||
	          asb->height != height || asb->refresh != refresh;
	asb->width = width;
	asb->height = height;
	asb->refresh = refresh;
	asb->runtime_mode = true;
	WRITE_ONCE(asb->vblank_period, ns_to_ktime(NSEC_PER_SEC / refresh));
	mutex_unlock(&asb->mode_lock);

	if (!changed)
		return 0;

	/* Publish the new preferred mode before asking userspace to reprobe. */
	asb_build_edid(asb);
	drm_kms_helper_hotplug_event(&asb->drm);
	return 0;
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
                         char *buf)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct asb_device *asb = to_asb(drm);
	unsigned int width, height, refresh;

	asb_mode_snapshot(asb, &width, &height, &refresh, NULL);
	return sysfs_emit(buf, "%ux%u@%u\n", width, height, refresh);
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
                          const char *buf, size_t count)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct asb_device *asb = to_asb(drm);
	char mode[32];
	char *p, *x, *at;
	unsigned int width, height, refresh;
	int ret;

	if (count == 0 || count >= sizeof(mode))
		return -EINVAL;
	memcpy(mode, buf, count);
	mode[count] = '\0';
	p = strim(mode);
	x = strchr(p, 'x');
	at = x ? strchr(x + 1, '@') : NULL;
	if (!x || !at || x == p || at == x + 1 || !at[1])
		return -EINVAL;
	*x = '\0';
	*at = '\0';
	ret = kstrtouint(p, 10, &width);
	if (ret) return ret;
	ret = kstrtouint(x + 1, 10, &height);
	if (ret) return ret;
	ret = kstrtouint(at + 1, 10, &refresh);
	if (ret) return ret;
	ret = asb_mode_set_runtime(asb, width, height, refresh);
	if (ret) return ret;
	dev_info(dev, "runtime_mode=%ux%u@%uHz\n", width, height, refresh);
	return count;
}

static DEVICE_ATTR_RW(mode);

/* --------------------------------------------------------------------------
 * drm_driver
 * -------------------------------------------------------------------------- */

DEFINE_DRM_GEM_FOPS(asb_fops);

static const struct drm_driver asb_drm_driver = {
	/* We deliberately do NOT set DRIVER_CURSOR_HOTSPOT. That flag tells
	 * the kernel to expose HOTSPOT_X/HOTSPOT_Y properties on the cursor
	 * plane and to hide the cursor plane from any DRM client that hasn't
	 * opted in via DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT. Mutter only sets
	 * that client cap for a hardcoded allowlist of driver names —
	 * "qxl", "vboxvideo", "virtio_gpu", "vmwgfx" — so if we advertise
	 * the flag, Mutter never sees our cursor plane and falls back to
	 * software cursor (rendered into the primary framebuffer). Without
	 * the flag, Mutter handles hotspot itself: it pre-adjusts the
	 * cursor plane's CRTC_X/CRTC_Y by -hotspot before the atomic commit.
	 * Our daemon reads the pre-adjusted coordinates directly, which is
	 * exactly what the host renderer expects.
	 *
	 * DRIVER_RENDER exposes a render-node (/dev/dri/renderDN) and lets
	 * Mutter create a gbm_device for buffer allocation on our fd. Mutter
	 * tries the GBM-allocated cursor path first; with no Mesa driver
	 * the GBM cursor check fails, but Mutter retries with NULL gbm_device
	 * which only checks that a cursor plane advertises the format. */
	.driver_features    = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC |
	                      DRIVER_RENDER,

	.name               = "asb_drm",
	.desc               = "AppSandbox virtual display",
	.major              = 1,
	.minor              = 0,

	.fops               = &asb_fops,

	/* Memory: use the kernel's shmem-backed GEM helpers. We don't need
	 * GPU memory of our own — the compositor allocates buffers (either
	 * shmem via dumb_create, or imported dma-buf from Mesa-d3d12). All
	 * mapping / mmap / fault paths come from these macros. */
	DRM_GEM_SHMEM_DRIVER_OPS,
};

/* --------------------------------------------------------------------------
 * mode_config — top-level limits and atomic-helper callbacks
 * -------------------------------------------------------------------------- */

static const struct drm_mode_config_funcs asb_mode_config_funcs = {
	.fb_create     = drm_gem_fb_create,
	.atomic_check  = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int asb_mode_config_setup(struct asb_device *asb)
{
	struct drm_device *drm = &asb->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width   = ASB_MIN_WIDTH;
	drm->mode_config.min_height  = ASB_MIN_HEIGHT;
	drm->mode_config.max_width   = ASB_MAX_WIDTH;
	drm->mode_config.max_height  = ASB_MAX_HEIGHT;
	drm->mode_config.cursor_width  = ASB_CURSOR_MAX_W;
	drm->mode_config.cursor_height = ASB_CURSOR_MAX_H;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.prefer_shadow   = 0;
	drm->mode_config.funcs = &asb_mode_config_funcs;
	return 0;
}

/* --------------------------------------------------------------------------
 * platform_driver — probe / remove
 * -------------------------------------------------------------------------- */

static int asb_probe(struct platform_device *pdev)
{
	struct asb_device *asb;
	struct drm_device *drm;
	int ret;

	/* simpledrm displacement is handled by a userland systemd unit
	 * (asb-evict-simpledrm.service) that runs before display-manager
	 * and unbinds the simple-framebuffer platform driver. Tried the
	 * kernel-side aperture_remove_all_conflicting_devices() route first
	 * but it doesn't evict simpledrm on this kernel because simpledrm
	 * doesn't register through the aperture registry — its platform
	 * device is created by sysfb during early boot and stays bound. */

	asb = devm_drm_dev_alloc(&pdev->dev, &asb_drm_driver,
	                         struct asb_device, drm);
	if (IS_ERR(asb))
		return PTR_ERR(asb);

	drm = &asb->drm;
	platform_set_drvdata(pdev, drm);

	mutex_init(&asb->mode_lock);
	asb->runtime_mode = false;

	/* Clamp module params into the supported range. Named profiles remain
	 * compatible, while the no-profile default is now 1280x720. */
	if (!strcmp(profile_param, "4k")) {
		asb->width = ASB_4K_WIDTH;
		asb->height = ASB_4K_HEIGHT;
		asb->refresh = ASB_4K_REFRESH;
	} else if (!strcmp(profile_param, "1080p")) {
		asb->width = 1920;
		asb->height = 1080;
		asb->refresh = ASB_DEFAULT_REFRESH;
	} else {
		asb->width   = clamp(width_param,   (unsigned)ASB_MIN_WIDTH, (unsigned)ASB_MAX_WIDTH);
		asb->height  = clamp(height_param,  (unsigned)ASB_MIN_HEIGHT, (unsigned)ASB_MAX_HEIGHT);
		asb->refresh = clamp(refresh_param, 24u, 240u);
	}

	ret = asb_mode_config_setup(asb);
	if (ret) {
		dev_err(&pdev->dev, "mode_config init failed: %d\n", ret);
		return ret;
	}

	/* Plane init MUST happen before CRTC init: drm_crtc_init_with_planes
	 * takes the primary plane as a constructor argument. */
	ret = asb_planes_init(asb);
	if (ret) {
		dev_err(&pdev->dev, "planes init failed: %d\n", ret);
		return ret;
	}

	ret = asb_mode_init(asb);
	if (ret) {
		dev_err(&pdev->dev, "mode init failed: %d\n", ret);
		return ret;
	}

	ret = asb_connector_init(asb);
	if (ret) {
		dev_err(&pdev->dev, "connector init failed: %d\n", ret);
		return ret;
	}

	ret = asb_writeback_init(asb);
	if (ret) {
		/* Writeback is optional — log and continue. */
		dev_warn(&pdev->dev, "writeback init failed (%d), continuing without\n", ret);
	}

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(&pdev->dev, "drm_dev_register failed: %d\n", ret);
		return ret;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_mode);
	if (ret) {
		dev_err(&pdev->dev, "mode sysfs attribute failed: %d\n", ret);
		drm_dev_unregister(drm);
		return ret;
	}

	dev_info(&pdev->dev, "AppSandbox virtual display ready: %ux%u@%uHz\n",
	         asb->width, asb->height, asb->refresh);
	return 0;
}

static void asb_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct asb_device *asb = to_asb(drm);

	device_remove_file(&pdev->dev, &dev_attr_mode);
	drm_dev_unplug(drm);
	asb_mode_fini(asb);
	drm_atomic_helper_shutdown(drm);
}

static struct platform_driver asb_platform_driver = {
	.driver = {
		.name = "asb_drm",
	},
	.probe  = asb_probe,
	.remove = asb_remove,
};

/* --------------------------------------------------------------------------
 * Module init / exit. Manually create one platform_device so the driver
 * has something to bind to (we're not enumerated by ACPI/DT/PCI/anything).
 * -------------------------------------------------------------------------- */

static struct platform_device *asb_platform_device;

static int __init asb_drm_init(void)
{
	int ret;

	ret = platform_driver_register(&asb_platform_driver);
	if (ret)
		return ret;

	asb_platform_device = platform_device_register_simple("asb_drm", 0, NULL, 0);
	if (IS_ERR(asb_platform_device)) {
		platform_driver_unregister(&asb_platform_driver);
		return PTR_ERR(asb_platform_device);
	}
	return 0;
}

static void __exit asb_drm_exit(void)
{
	platform_device_unregister(asb_platform_device);
	platform_driver_unregister(&asb_platform_driver);
}

module_init(asb_drm_init);
module_exit(asb_drm_exit);

MODULE_AUTHOR("AppSandbox");
MODULE_DESCRIPTION("AppSandbox virtual DRM/KMS driver");
/* Source is MIT (see SPDX header). MODULE_LICENSE must declare GPL-compat
 * for the kernel loader to resolve EXPORT_SYMBOL_GPL DRM helpers. */
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("1.0.0");
MODULE_ALIAS("platform:asb_drm");
