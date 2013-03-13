Name:       pims-ipc
Summary:    library for PIMs IPC
Version:    0.0.22
Release:    1
Group:      System/Libraries
License:    Apache-2.0
Source0:    %{name}-%{version}.tar.gz

BuildRequires: cmake
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(libsystemd-daemon)
BuildRequires: pkgconfig(libzmq)

%description
library for PIMs IPC

%package devel
Summary:    DB library for calendar
Group:      Development/Libraries
Requires:   %{name} = %{version}

%description devel
library for PIMs IPC (developement files)

%prep
%setup -q


%build
%cmake .
make %{?jobs:-j%jobs}

%install
%make_install


%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig


%files
%manifest pims-ipc.manifest
%defattr(-,root,root,-)
%{_libdir}/libpims-ipc.so.*

%files devel
%defattr(-,root,root,-)
%{_includedir}/pims-ipc/*.h
%{_libdir}/*.so
%{_libdir}/pims_ipc_test
%{_libdir}/pkgconfig/pims-ipc.pc
