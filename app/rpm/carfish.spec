Name:       carfish
Summary:    Android Auto projection for Sailfish OS
Version:    0.3.0
Release:    1
License:    GPLv3
URL:        https://example.org/carfish
Source0:    %{name}-%{version}.tar.bz2
Requires:   sailfishsilica-qt5 >= 0.10.9
Requires:   bluez5
Requires:   connman
Requires:   gstreamer1.0-plugins-base
Requires:   gstreamer1.0-plugins-good
# Native map pane: QtLocation OSM tiles + GPS via geoclue (Jolla repo packages).
Requires:   qt5-qtlocation
Requires:   qt5-qtdeclarative-import-location
Requires:   qt5-qtdeclarative-import-positioning
Requires:   qt5-plugin-geoservices-osm
Requires:   qt5-plugin-position-geoclue
Recommends: geoclue-provider-hybris-binder
# Phone (dialer) pane: contacts + recent calls + the voicecall-manager daemon.
Requires:   voicecall-qt5
Requires:   libcommhistory-qt5
Requires:   qtcontacts-sqlite-qt5
# carfish-helper reads the (privileged) contacts DB via QSqlDatabase/SQLite.
Requires:   qt5-plugin-sqldriver-sqlite
# hardware H.264 encoder (droidvenc) on real devices
Recommends: gstreamer1.0-droid
BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(protobuf-lite)
BuildRequires:  protobuf-compiler
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0)
BuildRequires:  pkgconfig(gstreamer-video-1.0)
BuildRequires:  pkgconfig(glesv2)
# Phone pane backends: QtContacts (address book) + libcommhistory (call log).
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(commhistory-qt5)
BuildRequires:  desktop-file-utils
BuildRequires:  cmake
# For building the bundled openh264 static library (codec/ sources, NEON asm).
BuildRequires:  make
BuildRequires:  gcc-c++

%description
Projects a car-optimised Sailfish OS UI to an Android Auto head unit over
USB (Android Open Accessory) or wireless (Bluetooth RFCOMM + WiFi + TCP).

%prep
%setup -q -n %{name}-%{version}

%build
%cmake
%make_build

%install
%make_install

desktop-file-install --delete-original \
  --dir %{buildroot}%{_datadir}/applications \
   %{buildroot}%{_datadir}/applications/*.desktop

# Install the AOA udev rule where udev actually reads it. On this device udev
# scans /lib/udev/rules.d (a real directory, NOT a symlink to /usr/lib) and
# ignores /usr/lib/udev/rules.d, so the rule must go in /lib. The name sorts
# after the stock /lib/udev/rules.d/999-android-system.rules so our MODE wins.
install -D -m 0644 %{buildroot}%{_datadir}/%{name}/config/999-carfish-aoa.rules \
   %{buildroot}/lib/udev/rules.d/999-carfish-aoa.rules

# Install the connman D-Bus policy so the app may join the head unit's WiFi.
install -D -m 0644 %{buildroot}%{_datadir}/%{name}/config/carfish-dbus.conf \
   %{buildroot}%{_datadir}/dbus-1/system.d/carfish.conf

# Privileged WiFi/routing helper: D-Bus policy, bus-activation and systemd unit.
install -D -m 0644 %{buildroot}%{_datadir}/%{name}/config/carfish-helper-dbus.conf \
   %{buildroot}%{_datadir}/dbus-1/system.d/carfish-helper.conf
install -D -m 0644 %{buildroot}%{_datadir}/%{name}/config/org.carfish.Helper.service \
   %{buildroot}%{_datadir}/dbus-1/system-services/org.carfish.Helper.service
install -D -m 0644 %{buildroot}%{_datadir}/%{name}/config/carfish-helper.service \
   %{buildroot}%{_unitdir}/carfish-helper.service

# User units: open Carfish when a USB cable is plugged in (enabled from the
# app's "Start when a USB cable is plugged in" setting).
install -D -m 0644 %{buildroot}%{_datadir}/%{name}/config/carfish-usb-autostart.path \
   %{buildroot}/usr/lib/systemd/user/carfish-usb-autostart.path
install -D -m 0644 %{buildroot}%{_datadir}/%{name}/config/carfish-usb-autostart.service \
   %{buildroot}/usr/lib/systemd/user/carfish-usb-autostart.service

%post
# Load the new udev rule and apply it to an already-present accessory node so
# wired Android Auto works without replugging.
udevadm control --reload-rules >/dev/null 2>&1 || :
udevadm trigger --subsystem-match=misc >/dev/null 2>&1 || :
# Make the system bus pick up the new D-Bus policies without a reboot.
dbus-send --system --type=method_call --dest=org.freedesktop.DBus \
    / org.freedesktop.DBus.ReloadConfig >/dev/null 2>&1 || :
# Enable and (re)start the privileged helper so wireless works on a stock
# Sailfish OS without putting the device user in the 'privileged' group.
systemctl daemon-reload >/dev/null 2>&1 || :
systemctl enable carfish-helper.service >/dev/null 2>&1 || :
systemctl restart carfish-helper.service >/dev/null 2>&1 || :

%preun
if [ "$1" = 0 ]; then
    systemctl disable --now carfish-helper.service >/dev/null 2>&1 || :
fi

%postun
if [ "$1" = 0 ]; then
    udevadm control --reload-rules >/dev/null 2>&1 || :
    systemctl daemon-reload >/dev/null 2>&1 || :
    dbus-send --system --type=method_call --dest=org.freedesktop.DBus \
        / org.freedesktop.DBus.ReloadConfig >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_bindir}/%{name}-helper
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%{_datadir}/dbus-1/system.d/carfish-helper.conf
%{_datadir}/dbus-1/system-services/org.carfish.Helper.service
%{_unitdir}/carfish-helper.service
/usr/lib/systemd/user/carfish-usb-autostart.path
/usr/lib/systemd/user/carfish-usb-autostart.service
/lib/udev/rules.d/999-carfish-aoa.rules
%{_datadir}/dbus-1/system.d/carfish.conf
