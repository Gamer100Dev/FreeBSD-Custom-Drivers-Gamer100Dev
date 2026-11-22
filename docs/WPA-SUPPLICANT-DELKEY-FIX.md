# FreeBSD wpa_supplicant DELKEY cleanup compatibility

## Diagnosis

The startup message

```
ioctl[SIOCS80211, op=20, val=0, arg_len=7]: Invalid argument
```

is a FreeBSD `IEEE80211_IOC_DELKEY` request. `arg_len=7` is the expected
size of `struct ieee80211req_del_key`; the request is not malformed because
of its length.

FreeBSD 15 net80211 accepts global/WEP key indices 0 through 3
(`IEEE80211_WEP_NKID == 4`). Its `ieee80211_ioctl_delkey()` returns
`EINVAL` when a global key-delete request contains an index greater than
or equal to 4. The BSD wpa_supplicant backend currently forwards every
cleanup index without checking that limit. Startup key cleanup can therefore
print this error for unused key slots.

This is a wpa_supplicant BSD-backend compatibility issue, not an
RTL8822BU/rtw88 ioctl implementation issue: the rejection occurs in the
generic net80211 ioctl handler before the device driver is called.

## Fix

Apply
`patches/wpa_supplicant/0001-freebsd-ignore-invalid-global-key-cleanup.patch`
to the FreeBSD source tree. It makes the BSD backend treat an out-of-range
global key deletion as a successful no-op on FreeBSD. Valid group-key deletes
(0..3), and pairwise key deletion using `IEEE80211_KEYIX_NONE`, still go
through the original ioctl path.

## Apply and rebuild the base-system wpa_supplicant

First confirm that the executable in use is the base-system binary:

```sh
which -a wpa_supplicant
wpa_supplicant -v
```

For the FreeBSD source tree at `/usr/src`:

```sh
sudo patch -C -p0 -d /usr/src < /root/FreeBSD-Custom-Drivers-Gamer100Dev/patches/wpa_supplicant/0001-freebsd-ignore-invalid-global-key-cleanup.patch
sudo patch -p0 -d /usr/src < /root/FreeBSD-Custom-Drivers-Gamer100Dev/patches/wpa_supplicant/0001-freebsd-ignore-invalid-global-key-cleanup.patch

cd /usr/src/usr.sbin/wpa/wpa_supplicant
sudo make
sudo make install
```

Do not apply the patch twice. If the check reports that the hunk is already
applied or reversed, inspect
`/usr/src/contrib/wpa/src/drivers/driver_bsd.c` before continuing.

If `which -a` shows that the active binary is under `/usr/local/sbin`,
it is a ports/package build; rebuild that wpa_supplicant from its matching
ports source with the same one-function change instead of replacing the
base-system executable.

After installing the corrected binary, restart wpa_supplicant. The two
startup DELKEY warnings should disappear. This fix does not change the
RTL8822BU kernel module or its key installation/CCMP datapath.
