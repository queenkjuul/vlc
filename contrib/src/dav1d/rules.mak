# libdav1d

DAV1D_VERSION := 1.2.3
DAV1D_URL := $(VIDEOLAN)/dav1d/$(DAV1D_VERSION)/dav1d-$(DAV1D_VERSION).tar.xz

DAV1D_GITURL := https://code.videolan.org/Skantes/dav1d.git
DAV1D_HASH := 1cc5096ea1227336a08352edd59b7f71d4939bc0
DAV1D_BRANCH := test-patch

DEPS_dav1d = vulkan-headers $(DEPS_vulkan-headers)

PKGS += dav1d
ifeq ($(call need_pkg,"dav1d"),)
PKGS_FOUND += dav1d
endif

DAV1D_CONF = -D enable_tests=false -D enable_tools=false

$(TARBALLS)/dav1d-$(DAV1D_VERSION).tar.xz:
#   $(call download_pkg,$(DAV1D_URL),dav1d)
	$(call download_git_branch,$(DAV1D_GITURL),,$(DAV1D_HASH),${DAV1D_BRANCH})

.sum-dav1d: dav1d-$(DAV1D_VERSION).tar.xz
	$(call check_githash,$(DAV1D_HASH))
	touch $@

dav1d: dav1d-$(DAV1D_VERSION).tar.xz .sum-dav1d
	$(UNPACK)
	$(MOVE)

.dav1d: dav1d crossfile.meson
	cd $< && rm -rf ./build
	cd $< && $(HOSTVARS_MESON) $(MESON) $(DAV1D_CONF) build
	cd $< && cd build && ninja install
	touch $@
