# OpenWrt 软件包（用于 SDK 编译）
#
# 用法：在仓库里执行
#   ./scripts/build_openwrt.sh /path/to/openwrt-sdk
# 脚本会把 ../../src 拷成 ./src 后整体放进 SDK 的 package/ 下编译。

include $(TOPDIR)/rules.mk

PKG_NAME:=cnkrouter
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/cnkrouter
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Campus portal (Dr.COM) auto-login client
  URL:=https://github.com/LunaRime/CampusNet-Keeper
  MAINTAINER:=CampusNet-Keeper
  DEPENDS:=
endef

define Package/cnkrouter/description
  Router-side campus portal client. Logs the router itself in so that every
  device behind NAT shares one authorised session. About 40 KB on flash and
  ~1.5 MB RAM; no python/openssl/curl dependency.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/cnk.h $(PKG_BUILD_DIR)/
	$(CP) ./src/crypto.c $(PKG_BUILD_DIR)/
	$(CP) ./src/net.c $(PKG_BUILD_DIR)/
	$(CP) ./src/portal.c $(PKG_BUILD_DIR)/
	$(CP) ./src/main.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -std=c99 -o $(PKG_BUILD_DIR)/cnkrouter \
		$(PKG_BUILD_DIR)/crypto.c \
		$(PKG_BUILD_DIR)/net.c \
		$(PKG_BUILD_DIR)/portal.c \
		$(PKG_BUILD_DIR)/main.c \
		$(TARGET_LDFLAGS)
	$(STRIP) $(PKG_BUILD_DIR)/cnkrouter
endef

define Package/cnkrouter/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cnkrouter $(1)/usr/bin/cnkrouter
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/cnkrouter $(1)/etc/init.d/cnkrouter
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/cnkrouter $(1)/etc/config/cnkrouter
	$(INSTALL_DIR) $(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) ./files/etc/hotplug.d/iface/90-cnkrouter $(1)/etc/hotplug.d/iface/90-cnkrouter
endef

$(eval $(call BuildPackage,cnkrouter))
