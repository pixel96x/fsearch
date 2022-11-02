%global giturl  https://github.com/cboxdoerfer/fsearch

Name:    fsearch
Summary: A fast file search utility for Unix-like systems based on GTK 3
Epoch:   2
Version: 0.3~alpha0
Release: %(date +%%Y%%m%%d)%{?dist}
License: GPLv2+
URL:     https://github.com/cboxdoerfer/fsearch
Source0: %{giturl}/archive/master/%{name}-master.tar.gz


BuildRequires: meson
BuildRequires: ninja-build
BuildRequires: gcc
BuildRequires: gtk3-devel
BuildRequires: glib2-devel
BuildRequires: libappstream-glib
BuildRequires: desktop-file-utils


%description
FSearch is a fast file search utility, inspired by Everything Search Engine. It's written in C and based on GTK 3.

%prep
%setup -q -n fsearch-master -c

mv fsearch-master build

%build
export LDFLAGS="%{?__global_ldflags} -pthread"
pushd build
%meson -Dchannel=copr-nightly
%meson_build -v
popd

%install
pushd build
%meson_install
popd

%find_lang %{name} --with-gnome

desktop-file-install \
  --dir=%{buildroot}%{_datadir}/applications/ \
  %{buildroot}%{_datadir}/applications/io.github.cboxdoerfer.FSearch.desktop

%files -f %{name}.lang
%{_bindir}/fsearch
%{_datadir}/applications/io.github.cboxdoerfer.FSearch.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.cboxdoerfer.FSearch.svg
%{_datadir}/man/man1/fsearch.1.gz
%{_datadir}/metainfo/io.github.cboxdoerfer.FSearch.metainfo.xml
%{_datadir}/locale/*/*/fsearch.mo

