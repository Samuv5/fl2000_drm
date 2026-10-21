obj-m := fl2000.o
fl2000-y := fl2000_drv.o fl2000_regs.o fl2000_it66121.o fl2000_modeset.o fl2000_stream.o

# Kernel API feature detection, so one source tree builds on LTS and on the newest kernels
ifneq ($(shell grep -s '^struct drm_atomic_commit {' $(srctree)/include/drm/drm_atomic.h),)
  ccflags-y += -DFL2000_HAVE_DRM_ATOMIC_COMMIT
endif
