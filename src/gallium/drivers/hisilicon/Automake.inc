if HAVE_GALLIUM_HISILICON

TARGET_DRIVERS += hisilicon
TARGET_CPPFLAGS += -DGALLIUM_HISILICON
TARGET_LIB_DEPS += \
    $(top_builddir)/src/gallium/winsys/hisilicon/drm/libhisilicondrm.la \
    $(LIBDRM_LIBS)

endif
