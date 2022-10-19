%global giturl  https://github.com/cboxdoerfer/fsearch

Name:    fsearch
Summary: A fast file search utility for Unix-like systems based on GTK 3
Epoch:   1
Version: 0.2
Release: 1%{?dist}
License: GPLv2+
URL:     https://github.com/cboxdoerfer/fsearch
Source0: %{giturl}/archive/%{version}/%{name}-%{version}.tar.gz


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
%setup -q -n fsearch-%{version} -c

mv fsearch-%{version} build

%build
export LDFLAGS="%{?__global_ldflags} -pthread"
pushd build
%meson
%meson_build -v
popd

%install
pushd build
%meson_install

desktop-file-install \
  --dir=%{buildroot}%{_datadir}/applications/ \
  %{buildroot}%{_datadir}/applications/io.github.cboxdoerfer.FSearch.desktop

%files
%{_bindir}/fsearch
%{_datadir}/applications/io.github.cboxdoerfer.FSearch.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.cboxdoerfer.FSearch.svg
%{_datadir}/man/man1/fsearch.1.gz
%{_datadir}/metainfo/io.github.cboxdoerfer.FSearch.metainfo.xml
%{_datadir}/locale/*/*/fsearch.mo

