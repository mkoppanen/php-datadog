# Define version and release number
%define version @PACKAGE_VERSION@
%define release 1

Name:      php-pecl-datadog
Version:   %{version}
Release:   %{release}%{?dist}
Packager:  Mikko Koppanen <mikko@lentor.io>
Summary:   PHP datadog extension
License:   PHP License
Group:     Web/Applications
URL:       http://github.com/lentor/php-datadog
Source:    datadog-%{version}.tgz
Prefix:    %{_prefix}
Buildroot: %{_tmppath}/%{name}-%{version}-%{release}-root
BuildRequires: php-devel, make, gcc, /usr/bin/phpize

%description
Monitoring extension for PHP

%prep
%setup -q -n datadog-%{version}

%build
/usr/bin/phpize && %configure -C && %{__make} %{?_smp_mflags}

# Clean the buildroot so that it does not contain any stuff from previous builds
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

# Install the extension
%{__make} install INSTALL_ROOT=%{buildroot}

# Create the ini location
%{__mkdir} -p %{buildroot}/etc/php.d

# Preliminary extension ini
echo "extension=datadog.so" > %{buildroot}/%{_sysconfdir}/php.d/datadog.ini

%clean
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

%files
%{_libdir}/php/modules/datadog.so
%{_sysconfdir}/php.d/datadog.ini

%changelog
* Mon Apr 29 2013 Mikko Koppanen <mikko@lentor.io>
 - Initial spec file
