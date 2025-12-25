EAPI=8
inherit git-r3

DESCRIPTION="ProfiWeb Cloud RMI C server/client/proxy implementation"
HOMEPAGE="https://git.profiweb.biz/profiweb/cloud-rmi"
EGIT_REPO_URI="file:///var/share/mirrors/cloud-rmi.git"
EGIT_COMMIT="v${PV}"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="amd64"
IUSE="+server +client proxy php linux debug"

DEPEND="
    sys-libs/glibc
    php? ( >=dev-lang/php-8.3:* )
"
RDEPEND="${DEPEND}"

src_compile() {
	local cppflags=()
	use linux && cppflags+=(-DENABLE_LINUX_FEATURES)
	use debug && cppflags+=(-DENABLE_DEBUG_LOGGING)

	use server && emake CPPFLAGS="${cppflags[*]}" server
	use client && emake CPPFLAGS="${cppflags[*]}" client
	use proxy  && emake CPPFLAGS="${cppflags[*]}" proxy

	use php && emake php
}

src_install() {
	use server && emake DESTDIR="${D}" server_install
	use client && emake DESTDIR="${D}" client_install
	use proxy  && emake DESTDIR="${D}" proxy_install

	newinitd "${FILESDIR}/cloud-rmi.initd" cloud-rmi

	use php && emake -C php/ext/profiweb_cloudrmi \
            INSTALL_ROOT="${D}" \
            install
}

pkg_postinst() {
    if use php; then
        elog
        elog "The PHP extension 'profiweb_cloudrmi' has been installed."
        elog
        elog "Do not forget to enable it in your PHP configuration"
        elog "for the appropriate SAPI (CLI/FPM/Apache), and reload"
        elog "the corresponding PHP service (e.g. php-fpm)."
        elog
    fi
}
