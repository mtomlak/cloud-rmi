EAPI=8
inherit git-r3

DESCRIPTION="ProfiWeb Cloud RMI C server/client/proxy implementation"
HOMEPAGE="https://git.profiweb.biz/profiweb/cloud-rmi"
EGIT_REPO_URI="file:///var/share/mirrors/cloud-rmi.git"
EGIT_COMMIT="v${PV}"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="amd64"
IUSE="+server +client proxy linux_features"

DEPEND="sys-libs/glibc"
RDEPEND="${DEPEND}"

src_compile() {
    local mycppflags=""
    use linux_features && mycppflags="-DENABLE_LINUX_FEATURES"

    use server && emake CPPFLAGS="${mycppflags}" server
    use client && emake CPPFLAGS="${mycppflags}" client
    use proxy  && emake CPPFLAGS="${mycppflags}" proxy
}

src_install() {
    use server && emake DESTDIR="${D}" server_install
    use client && emake DESTDIR="${D}" client_install
    use proxy  && emake DESTDIR="${D}" proxy_install
}
